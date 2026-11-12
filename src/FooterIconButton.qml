import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: control

    property string iconName
    property color iconColor: "#666666"
    property string tooltip
    property color tooltipColor: "#22221f"
    property color tooltipTextColor: "#d0d0d0"
    // The icons are drawn on a 16-pixel grid and scaled to this size.
    property real size: 16

    signal clicked()

    width: size
    height: size

    // Its own tooltip, since Material's is always the same dark grey.
    ToolTip {
        id: tip
        visible: hitArea.containsMouse && control.tooltip.length > 0
        text: control.tooltip
        background: Rectangle {
            radius: 4
            color: control.tooltipColor
        }
        contentItem: Text {
            text: tip.text
            font: tip.font
            color: control.tooltipTextColor
        }
    }

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
            context.setTransform(1, 0, 0, 1, 0, 0);
            context.clearRect(0, 0, width, height);
            var unit = dpr * control.size / 16;
            context.setTransform(unit, 0, 0, unit, 0, 0);
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
            } else if (control.iconName === "font") {
                context.moveTo(3, 13.5);
                context.lineTo(8, 2.5);
                context.lineTo(13, 13.5);
                context.moveTo(5, 9.5);
                context.lineTo(11, 9.5);
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
            function onSizeChanged() { iconCanvas.requestPaint(); }
        }
    }

    MouseArea {
        id: hitArea
        anchors.centerIn: parent
        width: control.size * 1.75
        height: control.size * 1.75
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: control.clicked()
    }
}
