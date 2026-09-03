import QtQuick
import QtQuick.Controls
import QtMultimedia

import QGroundControl
import QGroundControl.Controls

/****************************************************************************
 *
 * Floating secondary video window for the Fly View.
 *
 * - Free dragging anywhere in the Fly View (title bar drag)
 * - Corner resize keeping 16:9 aspect ratio
 * - Floats above the map / primary PiP (z order of widgets layer)
 * - Position and size are persisted across sessions
 * - Close button hides the window, a tab button brings it back
 *
 * The video sink attaches to the VideoOutput below via its objectName
 * ("secondaryVideo"), matching the receiver name created by VideoManager.
 *
 ***************************************************************************/

Item {
    id:                         _root

    readonly property string    _geometrySettingsKey:     "SecondaryVideoWindowGeometry"
    readonly property real      _aspectRatio:             9 / 16   // height / width
    readonly property real      _minWidthFactor:          0.10
    readonly property real      _maxWidthFactor:          0.75
    readonly property real      _titleBarHeight:          ScreenTools.defaultFontPixelHeight * 1.75

    property bool               _windowShown:             true
    property bool               _componentComplete:       false

    anchors.fill:               parent
    visible:                    QGroundControl.videoManager.hasSecondaryVideo

    function _saveGeometry() {
        if (!_componentComplete) {
            return;
        }
        QGroundControl.saveGlobalSetting(_geometrySettingsKey, Math.round(windowFrame.x) + "," + Math.round(windowFrame.y) + "," + Math.round(windowFrame.width));
    }

    function _clampToParent() {
        if (!parent) {
            return;
        }
        const pw = parent.width;
        const ph = parent.height;
        if (windowFrame.width > pw * _maxWidthFactor) {
            windowFrame.width = pw * _maxWidthFactor;
        } else if (windowFrame.width < pw * _minWidthFactor) {
            windowFrame.width = pw * _minWidthFactor;
        }
        windowFrame.x = Math.max(0, Math.min(windowFrame.x, pw - windowFrame.width));
        windowFrame.y = Math.max(0, Math.min(windowFrame.y, ph - windowFrame.height));
    }

    // The floating window itself
    Item {
        id:                     windowFrame

        x:                      Math.round(_root.width * 0.60)
        y:                      Math.round(_root.height * 0.10)
        width:                  Math.round(_root.width * 0.25)
        height:                 Math.round(width * _aspectRatio) + _titleBarHeight
        visible:                _root._windowShown && !QGroundControl.videoManager.fullScreen
        z:                      QGroundControl.zOrderWidgets

        Component.onCompleted: {
            const parts = QGroundControl.loadGlobalSetting(_root._geometrySettingsKey, "").split(",");
            if (parts.length === 3) {
                const gx = parseFloat(parts[0]);
                const gy = parseFloat(parts[1]);
                const gw = parseFloat(parts[2]);
                if (!isNaN(gw) && gw > 0) {
                    windowFrame.width = gw;
                }
                if (!isNaN(gx) && !isNaN(gy)) {
                    windowFrame.x = gx;
                    windowFrame.y = gy;
                }
            }
            _root._componentComplete = true;
            _root._clampToParent();
        }

        onWidthChanged: {
            height = Math.round(width * _root._aspectRatio) + _root._titleBarHeight;
            _root._saveGeometry();
        }
        onXChanged:            _root._saveGeometry()
        onYChanged:            _root._saveGeometry()

        Rectangle {
            id:                 windowBackground
            anchors.fill:       parent
            color:              "black"
            border.color:       Qt.rgba(1, 1, 1, 0.35)
            border.width:       1
        }

        // Title bar - drag to move the window
        Rectangle {
            id:                 titleBar
            height:             _root._titleBarHeight
            color:              Qt.rgba(0, 0, 0, 0.65)
            anchors.top:        parent.top
            anchors.left:       parent.left
            anchors.right:      parent.right

            QGCLabel {
                text:           qsTr("Video 2")
                color:          "white"
                font.pointSize: ScreenTools.smallFontPointSize
                anchors.left:   parent.left
                anchors.leftMargin: ScreenTools.defaultFontPixelWidth
                anchors.verticalCenter: parent.verticalCenter
            }

            // Close button
            Rectangle {
                width:          parent.height * 0.75
                height:         width
                radius:         2
                color:          closeMouseArea.pressed ? Qt.rgba(1, 1, 1, 0.45) : Qt.rgba(1, 1, 1, 0.20)
                anchors.right:  parent.right
                anchors.rightMargin: ScreenTools.defaultFontPixelWidth * 0.5
                anchors.verticalCenter: parent.verticalCenter

                QGCLabel {
                    text:       "✕"
                    color:      "white"
                    anchors.centerIn: parent
                }

                MouseArea {
                    id:             closeMouseArea
                    anchors.fill:   parent
                    onClicked:      _root._windowShown = false
                }
            }

            MouseArea {
                id:                 titleDragMouseArea
                anchors.fill:       parent
                preventStealing:    true
                cursorShape:        Qt.SizeAllCursor

                drag.target:        windowFrame
                drag.axis:          Drag.XAndYAxis
                drag.minimumX:      0
                drag.minimumY:      0
                drag.maximumX:      _root.width - windowFrame.width
                drag.maximumY:      _root.height - windowFrame.height

                onReleased:         _root._saveGeometry()
            }
        }

        // Video content
        VideoOutput {
            id:                 secondaryVideoOutput
            objectName:         "secondaryVideo"
            anchors.top:        titleBar.bottom
            anchors.bottom:     parent.bottom
            anchors.left:       parent.left
            anchors.right:      parent.right
            fillMode:           VideoOutput.PreserveAspectFit
        }

        // Resize handle (bottom right corner)
        MouseArea {
            id:                 resizeHandle
            width:              ScreenTools.defaultFontPixelHeight * 2
            height:             ScreenTools.defaultFontPixelHeight * 2
            anchors.right:      parent.right
            anchors.bottom:     parent.bottom
            cursorShape:        Qt.SizeFDiagCursor
            preventStealing:    true
            z:                  1

            property real       _initialX:      0
            property real       _initialWidth:  0

            onPressed: (mouse) => {
                _initialX = mouse.x;
                _initialWidth = windowFrame.width;
            }

            onPositionChanged: (mouse) => {
                if (!pressed) {
                    return;
                }
                const newWidth = _initialWidth + mouse.x - _initialX;
                if ((newWidth <= _root.width * _root._maxWidthFactor) && (newWidth >= _root.width * _root._minWidthFactor)) {
                    windowFrame.width = newWidth;
                }
            }

            onReleased: _root._saveGeometry()
        }

        Image {
            source:         "/qmlimages/pipResize.svg"
            fillMode:       Image.PreserveAspectFit
            mipmap:         true
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height:         ScreenTools.defaultFontPixelHeight * 2
            width:          ScreenTools.defaultFontPixelHeight * 2
            sourceSize.height: height
            z:              2
        }
    }

    // Re-open tab shown while the window is closed
    Rectangle {
        visible:            _root._windowShown === false && !QGroundControl.videoManager.fullScreen
        width:              ScreenTools.defaultFontPixelHeight * 2.5
        height:             ScreenTools.defaultFontPixelHeight * 2.5
        radius:             height / 4
        color:              Qt.rgba(0, 0, 0, 0.65)
        anchors.right:      parent.right
        anchors.rightMargin: ScreenTools.defaultFontPixelWidth
        anchors.verticalCenter: parent.verticalCenter
        z:                  QGroundControl.zOrderWidgets

        Image {
            source:             "/qmlimages/PiP.svg"
            fillMode:           Image.PreserveAspectFit
            mipmap:             true
            width:              parent.width * 0.6
            height:             parent.height * 0.6
            sourceSize.height:  height
            anchors.centerIn:   parent
        }

        MouseArea {
            anchors.fill:   parent
            onClicked: {
                _root._windowShown = true;
                _root._clampToParent();
            }
        }
    }

    // Keep the window inside the view when the parent resizes
    Connections {
        target: _root.parent

        function onWidthChanged() {
            if (_root._componentComplete) {
                _root._clampToParent();
            }
        }

        function onHeightChanged() {
            if (_root._componentComplete) {
                _root._clampToParent();
            }
        }
    }
}
