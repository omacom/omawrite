import QtQuick
import QtQuick.Controls

Dialog {
    id: root

    property bool deleted: false
    property bool locallyModified: false
    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color activeButtonColor: "#428bca"
    property int containerWidth: 520
    property int containerHeight: 320
    property real textScale: 1

    signal keepRequested()
    signal reloadRequested()

    modal: true
    focus: true
    // Both ways out are answers. Escape would be a third, and every meaning it
    // could be given — keep, reload, or neither — decides the thing being
    // asked, so it is not offered.
    closePolicy: Popup.NoAutoClose
    width: Math.min(520, containerWidth - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 20

    onOpened: (deleted ? keepButton : reloadButton).forceActiveFocus()

    background: Rectangle {
        color: root.darkMode ? "#1a1a1a" : "#ffffff"
        border.color: root.darkMode ? "#343434" : "#d8d8d8"
        radius: 0
    }

    contentItem: Column {
        spacing: 12

        Label {
            text: root.deleted ? "File removed" : "File changed"
            color: root.strongTextColor
            font.family: "iA Writer Mono S"
            font.pixelSize: Math.round(16 * root.textScale)
            font.bold: true
        }

        Label {
            width: parent.width
            text: root.deleted
                ? "This file was removed outside Omawrite. Keep your text as an unsaved document?"
                : (root.locallyModified
                   ? "This file changed outside Omawrite. Reloading will discard your changes."
                   : "This file changed outside Omawrite.")
            color: root.textColor
            wrapMode: Text.Wrap
            font.family: "iA Writer Mono S"
            font.pixelSize: Math.round(13 * root.textScale)
        }
    }

    footer: Item {
        implicitHeight: dialogButtons.implicitHeight + 20

        Row {
            id: dialogButtons
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            SquareDialogButton {
                id: keepButton
                text: "Keep Mine"
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.deleted ? "#ffffff" : root.textColor
                primary: root.deleted
                activeColor: root.activeButtonColor
                KeyNavigation.left: reloadButton
                KeyNavigation.right: reloadButton
                KeyNavigation.tab: reloadButton
                KeyNavigation.backtab: reloadButton
                onClicked: {
                    root.close();
                    root.keepRequested();
                }
            }

            SquareDialogButton {
                id: reloadButton
                text: "Reload"
                enabled: !root.deleted
                primary: true
                darkMode: root.darkMode
                textScale: root.textScale
                activeColor: root.activeButtonColor
                KeyNavigation.left: keepButton
                KeyNavigation.right: keepButton
                KeyNavigation.tab: keepButton
                KeyNavigation.backtab: keepButton
                onClicked: {
                    root.close();
                    root.reloadRequested();
                }
            }
        }
    }
}
