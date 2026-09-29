import QtQuick
import QtQuick.Controls

// A thin strip of open-document tabs. Stateless and prop-driven, matching the
// rest of Omawrite's dialog/button components: the caller supplies the tab
// list and colors, and reacts to tabActivated/tabCloseRequested/newTabRequested.
Row {
    id: root

    property var tabsModel: []
    property int activeIndex: 0
    property bool darkMode: true
    property color textColor: "#eeeeee"
    property color mutedColor: "#909191"
    property color accentColor: "#5584aa"
    property real textScale: 1

    signal tabActivated(int index)
    signal tabCloseRequested(int index)
    signal newTabRequested()

    spacing: Math.round(4 * textScale)
    height: visible ? Math.round(28 * textScale) : 0
    visible: tabsModel.length > 1

    Repeater {
        model: root.tabsModel

        delegate: Rectangle {
            id: tabButton
            required property var modelData
            required property int index

            height: root.height
            width: label.implicitWidth + closeGlyph.implicitWidth
                + Math.round(26 * root.textScale)
            radius: Math.round(6 * root.textScale)
            color: index === root.activeIndex
                ? (root.darkMode ? "#2a2a26" : "#eeeeea")
                : "transparent"

            MouseArea {
                anchors.fill: parent
                onClicked: root.tabActivated(tabButton.index)
            }

            Row {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Math.round(10 * root.textScale)
                spacing: Math.round(6 * root.textScale)

                Label {
                    id: label
                    text: (tabButton.modelData.modified ? "• " : "")
                        + tabButton.modelData.fileName
                    color: tabButton.index === root.activeIndex ? root.textColor : root.mutedColor
                    font.family: "iA Writer Mono S"
                    font.pixelSize: Math.round(12 * root.textScale)
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, Math.round(160 * root.textScale))
                }

                Label {
                    id: closeGlyph
                    text: "×"
                    color: closeArea.containsMouse ? root.textColor : root.mutedColor
                    font.pixelSize: Math.round(14 * root.textScale)

                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        anchors.margins: Math.round(-4 * root.textScale)
                        hoverEnabled: true
                        onClicked: root.tabCloseRequested(tabButton.index)
                    }
                }
            }
        }
    }

    Label {
        id: newTabButton
        text: "+"
        color: newTabArea.containsMouse ? root.textColor : root.mutedColor
        font.pixelSize: Math.round(16 * root.textScale)
        anchors.verticalCenter: parent.verticalCenter
        leftPadding: Math.round(6 * root.textScale)
        rightPadding: Math.round(6 * root.textScale)

        MouseArea {
            id: newTabArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.newTabRequested()
        }
    }
}
