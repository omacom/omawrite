import QtQuick
import QtQuick.Controls

// Foreground sibling of the negative-z background: never grab body selections.
Item {
    id: controls
    objectName: "codeBlockControls"
    required property var modelData
    required property int index
    required property var preview
    required property bool readMode
    required property color labelColor
    property bool composing: false
    x: modelData.x
    y: modelData.y
    width: modelData.width
    height: modelData.height
    z: 2
    HoverHandler { id: codeHover }
    BlockOverflow {
        anchors.fill: parent
        viewportBottom: controls.preview.viewportBottom < 0 ? height : controls.preview.viewportBottom - controls.y
        contentWidth: controls.modelData.contentWidth
        viewportWidth: controls.modelData.viewportWidth
        offset: controls.preview.blockOffsets[controls.modelData.id] || 0
        onScrollRequested: function(offset) { controls.preview.scrollBlock(controls.modelData.id, offset); }
    }
    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 4
        anchors.rightMargin: 8
        spacing: 8
        Text {
            objectName: "codeBlockLanguage"
            visible: controls.readMode
            text: controls.modelData.language
            textFormat: Text.PlainText
            font.pixelSize: 12
            color: controls.labelColor
            height: 24
            width: Math.min(implicitWidth, Math.max(0, Math.min(140, controls.width - 80)))
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        ToolButton {
            objectName: "tableGridButton"
            text: "Grid"
            visible: !!controls.modelData.tableId
            focusPolicy: Qt.NoFocus
            height: 24
            onClicked: controls.preview.setTableSource(controls.modelData.tableId, false)
        }
        ToolButton {
            id: copyButton
            objectName: "codeBlockCopyButton"
            property bool copied: false
            text: copied ? "Copied" : "Copy"
            width: 64
            height: 24
            padding: 2
            opacity: codeHover.hovered ? 1 : 0
            enabled: codeHover.hovered && !controls.composing
            focusPolicy: Qt.NoFocus
            Accessible.name: "Copy code"
            onClicked: {
                if (controls.preview.copyCodeBlock(controls.index)) {
                    copied = true;
                    copiedReset.restart();
                }
            }
            Timer { id: copiedReset; interval: 1200; onTriggered: copyButton.copied = false }
            ToolTip.visible: hovered
            ToolTip.text: "Copy code"
        }
    }
}
