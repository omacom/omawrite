import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: control

    property string iconName
    property color iconColor: "#666666"
    property string tooltip

    signal clicked()

    width: 16
    height: 16

    ToolTip.visible: hitArea.containsMouse && tooltip.length > 0
    ToolTip.text: tooltip

    Canvas {
        id: iconCanvas

        // Canvas rasterizes one surface pixel per logical pixel and gets
        // upscaled blurry on hidpi screens. Draw at the physical size and
        // scale the item back down so surface pixels match screen pixels.
        readonly property real dpr: Screen.devicePixelRatio
        width: control.width * dpr
        height: control.height * dpr
        transformOrigin: Item.TopLeft
        scale: 1 / dpr
        onDprChanged: requestPaint()

        onPaint: {
            var context = getContext("2d");
            context.setTransform(dpr, 0, 0, dpr, 0, 0);
            context.clearRect(0, 0, width, height);
            context.strokeStyle = control.iconColor;
            context.lineWidth = 1.4;
            context.lineCap = "round";
            context.lineJoin = "round";
            context.beginPath();
            if (control.iconName === "save") {
                context.moveTo(2.5, 2.5);
                context.lineTo(10.5, 2.5);
                context.lineTo(13.5, 5.5);
                context.lineTo(13.5, 13.5);
                context.lineTo(2.5, 13.5);
                context.closePath();
                context.moveTo(5.5, 2.5);
                context.lineTo(5.5, 6);
                context.lineTo(10, 6);
                context.lineTo(10, 2.5);
                context.moveTo(4.5, 13.5);
                context.lineTo(4.5, 9.5);
                context.lineTo(11.5, 9.5);
                context.lineTo(11.5, 13.5);
            } else if (control.iconName === "preview") {
                context.moveTo(1.5, 8);
                context.bezierCurveTo(4.5, 4, 11.5, 4, 14.5, 8);
                context.bezierCurveTo(11.5, 12, 4.5, 12, 1.5, 8);
                context.closePath();
                context.moveTo(10, 8);
                context.arc(8, 8, 2, 0, Math.PI * 2);
            } else if (control.iconName === "edit") {
                // A pencil: shown while previewing, because the click goes
                // back to the source rather than deeper into the preview.
                context.moveTo(2.5, 13.5);
                context.lineTo(3.5, 10.5);
                context.lineTo(10.5, 3.5);
                context.lineTo(12.5, 5.5);
                context.lineTo(5.5, 12.5);
                context.closePath();
                context.moveTo(9.5, 4.5);
                context.lineTo(11.5, 6.5);
            } else {
                context.moveTo(2.5, 13);
                context.lineTo(2.5, 3.5);
                context.lineTo(6.5, 3.5);
                context.lineTo(8.5, 5.5);
                context.lineTo(13.5, 5.5);
                context.lineTo(13.5, 13);
                context.closePath();
            }
            context.stroke();
        }

        Connections {
            target: control
            function onIconColorChanged() { iconCanvas.requestPaint(); }
            function onIconNameChanged() { iconCanvas.requestPaint(); }
        }
    }

    MouseArea {
        id: hitArea
        anchors.centerIn: parent
        width: 28
        height: 28
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.clicked()
    }
}
