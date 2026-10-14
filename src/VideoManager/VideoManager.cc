#include "VideoManager.h"
#include "AppSettings.h"
#include "MavlinkCameraControlInterface.h"
#include "MultiVehicleManager.h"
#include "AppMessages.h"
#include "QGCApplication.h"
#include "QGCCameraManager.h"
#include "QGCCorePlugin.h"
#include "QGCLoggingCategory.h"
#include "QGCVideoStreamInfo.h"
#include "SettingsManager.h"
#include "SubtitleWriter.h"
#include "Vehicle.h"
#include "VehicleLinkManager.h"
#include "VideoReceiver.h"
#include "VideoSettings.h"
#include "UVCReceiver.h"
#include "VideoBackend.h"

#include <algorithm>
#include <climits>

#include <QtConcurrent/QtConcurrent>
#include <QtCore/QApplicationStatic>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFutureWatcher>
#include <QtCore/QRunnable>
#include <QtCore/QTimer>
#include <QtCore/QRegularExpression>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

QGC_LOGGING_CATEGORY(VideoManagerLog, "Video.VideoManager")

static constexpr const char *kFileExtension[VideoReceiver::FILE_FORMAT_MAX + 1] = {
    "mkv",
    "mov",
    "mp4"
};

// RSS (kB) at which streams are shed to stay clear of the vendor abnormal-memory killer.
// 400 MB proved too late on ColorOS/OxygenOS — the process was killed before the shed helped.
static constexpr int kRssShedSecondaryKb = 350 * 1024;
static constexpr int kRssShedAllKb      = 420 * 1024;

// Consecutive failed restarts allowed per receiver before we stop retrying. Bounds the stop/start
// churn that rebuilt the GStreamer pipeline (and Android MediaCodec) fast enough to grow the
// native heap until the OS killed us.
static constexpr int kMaxRestartAttempts = 8;

Q_APPLICATION_STATIC(VideoManager, _videoManagerInstance);

VideoManager::VideoManager(QObject *parent)
    : QObject(parent)
    , _subtitleWriter(new SubtitleWriter(this))
    , _videoSettings(SettingsManager::instance()->videoSettings())
{
    qCDebug(VideoManagerLog) << this;

    (void) qRegisterMetaType<VideoReceiver::STATUS>("STATUS");

    // Monitor low memory to prevent OOM kill. 2 s sampling: at 5 s a fast RSS climb (pipeline
    // rebuild storms on Android) could cover the gap to the ColorOS kill threshold between ticks.
    _lowMemoryWarningTimer = new QTimer(this);
    _lowMemoryWarningTimer->setInterval(2000);
    (void) connect(_lowMemoryWarningTimer, &QTimer::timeout, this, &VideoManager::_onLowMemory);
    _lowMemoryWarningTimer->start();

    if (VideoBackend::needsAsyncInit()) {
        _backendDisabledForTests = VideoBackend::disabledForUnitTests();
        if (_backendDisabledForTests) {
            qCInfo(VideoManagerLog) << "Skipping video backend initialization for unit tests";
        }
    }
}

VideoManager::~VideoManager()
{
    qCDebug(VideoManagerLog) << this;
}

VideoManager *VideoManager::instance()
{
    return _videoManagerInstance();
}

void VideoManager::startVideoBackendInit()
{
    if (!VideoBackend::needsAsyncInit()) return;

    if (_backendDisabledForTests) {
        _initState.store(InitState::BackendReady);
        qCInfo(VideoManagerLog) << "video initialization disabled for unit tests";
        return;
    }

    // CAS-gate NotStarted -> Pending: init() (GUI thread) and waitForVideoBackendReady() (other threads)
    // both enter here; without it both launch VideoBackend::initialize() -> double-init SIGABRT.
    // The mutex holds back waiters until _backendInitFuture is assigned.
    QMutexLocker lock(&_initFutureMutex);
    InitState expected = InitState::NotStarted;
    if (!_initState.compare_exchange_strong(expected, InitState::Pending)) {
        qCWarning(VideoManagerLog) << "video init already started";
        return;
    }

    const VideoBackend::EnvPrepResult envResult = VideoBackend::prepareEnvironment();
    // Snapshot argv + env result here on the GUI thread; QCoreApplication::arguments() is not thread-safe.
    _backendInitFuture = QtConcurrent::run(&VideoBackend::initialize, QCoreApplication::arguments(), envResult);

    _backendInitFuture.then(this, [this](bool success) {
        _onBackendInitComplete(success);
    }).onCanceled(this, [this] {
        _onBackendInitComplete(false);
    });
}

bool VideoManager::waitForVideoBackendReady(std::chrono::milliseconds timeout)
{
    if (!VideoBackend::needsAsyncInit()) return true;

    if (_backendDisabledForTests) {
        return true;
    }

    if (_initState.load() == InitState::NotStarted) {
        startVideoBackendInit();
    }

    switch (_initState.load()) {
    case InitState::Failed:
        return false;
    case InitState::BackendReady:
    case InitState::Running:
        return true;
    case InitState::NotStarted:
    case InitState::Pending:
    case InitState::QmlReady:
        break;
    }

    QFuture<bool> future;
    {
        QMutexLocker lock(&_initFutureMutex);
        future = _backendInitFuture;
    }
    if (!future.isValid()) {
        qCCritical(VideoManagerLog) << "waitForVideoBackendReady: no valid future";
        return false;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QFutureWatcher<bool> watcher;
    (void) connect(&watcher, &QFutureWatcher<bool>::finished, &loop, &QEventLoop::quit);
    (void) connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    watcher.setFuture(future);
    if (!watcher.isFinished()) {
        timer.start(timeout);
        loop.exec();
    }

    if (!watcher.isFinished()) {
        qCCritical(VideoManagerLog) << "Timed out waiting for video init";
        return false;
    }

    const bool success = watcher.result();
    if (_initState.load() == InitState::Pending || _initState.load() == InitState::QmlReady) {
        _onBackendInitComplete(success);
    }
    return _initState.load() != InitState::Failed;
}

void VideoManager::init(QQuickWindow *mainWindow)
{
    if (_initialized) {
        qCDebug(VideoManagerLog) << "Video Manager already initialized";
        return;
    }

    if (!mainWindow) {
        qCCritical(VideoManagerLog) << "Failed To Init Video Manager - mainWindow is NULL";
        return;
    }
    _mainWindow = mainWindow;

    VideoBackend::onMainWindowReady(mainWindow);

    (void) connect(_videoSettings->videoSource(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->udpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->rtspUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->tcpUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->secondaryVideoEnabled(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->secondaryVideoUrl(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    // The source-type dropdown drives the URL scheme, so changing it must re-resolve the stream.
    (void) connect(_videoSettings->secondaryVideoSource(), &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
    (void) connect(_videoSettings->aspectRatio(), &Fact::rawValueChanged, this, &VideoManager::aspectRatioChanged);
    (void) connect(_videoSettings->lowLatencyMode(), &Fact::rawValueChanged, this, [this](const QVariant &value) { Q_UNUSED(value); _restartAllVideos(); });
    // rtpJitterLatencyMs needs a pipeline restart; route through _videoSourceChanged so _updateSettings
    // pushes the new value to each receiver and restarts exactly once (no double restart).
    (void) connect(_videoSettings->rtpJitterLatencyMs(), &Fact::rawValueChanged, this, [this](const QVariant &value) { Q_UNUSED(value); _videoSourceChanged(); });
    // autoReconnect is a live setting — push without restart so an in-flight reconnect
    // can be cancelled mid-backoff.
    (void) connect(_videoSettings->rtspAutoReconnect(), &Fact::rawValueChanged, this, [this](const QVariant &value) {
        const bool enabled = value.toBool();
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            receiver->setAutoReconnect(enabled);
        }
    });
    VideoBackend::bindDebugLevelFact(SettingsManager::instance()->appSettings()->gstDebugLevel(), this);
    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &VideoManager::_setActiveVehicle);

    (void) connect(this, &VideoManager::autoStreamConfiguredChanged, this, &VideoManager::_videoSourceChanged);

    if (VideoBackend::needsAsyncInit() && _initState.load() == InitState::NotStarted) {
        startVideoBackendInit();
    }

    _mainWindow->scheduleRenderJob(
        QRunnable::create([this] {
            QMetaObject::invokeMethod(this, &VideoManager::_initAfterQmlIsReady, Qt::QueuedConnection);
        }),
        QQuickWindow::AfterSynchronizingStage);

    _initialized = true;
}

void VideoManager::_initAfterQmlIsReady()
{
    if (!_mainWindow) {
        qCCritical(VideoManagerLog) << "_initAfterQmlIsReady called with NULL mainWindow";
        return;
    }

    qCDebug(VideoManagerLog) << "_initAfterQmlIsReady";

    if (VideoBackend::needsAsyncInit()) {
        switch (_initState.load()) {
        case InitState::Pending:
            _initState.store(InitState::QmlReady);
            qCDebug(VideoManagerLog) << "QML ready, waiting for video";
            return;
        case InitState::BackendReady:
            _initState.store(InitState::Running);
            qCDebug(VideoManagerLog) << "QML ready, video already done — creating receivers";
            break;
        case InitState::Failed:
            qCWarning(VideoManagerLog) << "QML ready but video init failed";
            return;
        case InitState::NotStarted:
        case InitState::QmlReady:
        case InitState::Running:
            qCWarning(VideoManagerLog) << "_initAfterQmlIsReady: unexpected state" << static_cast<int>(_initState.load());
            return;
        }
    }
    _createVideoReceivers();
}

void VideoManager::_onBackendInitComplete(bool success)
{
    if (!success) {
        _initState.store(InitState::Failed);
        qCCritical(VideoManagerLog) << "video initialization failed";
        return;
    }

    if (VideoBackend::needsAsyncInit() && _videoSettings) {
        _videoSettings->pruneUnavailableDecoders();
        VideoBackend::applyDecoderPriorities(_videoSettings->forceVideoDecoder()->rawValue().toInt());
    }

    switch (_initState.load()) {
    case InitState::Pending:
        _initState.store(InitState::BackendReady);
        qCDebug(VideoManagerLog) << "video ready, waiting for QML";
        return;
    case InitState::QmlReady:
        _initState.store(InitState::Running);
        qCDebug(VideoManagerLog) << "video ready, QML already done — creating receivers";
        _createVideoReceivers();
        return;
    default:
        qCWarning(VideoManagerLog) << "_onBackendInitComplete: unexpected state" << static_cast<int>(_initState.load());
        return;
    }
}

void VideoManager::_createVideoReceivers()
{
#ifdef QGC_UNITTEST_BUILD
    if (_createVideoReceiversForTest) {
        _createVideoReceiversForTest();
        return;
    }
#endif
    static const QStringList videoStreamList = {
        "videoContent",
        "thermalVideo",
        "secondaryVideo"
    };

    QStringList existing;
    existing.reserve(_videoReceivers.size());
    for (const VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        existing.append(receiver->name());
    }

    for (const QString &streamName : videoStreamList) {
        // Skip only names that already initialized; a once-failed receiver was removed from the
        // list, so re-entry retries it instead of being blocked by an all-or-nothing guard.
        if (existing.contains(streamName)) {
            continue;
        }
        VideoReceiver *receiver = QGCCorePlugin::instance()->createVideoReceiver(this);
        if (!receiver) {
            continue;
        }
        receiver->setName(streamName);

        _initVideoReceiver(receiver, _mainWindow);
    }
}

void VideoManager::cleanup()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        QGCCorePlugin::instance()->releaseVideoSink(receiver->sink());
    }
}

void VideoManager::_cleanupOldVideos()
{
    if (!SettingsManager::instance()->videoSettings()->enableStorageLimit()->rawValue().toBool()) {
        return;
    }

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    QDir videoDir = QDir(savePath);
    videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    videoDir.setSorting(QDir::Time);

    QStringList nameFilters;
    for (size_t i = 0; i < std::size(kFileExtension); i++) {
        nameFilters << QStringLiteral("*.") + kFileExtension[i];
    }

    videoDir.setNameFilters(nameFilters);
    QFileInfoList vidList = videoDir.entryInfoList();
    if (vidList.isEmpty()) {
        return;
    }

    uint64_t total = 0;
    for (const QFileInfo &video : std::as_const(vidList)) {
        total += video.size();
    }

    const uint64_t maxSize = SettingsManager::instance()->videoSettings()->maxVideoSize()->rawValue().toUInt() * qPow(1024, 2);
    while ((total >= maxSize) && !vidList.isEmpty()) {
        const QFileInfo info = vidList.takeLast();
        total -= info.size();
        const QString path = info.filePath();
        qCDebug(VideoManagerLog) << "Removing old video file:" << path;
        (void) QFile::remove(path);
    }
}

void VideoManager::startRecording(const QString &videoFile)
{
    const VideoReceiver::FILE_FORMAT fileFormat = static_cast<VideoReceiver::FILE_FORMAT>(_videoSettings->recordingFormat()->rawValue().toInt());
    if (!VideoReceiver::isValidFileFormat(fileFormat)) {
        QGC::showAppMessage(tr("Invalid video format defined."));
        return;
    }

    _cleanupOldVideos();

    const QString savePath = SettingsManager::instance()->appSettings()->videoSavePath();
    if (savePath.isEmpty()) {
        QGC::showAppMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    const QString videoFileUrl = videoFile.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") : videoFile;
    const QString ext = kFileExtension[fileFormat];

    const QString videoFileNameTemplate = savePath + "/" + videoFileUrl + ".%1" + ext;

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        if (!receiver->started()) {
            qCDebug(VideoManagerLog) << "Video receiver is not ready.";
            continue;
        }
        const QString streamName = (receiver->name() == QStringLiteral("videoContent")) ? "" : (receiver->name() + ".");
        const QString videoFileName = videoFileNameTemplate.arg(streamName);
        receiver->startRecording(videoFileName, fileFormat);
    }
}

void VideoManager::stopRecording()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->stopRecording();
    }
}

void VideoManager::grabImage(const QString &imageFile)
{
    if (imageFile.isEmpty()) {
        _imageFile = SettingsManager::instance()->appSettings()->photoSavePath();
        _imageFile += QStringLiteral("/") + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss.zzz") + QStringLiteral(".jpg");
    } else {
        _imageFile = imageFile;
    }

    emit imageFileChanged(_imageFile);

    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        receiver->takeScreenshot(_imageFile);
        // QSharedPointer<QQuickItemGrabResult> result = receiver->widget()->grabToImage(const QSize &targetSize = QSize())
    }
}

double VideoManager::aspectRatio() const
{
    // Live decoded resolution wins — set by VideoReceiver::videoSizeChanged once frames flow.
    if (!_videoSize.isEmpty()) {
        return static_cast<double>(_videoSize.width()) / _videoSize.height();
    }

    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && pInfo && !pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

double VideoManager::thermalAspectRatio() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->aspectRatio();
        }
    }

    return 1.0;
}

double VideoManager::hfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && pInfo && !pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return 1.0;
}

double VideoManager::thermalHfov() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return pInfo->hfov();
        }
    }

    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

bool VideoManager::hasThermal() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (receiver->isThermal() && pInfo && pInfo->isThermal()) {
            return true;
        }
    }

    return false;
}

bool VideoManager::hasVideo() const
{
    return (_videoSettings->streamEnabled()->rawValue().toBool() && _videoSettings->streamConfigured());
}

bool VideoManager::hasSecondaryVideo() const
{
    return (_videoSettings->secondaryVideoEnabled()->rawValue().toBool()
            && !_videoSettings->secondaryVideoUrl()->rawValue().toString().isEmpty());
}

bool VideoManager::isUvc() const
{
    return (!_uvcVideoSourceID.isEmpty() && UVCReceiver::enabled() && hasVideo());
}

void VideoManager::setfullScreen(bool on)
{
    if (on) {
        if (!_activeVehicle || _activeVehicle->vehicleLinkManager()->communicationLost()) {
            on = false;
        }
    }

    if (on != _fullScreen) {
        _fullScreen = on;
        emit fullScreenChanged();
    }
}

bool VideoManager::isStreamSource() const
{
    static const QStringList videoSourceList = {
        VideoSettings::videoSourceUDPH264,
        VideoSettings::videoSourceUDPH265,
        VideoSettings::videoSourceRTSP,
        VideoSettings::videoSourceTCP,
        VideoSettings::videoSourceMPEGTS,
        VideoSettings::videoSource3DRSolo,
        VideoSettings::videoSourceParrotDiscovery,
        VideoSettings::videoSourceYuneecMantisG,
        VideoSettings::videoSourceHerelinkAirUnit,
        VideoSettings::videoSourceHerelinkHotspot,
    };
    const QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return (videoSourceList.contains(videoSource) || autoStreamConfigured());
}

void VideoManager::_videoSourceChanged()
{
    // A settings change is fresh user intent: clear every restart budget so a receiver that already
    // exhausted its attempts gets retried instead of staying dead until the app restarts.
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        _resetReceiverRestartBudget(receiver);
    }

    bool changed = false;
    if (_activeVehicle) {
        QGCCameraManager* camMgr = _activeVehicle->cameraManager();
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            QGCVideoStreamInfo* info = nullptr;
            if (receiver->isSecondary()) {
                // The secondary stream is driven purely by its own URL setting,
                // it never participates in MAVLink camera auto-discovery.
                info = nullptr;
            } else if (receiver->isThermal()) {
                info = camMgr ? camMgr->thermalStreamInstance() : nullptr;
            } else {
                info = camMgr ? camMgr->currentStreamInstance() : nullptr;
            }
            receiver->setVideoStreamInfo(info);
            changed |= _updateSettings(receiver);
        }
    } else {
        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            receiver->setVideoStreamInfo(nullptr);
            changed |= _updateSettings(receiver);
        }
    }

    if (changed) {
        emit hasVideoChanged();
        emit hasSecondaryVideoChanged();
        emit isStreamSourceChanged();
        emit isAutoStreamChanged();

        if (hasVideo() || hasSecondaryVideo()) {
            _restartAllVideos();
        } else {
            stopVideo();
        }

        qCDebug(VideoManagerLog) << "New Video Source:" << _videoSettings->videoSource()->rawValue().toString();
    }
}

void VideoManager::_onLowMemory()
{
    QFile statusFile(QString("/proc/%1/status").arg(QCoreApplication::applicationPid()));
    if (!statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    int rssKb = 0;
    QTextStream in(&statusFile);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith("VmRSS:")) {
            rssKb = line.split(QRegularExpression("\\s+"))[1].toInt();
            break;
        }
    }
    statusFile.close();

    if (rssKb <= 0) {
        return;
    }

    static qint64 s_lastLogMs = 0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if ((nowMs - s_lastLogMs) >= 10000) {
        s_lastLogMs = nowMs;
        qCInfo(VideoManagerLog) << "RSS" << (rssKb / 1024) << "MB";
    }

    // Ordered shed, well below the vendor's abnormal-memory kill threshold: 400 MB was already too
    // late on ColorOS/OxygenOS, and by then the native heap is too fragmented to give anything back.
    if (rssKb < kRssShedSecondaryKb) {
        return;
    }

    if (_videoSettings->secondaryVideoEnabled()->rawValue().toBool()) {
        qCWarning(VideoManagerLog) << "High memory usage (" << (rssKb / 1024)
                                   << "MB) — disabling secondary video to avoid an OOM kill";
        _videoSettings->secondaryVideoEnabled()->setRawValue(false);
        return;
    }

    // Secondary is already off and RSS keeps climbing: stop every stream rather than be killed.
    if (rssKb >= kRssShedAllKb && hasVideo()) {
        qCWarning(VideoManagerLog) << "Memory still critical (" << (rssKb / 1024)
                                   << "MB) — stopping all video to avoid an OOM kill";
        stopVideo();
        QGC::showAppMessage(QStringLiteral("Video stopped: memory usage reached %1 MB and the app was about to be "
                                           "killed by the system. Reduce the number of active video streams.")
                                .arg(rssKb / 1024));
    }
}

bool VideoManager::_receiverShouldRun(VideoReceiver *receiver) const
{
    if (!receiver) {
        return false;
    }

    if (receiver->isSecondary()) {
        return (_videoSettings->secondaryVideoEnabled()->rawValue().toBool()
                && !_videoSettings->secondaryVideoUrl()->rawValue().toString().isEmpty());
    }

    if (receiver->isThermal()) {
        // Thermal has no URI setting — it is driven entirely by the MAVLink stream info.
        return (receiver->videoStreamInfo() != nullptr);
    }

    return (hasVideo() && !receiver->uri().isEmpty());
}

QString VideoManager::_normalizeSecondaryVideoUri(const QString &rawUri) const
{
    static const QStringList kSupportedSchemes = {
        QStringLiteral("rtsp"), QStringLiteral("udp"), QStringLiteral("udp265"),
        QStringLiteral("tcp"), QStringLiteral("mpegts")
    };

    const QString uri = rawUri.trimmed();
    if (uri.isEmpty()) {
        return QString();
    }

    const QString source = _videoSettings->secondaryVideoSource()->rawValue().toString();
    QString wantedScheme;
    if (source == VideoSettings::videoSourceUDPH264) {
        wantedScheme = QStringLiteral("udp");
    } else if (source == VideoSettings::videoSourceUDPH265) {
        wantedScheme = QStringLiteral("udp265");
    } else if (source == VideoSettings::videoSourceTCP) {
        wantedScheme = QStringLiteral("tcp");
    } else if (source == VideoSettings::videoSourceMPEGTS) {
        wantedScheme = QStringLiteral("mpegts");
    } else if (source == VideoSettings::videoDisabled) {
        // "Video Stream Disabled" in the dropdown wins over a filled-in URL.
        return QString();
    } else {
        wantedScheme = QStringLiteral("rtsp");
    }

    const int sep = uri.indexOf(QStringLiteral("://"));
    const QString currentScheme = (sep > 0) ? uri.left(sep).toLower() : QString();
    const bool currentSupported = kSupportedSchemes.contains(currentScheme);
    const bool udpPair = ((currentScheme == QStringLiteral("udp")) || (currentScheme == QStringLiteral("udp265")))
                       && ((wantedScheme == QStringLiteral("udp")) || (wantedScheme == QStringLiteral("udp265")));

    QString result = uri;
    if (!currentSupported) {
        // No usable scheme (a bare "host:port" for instance): adopt the dropdown's.
        result = wantedScheme + QStringLiteral("://") + uri;
    } else if (udpPair && (currentScheme != wantedScheme)) {
        // udp vs udp265 is the one ambiguity the URL cannot settle by itself — both are "udp" to a
        // reader, but they select different caps, depayloader and parser. The dropdown is the
        // explicit statement of which codec to expect. rtsp/tcp/mpegts are left untouched so a
        // filled-in URL always beats the dropdown's default.
        result = wantedScheme + QStringLiteral("://") + uri.mid(sep + 3);
    }

    if (result != uri) {
        qCInfo(VideoManagerLog) << "Secondary video URL normalized:" << uri << "->" << result
                                << "(source:" << source << ")";
    }

    return result;
}

void VideoManager::_resetReceiverRestartBudget(VideoReceiver *receiver)
{
    if (!receiver) {
        return;
    }

    auto it = _restartState.find(receiver);
    if (it == _restartState.end()) {
        return;
    }

    it->attempts = 0;
    ++it->token;  // invalidate any restart already queued against the old budget
}

void VideoManager::_scheduleReceiverRestart(VideoReceiver *receiver, const char *reason)
{
    if (!receiver) {
        return;
    }

    // The stream is gone from the settings: drop the budget entirely instead of retrying forever.
    if (!_receiverShouldRun(receiver)) {
        _restartState.remove(receiver);
        qCDebug(VideoManagerLog) << "Not restarting" << receiver->name() << "— stream no longer configured (" << reason << ")";
        return;
    }

    ReceiverRestartState &state = _restartState[receiver];
    state.attempts = std::min(state.attempts + 1, kMaxRestartAttempts + 1);
    ++state.token;
    const int attempts = state.attempts;
    const quint64 token = state.token;

    if (attempts > kMaxRestartAttempts) {
        qCWarning(VideoManagerLog) << "Giving up on" << receiver->name() << "after" << (attempts - 1)
                                   << "failed restarts (" << reason << "). Change a video setting to retry.";
        return;
    }

    // 1s → 2s → 4s → 8s → 16s → 30s. The fixed 1 s retry used to win the race against the
    // receiver's own backoff, so every hiccup rebuilt the pipeline at ~1 Hz.
    const int delaySec = std::min(1 << std::min(attempts - 1, 5), 30);
    qCInfo(VideoManagerLog) << "Restarting" << receiver->name() << "in" << delaySec << "s (attempt" << attempts
                            << ", after" << reason << ")" << receiver->uri();

    QTimer::singleShot(delaySec * 1000, receiver, [this, receiver, token]() {
        const auto it = _restartState.constFind(receiver);
        if ((it == _restartState.constEnd()) || (it->token != token)) {
            return;  // superseded by a newer restart or a settings change
        }
        if (receiver->started() || !_receiverShouldRun(receiver)) {
            return;
        }
        _startReceiver(receiver);
    });
}


bool VideoManager::_updateUVC(VideoReceiver * /*receiver*/)
{
    bool result = false;

    const QString oldUvcVideoSrcID = _uvcVideoSourceID;

    if (!UVCReceiver::enabled() || !hasVideo() || isStreamSource()) {
        _uvcVideoSourceID = QString();
    } else {
        _uvcVideoSourceID = UVCReceiver::getSourceId();
    }

    if (oldUvcVideoSrcID != _uvcVideoSourceID) {
        qCDebug(VideoManagerLog) << "UVC changed from [" << oldUvcVideoSrcID << "] to [" << _uvcVideoSourceID << "]";
        if (!_uvcVideoSourceID.isEmpty()) {
            UVCReceiver::checkPermission();
        }
        result = true;
        emit uvcVideoSourceIDChanged();
        emit isUvcChanged();
    }

    return result;
}

bool VideoManager::autoStreamConfigured() const
{
    for (VideoReceiver *receiver : _videoReceivers) {
        QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
        if (!receiver->isThermal() && pInfo && !pInfo->isThermal()) {
            return !pInfo->uri().isEmpty();
        }
    }

    return false;
}

bool VideoManager::_updateAutoStream(VideoReceiver *receiver)
{
    const QGCVideoStreamInfo *pInfo = receiver->videoStreamInfo();
    if (!pInfo) {
        return false;
    }

    qCDebug(VideoManagerLog) << QString("Configure stream (%1):").arg(receiver->name()) << pInfo->uri();

    QString source, url;
    switch (pInfo->type()) {
    case VIDEO_STREAM_TYPE_RTSP:
        source = VideoSettings::videoSourceRTSP;
        url = pInfo->uri();
        if (source == VideoSettings::videoSourceRTSP) {
            _videoSettings->rtspUrl()->setRawValue(url);
        }
        break;
    case VIDEO_STREAM_TYPE_TCP_MPEG:
        source = VideoSettings::videoSourceTCP;
        url = pInfo->uri();
        break;
    case VIDEO_STREAM_TYPE_RTPUDP:
        if (pInfo->encoding() == VIDEO_STREAM_ENCODING_H265) {
            source = VideoSettings::videoSourceUDPH265;
            url = pInfo->uri().contains("udp265://") ? pInfo->uri() : QStringLiteral("udp265://0.0.0.0:%1").arg(pInfo->uri());
        } else {
            source = VideoSettings::videoSourceUDPH264;
            url = pInfo->uri().contains("udp://") ? pInfo->uri() : QStringLiteral("udp://0.0.0.0:%1").arg(pInfo->uri());
        }
        break;
    case VIDEO_STREAM_TYPE_MPEG_TS:
        source = VideoSettings::videoSourceMPEGTS;
        url = pInfo->uri().contains("mpegts://") ? pInfo->uri() : QStringLiteral("mpegts://0.0.0.0:%1").arg(pInfo->uri());
        break;
    default:
        qCWarning(VideoManagerLog) << "Unknown VIDEO_STREAM_TYPE";
        source = VideoSettings::videoSourceNoVideo;
        url = pInfo->uri();
        break;
    }

    const bool settingsChanged = _updateVideoUri(receiver, url);
    if (settingsChanged) {
        if (!receiver->isThermal()) {
            _videoSettings->videoSource()->setRawValue(source);
        }

        emit autoStreamConfiguredChanged();
    }

    return settingsChanged;
}

bool VideoManager::_updateVideoUri(VideoReceiver *receiver, const QString &uri)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    if ((uri == receiver->uri()) && !receiver->uri().isNull()) {
        return false;
    }

    qCDebug(VideoManagerLog) << "New Video URI" << uri;

    receiver->setUri(uri);

    return true;
}

bool VideoManager::_updateSettings(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return false;
    }

    bool settingsChanged = false;

    const bool lowLatency = _videoSettings->lowLatencyMode()->rawValue().toBool();
    if (lowLatency != receiver->lowLatency()) {
        receiver->setLowLatency(lowLatency);
        settingsChanged = true;
    }

    const int rtpLatencyMs = static_cast<int>(std::min(_videoSettings->rtpJitterLatencyMs()->rawValue().toUInt(), static_cast<uint>(INT_MAX)));
    if (rtpLatencyMs != receiver->rtpJitterLatencyMs()) {
        receiver->setRtpJitterLatencyMs(rtpLatencyMs);
        settingsChanged = true;
    }

    const bool autoReconnect = _videoSettings->rtspAutoReconnect()->rawValue().toBool();
    if (autoReconnect != receiver->autoReconnect()) {
        receiver->setAutoReconnect(autoReconnect);
        // No settingsChanged: autoReconnect is live, doesn't require pipeline restart.
    }

    if (receiver->isSecondary()) {
        // The secondary stream is driven by its own URL setting and shares the low-latency /
        // jitter / auto-reconnect receiver options with the primary. The source-type dropdown
        // supplies the scheme when the URL has none, and resolves the udp vs udp265 ambiguity.
        const QString uri = _videoSettings->secondaryVideoEnabled()->rawValue().toBool()
                ? _normalizeSecondaryVideoUri(_videoSettings->secondaryVideoUrl()->rawValue().toString())
                : QString();
        settingsChanged |= _updateVideoUri(receiver, uri);
        return settingsChanged;
    }

    if (receiver->isThermal()) {
        return settingsChanged;
    }

    settingsChanged |= _updateUVC(receiver);
    settingsChanged |= _updateAutoStream(receiver);

    const QString source = _videoSettings->videoSource()->rawValue().toString();
    if (source == VideoSettings::videoSourceUDPH264) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceUDPH265) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp265://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceMPEGTS) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("mpegts://%1").arg(_videoSettings->udpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSourceRTSP) {
        settingsChanged |= _updateVideoUri(receiver, _videoSettings->rtspUrl()->rawValue().toString());
    } else if (source == VideoSettings::videoSourceTCP) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("tcp://%1").arg(_videoSettings->tcpUrl()->rawValue().toString()));
    } else if (source == VideoSettings::videoSource3DRSolo) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:5600"));
    } else if (source == VideoSettings::videoSourceParrotDiscovery) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("udp://0.0.0.0:8888"));
    } else if (source == VideoSettings::videoSourceYuneecMantisG) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.42.1:554/live"));
    } else if (source == VideoSettings::videoSourceHerelinkAirUnit) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.0.10:8554/H264Video"));
    } else if (source == VideoSettings::videoSourceHerelinkHotspot) {
        settingsChanged |= _updateVideoUri(receiver, QStringLiteral("rtsp://192.168.43.1:8554/fpv_stream"));
    } else if ((source == VideoSettings::videoDisabled) || (source == VideoSettings::videoSourceNoVideo)) {
        settingsChanged |= _updateVideoUri(receiver, QString());
    } else {
        settingsChanged |= _updateVideoUri(receiver, QString());
        if (!isUvc()) {
            qCCritical(VideoManagerLog) << "Video source URI \"" << source << "\" is not supported. Please add support!";
        }
    }

    return settingsChanged;
}

void VideoManager::_setActiveVehicle(Vehicle *vehicle)
{
    qCDebug(VideoManagerLog) << Q_FUNC_INFO << "new vehicle" << vehicle << "old active vehicle" << _activeVehicle;

    if (_activeVehicle) {
        (void) disconnect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        auto cameraManager = _activeVehicle->cameraManager();
        if (cameraManager) {
            MavlinkCameraControlInterface *pCamera = cameraManager->currentCameraInstance();
            if (pCamera) {
                pCamera->stopStream();
            }
            (void) disconnect(cameraManager, &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
        }

        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            // disconnect(receiver->videoStreamInfo(), &QGCVideoStreamInfo::infoChanged, ))
            receiver->setVideoStreamInfo(nullptr);
        }
    }

    _activeVehicle = vehicle;
    if (_activeVehicle) {
        (void) connect(_activeVehicle->vehicleLinkManager(), &VehicleLinkManager::communicationLostChanged, this, &VideoManager::_communicationLostChanged);
        if (_activeVehicle->cameraManager()) {
            (void) connect(_activeVehicle->cameraManager(), &QGCCameraManager::streamChanged, this, &VideoManager::_videoSourceChanged);
            MavlinkCameraControlInterface *pCamera = _activeVehicle->cameraManager()->currentCameraInstance();
            if (pCamera) {
                pCamera->resumeStream();
            }
        }

        for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
            if (_activeVehicle->cameraManager()) {
                if (receiver->isSecondary()) {
                    receiver->setVideoStreamInfo(nullptr);
                } else if (receiver->isThermal()) {
                    receiver->setVideoStreamInfo(_activeVehicle->cameraManager()->thermalStreamInstance());
                } else {
                    receiver->setVideoStreamInfo(_activeVehicle->cameraManager()->currentStreamInstance());
                }
            } else {
                receiver->setVideoStreamInfo(nullptr);
            }
            // connect(receiver->videoStreamInfo(), &QGCVideoStreamInfo::infoChanged, ))
        }
    } else {
        setfullScreen(false);
    }
}

void VideoManager::_communicationLostChanged(bool connectionLost)
{
    if (connectionLost) {
        setfullScreen(false);
    }
}

void VideoManager::_restartAllVideos()
{
    for (VideoReceiver *videoReceiver : std::as_const(_videoReceivers)) {
        _restartVideo(videoReceiver);
    }
}

void VideoManager::_restartVideo(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    qCDebug(VideoManagerLog) << "Restart video receiver" << receiver->name();

    if (receiver->started()) {
        _stopReceiver(receiver);
        // onStopComplete Signal Will Restart It
    } else {
        _startReceiver(receiver);
    }
}

void VideoManager::_stopReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        receiver->stop();
    }
}

void VideoManager::stopVideo()
{
    for (VideoReceiver *receiver : std::as_const(_videoReceivers)) {
        _stopReceiver(receiver);
    }
}

void VideoManager::_startReceiver(VideoReceiver *receiver)
{
    if (!receiver) {
        qCDebug(VideoManagerLog) << "VideoReceiver is NULL";
        return;
    }

    if (receiver->started()) {
        qCDebug(VideoManagerLog) << "VideoReceiver is already started" << receiver->name();
        return;
    }

    if (receiver->uri().isEmpty()) {
        qCDebug(VideoManagerLog) << "VideoUri is NULL" << receiver->name();
        return;
    }

    const QString source = _videoSettings->videoSource()->rawValue().toString();
    // RTSP timeouts apply when either the primary source or this receiver's URI is RTSP
    const uint32_t timeout = ((source == VideoSettings::videoSourceRTSP) || receiver->uri().startsWith(QStringLiteral("rtsp://"))) ? _videoSettings->rtspTimeout()->rawValue().toUInt() : 3;

    receiver->start(timeout);
}

void VideoManager::_initVideoReceiver(VideoReceiver *receiver, QQuickWindow *window)
{
    if (_videoReceivers.contains(receiver)) {
        qCWarning(VideoManagerLog) << "Receiver already initialized";
        return;
    }

    // Register before any setup so re-entry is blocked at every point below; error paths remove it.
    _videoReceivers.append(receiver);

    QQuickItem *widget = window->findChild<QQuickItem*>(receiver->name());
    if (!widget) {
        qCCritical(VideoManagerLog) << "stream widget not found" << receiver->name();
        _videoReceivers.removeOne(receiver);
        receiver->deleteLater();
        return;
    }
    receiver->setWidget(widget);

    void *sink = QGCCorePlugin::instance()->createVideoSink(receiver->widget(), receiver);
    if (!sink) {
        qCCritical(VideoManagerLog) << "createVideoSink() failed" << receiver->name();
        _videoReceivers.removeOne(receiver);
        receiver->deleteLater();
        return;
    }
    receiver->setSink(sink);

    VideoBackend::attachSink(receiver, sink, widget);

    (void) connect(receiver, &VideoReceiver::onStartComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "Start complete, status:" << status;
        switch (status) {
        case VideoReceiver::STATUS_OK:
            receiver->setStarted(true);
            // Stream is up: clear the failure budget so the next unrelated hiccup starts at 1 s again.
            _resetReceiverRestartBudget(receiver);
            if (receiver->sink()) {
                receiver->startDecoding(receiver->sink());
            }
            break;
        case VideoReceiver::STATUS_INVALID_URL:
        case VideoReceiver::STATUS_INVALID_STATE:
            break;
        default:
            // Rate limit restarts on start failure — bounded backoff instead of a fixed 1 s retry.
            _scheduleReceiverRestart(receiver, "start failed");
            break;
        }
    });

    (void) connect(receiver, &VideoReceiver::onStopComplete, this, [this, receiver](VideoReceiver::STATUS status) {
        qCDebug(VideoManagerLog) << "Stop complete" << receiver->name() << receiver->uri()  << ", status:" << status;
        receiver->setStarted(false);
        if (status == VideoReceiver::STATUS_INVALID_URL) {
            qCDebug(VideoManagerLog) << "Invalid video URL. Not restarting";
        } else {
            // Bounded backoff. The old unconditional 1 s restart raced the receiver's own backoff
            // reconnect, collapsing it into a ~1 s stop/start loop that rebuilt the pipeline (and the
            // Android MediaCodec) continuously — the native-heap growth behind the OOM kills.
            _scheduleReceiverRestart(receiver, "stopped");
        }
    });

    (void) connect(receiver, &VideoReceiver::streamingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "streaming changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isSecondary()) {
            _streaming = active;
            emit streamingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::decodingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "decoding changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isSecondary()) {
            _decoding = active;
            emit decodingChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingChanged, this, [this, receiver](bool active) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording changed, active:" << (active ? "yes" : "no");
        if (!receiver->isThermal() && !receiver->isSecondary()) {
            _recording = active;
            if (!active) {
                _subtitleWriter->stopCapturingTelemetry();
            }
            emit recordingChanged(_recording);
        }
    });

    (void) connect(receiver, &VideoReceiver::recordingStarted, this, [this, receiver](const QString &filename) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "recording started";
        if (!receiver->isThermal() && !receiver->isSecondary()) {
            _subtitleWriter->startCapturingTelemetry(filename, videoSize());
        }
    });

    (void) connect(receiver, &VideoReceiver::videoSizeChanged, this, [this, receiver](QSize size) {
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "resized. New resolution:" << size.width() << "x" << size.height();
        if (!receiver->isThermal() && !receiver->isSecondary()) {
            _videoSize = size;
            emit videoSizeChanged();
            emit aspectRatioChanged();
        }
    });

    (void) connect(receiver, &VideoReceiver::onTakeScreenshotComplete, this, [receiver](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            qCDebug(VideoManagerLog) << "Video" << receiver->name() << "screenshot taken";
        } else {
            qCWarning(VideoManagerLog) << "Video" << receiver->name() << "screenshot failed";
        }
    });

    (void) connect(receiver, &VideoReceiver::videoStreamInfoChanged, this, [this, receiver]() {
        const QGCVideoStreamInfo *videoStreamInfo = receiver->videoStreamInfo();
        qCDebug(VideoManagerLog) << "Video" << receiver->name() << "stream info:" << (videoStreamInfo ? "received" : "lost");

        (void) _updateAutoStream(receiver);
    });

    (void) _updateSettings(receiver);

    if (hasVideo() || (receiver->isSecondary() && hasSecondaryVideo())) {
        _startReceiver(receiver);
    }
}

void VideoManager::startVideo()
{
    qCDebug(VideoManagerLog) << "startVideo";

    if (!hasVideo()) {
        qCDebug(VideoManagerLog) << "Stream not enabled/configured";
        return;
    }

    _restartAllVideos();
}