#pragma once

#include <atomic>
#include <chrono>

#include <QtCore/QDateTime>
#include <QtCore/QFuture>
#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QPromise>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtQmlIntegration/QtQmlIntegration>

#ifdef QGC_UNITTEST_BUILD
#include <functional>
#endif

class QQuickWindow;
class SubtitleWriter;
class Vehicle;
class VideoReceiver;
class VideoSettings;

class VideoManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")
    Q_MOC_INCLUDE("Vehicle.h")

    Q_PROPERTY(bool     autoStreamConfigured    READ autoStreamConfigured                       NOTIFY autoStreamConfiguredChanged)
    Q_PROPERTY(bool     decoding                READ decoding                                   NOTIFY decodingChanged)
    Q_PROPERTY(bool     fullScreen              READ fullScreen             WRITE setfullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(bool     hasSecondaryVideo       READ hasSecondaryVideo                          NOTIFY hasSecondaryVideoChanged)
    Q_PROPERTY(bool     hasThermal              READ hasThermal                                 NOTIFY decodingChanged)
    Q_PROPERTY(bool     hasVideo                READ hasVideo                                   NOTIFY hasVideoChanged)
    Q_PROPERTY(bool     isStreamSource          READ isStreamSource                             NOTIFY isStreamSourceChanged)
    Q_PROPERTY(bool     isUvc                   READ isUvc                                      NOTIFY isUvcChanged)
    Q_PROPERTY(bool     recording               READ recording                                  NOTIFY recordingChanged)
    Q_PROPERTY(bool     streaming               READ streaming                                  NOTIFY streamingChanged)
    Q_PROPERTY(double   aspectRatio             READ aspectRatio                                NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   hfov                    READ hfov                                       NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   thermalAspectRatio      READ thermalAspectRatio                         NOTIFY aspectRatioChanged)
    Q_PROPERTY(double   thermalHfov             READ thermalHfov                                NOTIFY aspectRatioChanged)
    Q_PROPERTY(QSize    videoSize               READ videoSize                                  NOTIFY videoSizeChanged)
    Q_PROPERTY(QString  imageFile               READ imageFile                                  NOTIFY imageFileChanged)
    Q_PROPERTY(QString  uvcVideoSourceID        READ uvcVideoSourceID                           NOTIFY uvcVideoSourceIDChanged)

    friend class VideoManagerInitTest;

public:
    explicit VideoManager(QObject *parent = nullptr);
    ~VideoManager();

    static VideoManager *instance();

    Q_INVOKABLE void grabImage(const QString &imageFile = QString());
    Q_INVOKABLE void startRecording(const QString &videoFile = QString());
    Q_INVOKABLE void startVideo();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void stopVideo();

    void init(QQuickWindow *mainWindow);
    void startVideoBackendInit();
    bool waitForVideoBackendReady(std::chrono::milliseconds timeout = std::chrono::minutes(1));
    void cleanup();
    bool autoStreamConfigured() const;
    bool decoding() const { return _decoding; }
    bool fullScreen() const { return _fullScreen; }
    bool hasSecondaryVideo() const;
    bool hasThermal() const;
    bool hasVideo() const;
    bool isStreamSource() const;
    bool isUvc() const;
    bool recording() const { return _recording; }
    bool streaming() const { return _streaming; }
    double aspectRatio() const;
    double hfov() const;
    double thermalAspectRatio() const;
    double thermalHfov() const;
    QSize videoSize() const { return _videoSize; }
    QString imageFile() const { return _imageFile; }
    QString uvcVideoSourceID() const { return _uvcVideoSourceID; }
    void setfullScreen(bool on);

signals:
    void aspectRatioChanged();
    void autoStreamConfiguredChanged();
    void decodingChanged();
    void fullScreenChanged();
    void hasSecondaryVideoChanged();
    void hasVideoChanged();
    void imageFileChanged(const QString &filename);
    void isAutoStreamChanged();
    void isStreamSourceChanged();
    void isUvcChanged();
    void recordingChanged(bool recording);
    void recordingStarted(const QString &filename);
    void streamingChanged();
    void uvcVideoSourceIDChanged();
    void videoSizeChanged();

private slots:
    void _communicationLostChanged(bool communicationLost);
    void _setActiveVehicle(Vehicle *vehicle);
    void _videoSourceChanged();
    void _onLowMemory();

private:
    enum class InitState : uint8_t {
        NotStarted,
        Pending,
        BackendReady,
        QmlReady,
        Running,
        Failed
    };

    void _initAfterQmlIsReady();
    void _onBackendInitComplete(bool success);
    void _createVideoReceivers();
    void _initVideoReceiver(VideoReceiver *receiver, QQuickWindow *window);
    bool _updateAutoStream(VideoReceiver *receiver);
    bool _updateUVC(VideoReceiver *receiver);
    bool _updateSettings(VideoReceiver *receiver);
    bool _updateVideoUri(VideoReceiver *receiver, const QString &uri);
    void _restartAllVideos();
    void _restartVideo(VideoReceiver *receiver);
    void _startReceiver(VideoReceiver *receiver);
    void _stopReceiver(VideoReceiver *receiver);
    static void _cleanupOldVideos();

    /// Schedules a *bounded* restart: exponential backoff, an attempt cap, and a liveness check so a
    /// receiver whose stream is gone (or that keeps failing) can't spin a stop/start loop. Unbounded
    /// 1 s restarts rebuilt the GStreamer pipeline + Android MediaCodec over and over, and each
    /// rebuild leaked native heap until ColorOS killed the process for "abnormal memory".
    void _scheduleReceiverRestart(VideoReceiver *receiver, const char *reason);
    /// True when this receiver's stream is still configured and therefore worth (re)starting.
    bool _receiverShouldRun(VideoReceiver *receiver) const;
    /// Builds the secondary stream URI the way the primary one is built: the source dropdown
    /// selects the transport and supplies the scheme, the matching address field supplies host:port.
    /// Empty when disabled or when the selected transport has no address yet.
    QString _secondaryVideoUri() const;

    /// Clears the backoff/attempt budget after a successful start or a user-initiated settings change.
    void _resetReceiverRestartBudget(VideoReceiver *receiver);

    struct ReceiverRestartState {
        int attempts = 0;
        quint64 token = 0;
    };

    QHash<VideoReceiver*, ReceiverRestartState> _restartState;
    QList<VideoReceiver*> _videoReceivers;
    SubtitleWriter *_subtitleWriter = nullptr;
    VideoSettings *_videoSettings = nullptr;
    QQuickWindow *_mainWindow = nullptr;
    QTimer *_lowMemoryWarningTimer = nullptr;
    Vehicle *_activeVehicle = nullptr;
    /// Set when memory pressure forced the secondary stream off. Deliberately *not* a setting:
    /// it must not overwrite what the user picked in the source dropdown, and it resolves
    /// itself once RSS comes back down.
    bool _secondaryShedByLowMemory = false;

    std::atomic<InitState> _initState = InitState::NotStarted;
    // Orders _backendInitFuture publication against cross-thread waiters.
    QMutex _initFutureMutex;
    QFuture<bool> _backendInitFuture;
    bool _initialized = false;
    bool _backendDisabledForTests = false;
    bool _fullScreen = false;

    QAtomicInteger<bool> _decoding = false;
    QAtomicInteger<bool> _recording = false;
    QAtomicInteger<bool> _streaming = false;
    QSize _videoSize;
    QString _imageFile;
    QString _uvcVideoSourceID;

#ifdef QGC_UNITTEST_BUILD
    std::function<void()> _createVideoReceiversForTest;
#endif
};