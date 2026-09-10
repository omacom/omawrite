import QtQuick
import QtQuick.Controls

Dialog {
    id: root

    property bool deleted: false
    // A file turned up on a path this document took while it was empty and
    // never read. Unlike the other two cases there is no copy of this text on
    // disk, so Reload is the destructive answer here, not the safe one.
    property bool appeared: false
    property bool locallyModified: false
    readonly property bool keepIsSafer: deleted || appeared
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
    closePolicy: Popup.CloseOnEscape
    width: Math.min(520, containerWidth - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 20

    onOpened: (keepIsSafer ? keepButton : reloadButton).forceActiveFocus()

    background: Rectangle {
        color: root.darkMode ? "#1a1a1a" : "#ffffff"
        border.color: root.darkMode ? "#343434" : "#d8d8d8"
        radius: 0
    }

    contentItem: Column {
        spacing: 12

        Label {
            objectName: "externalChangeHeading"
            text: root.deleted
                ? "File removed"
                : (root.appeared ? "File appeared" : "File changed")
            color: root.strongTextColor
            font.family: "iA Writer Mono S"
            font.pixelSize: Math.round(16 * root.textScale)
            font.bold: true
        }

        Label {
            objectName: "externalChangeMessage"
            width: parent.width
            text: root.deleted
                ? "This file was removed outside Omawrite. Keep your text as an unsaved document?"
                : (root.appeared
                   ? "Something else created this file after Omawrite took the name. None of your text has been written yet, so reloading will discard everything you have typed."
                   : (root.locallyModified
                      ? "This file changed outside Omawrite. Reloading will discard your changes."
                      : "This file changed outside Omawrite."))
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
                objectName: "keepMineButton"
                text: "Keep Mine"
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.keepIsSafer ? "#ffffff" : root.textColor
                primary: root.keepIsSafer
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
                objectName: "reloadButton"
                text: "Reload"
                enabled: !root.deleted
                primary: !root.keepIsSafer
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
