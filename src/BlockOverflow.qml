import QtQuick
import QtQuick.Controls

// Shared, focus-neutral overflow chrome. Text/table contents retain their own
// native hit testing; only horizontal gestures are consumed here.
Item {
    id: overflow
    property real contentWidth: width
    property real viewportWidth: width
    property real offset: 0
    // Local coordinates: keep the bar at the visible block bottom, not the
    // distant end of a tall native paragraph group.
    property real viewportBottom: height
    property alias wheelOrientation: wheelHandler.orientation
    property alias wheelDevices: wheelHandler.acceptedDevices
    signal scrollRequested(real offset)
    WheelHandler {
        id: wheelHandler
        acceptedModifiers: Qt.NoModifier
        onWheel: function(event) {
            var dx = event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.angleDelta.x / 2;
            if (dx === 0 || overflow.contentWidth <= overflow.viewportWidth) { event.accepted = false; return; }
            overflow.scrollRequested(Math.max(0, Math.min(overflow.contentWidth - overflow.viewportWidth, overflow.offset - dx)));
            event.accepted = true;
        }
    }
    ScrollBar {
        objectName: "blockHorizontalScrollBar"
        y: Math.max(0, Math.min(parent.height, overflow.viewportBottom) - height)
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width, overflow.viewportWidth)
        height: 14
        orientation: Qt.Horizontal
        policy: ScrollBar.AlwaysOn
        visible: overflow.viewportBottom > 0 && overflow.contentWidth > overflow.viewportWidth + 0.5
        active: hovered || pressed
        focusPolicy: Qt.NoFocus
        size: Math.min(1, overflow.viewportWidth / Math.max(1, overflow.contentWidth))
        position: overflow.offset / Math.max(1, overflow.contentWidth)
        onPositionChanged: if (pressed) overflow.scrollRequested(position * overflow.contentWidth)
    }
}
