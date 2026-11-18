import QtQuick
import QtQuick.Controls
import QtQuick.Window

ToolButton {
    id: control
    property bool readMode: false
    property color iconColor: "#909191"
    property string shortcutPlatform: Qt.platform.os
    readonly property string shortcutText: (shortcutPlatform === "osx" ? "Cmd" : "Ctrl") + "+Shift+M"
    implicitWidth: 40
    implicitHeight: 40
    padding: 8
    Accessible.name: readMode ? "Edit Markdown" : "Read document"
    ToolTip.visible: hovered
    ToolTip.text: (readMode ? "Edit" : "Read") + " (" + shortcutText + ")"
    contentItem: Item {
        implicitWidth: 24
        implicitHeight: 24
        Canvas {
            id: canvas
            objectName: "previewToggleCanvas"
            // Center the logical 24px glyph, not its high-DPI backing surface.
            x: (parent.width - 24) / 2
            y: (parent.height - 24) / 2
            readonly property real dpr: Screen.devicePixelRatio
            width: 24 * dpr
            height: 24 * dpr
            scale: 1 / dpr
            transformOrigin: Item.TopLeft
            onDprChanged: requestPaint()
            onPaint: {
                var c = getContext("2d");
                c.setTransform(dpr, 0, 0, dpr, 0, 0);
                c.clearRect(0, 0, 24, 24);
                c.strokeStyle = control.iconColor;
                c.lineWidth = 1.6;
                c.lineCap = "round";
                c.lineJoin = "round";
                c.beginPath();
                if (control.readMode) {
                    c.moveTo(5, 15); c.lineTo(16, 4); c.lineTo(20, 8);
                    c.lineTo(9, 19); c.lineTo(4, 20); c.closePath();
                    c.moveTo(14, 6); c.lineTo(18, 10);
                    c.moveTo(5, 15); c.lineTo(9, 19);
                } else {
                    c.moveTo(12, 6); c.quadraticCurveTo(7, 2, 2, 4);
                    c.lineTo(2, 19); c.quadraticCurveTo(7, 17, 12, 21);
                    c.quadraticCurveTo(17, 17, 22, 19); c.lineTo(22, 4);
                    c.quadraticCurveTo(17, 2, 12, 6); c.lineTo(12, 21);
                }
                c.stroke();
            }
            Connections {
                target: control
                function onReadModeChanged() { canvas.requestPaint(); }
                function onIconColorChanged() { canvas.requestPaint(); }
            }
        }
    }
}
