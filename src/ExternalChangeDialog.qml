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
    property color activeButtonLabelColor: "#ffffff"
    property int containerWidth: 520
    property int containerHeight: 320
    property real textScale: 1
    // Closing without a choice, as with Escape, decides nothing destructive:
    // auto-save stays paused over a changed file, and a removed one's text
    // is kept as unsaved, so closing the window still asks about it.
    property bool decided: false

    signal keepRequested()
    signal reloadRequested()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    // Wide enough for the buttons, which run longer in some languages.
    width: Math.min(Math.max(520, dialogButtons.implicitWidth + 40), containerWidth - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 20

    onAboutToShow: decided = false
    onOpened: (deleted ? keepButton : reloadButton).forceActiveFocus()
    // The file can vanish while the dialog is up; Enter must not reload it.
    onDeletedChanged: {
        if (deleted && opened)
            keepButton.forceActiveFocus();
    }
    onClosed: {
        if (!decided && deleted)
            keepRequested();
    }

    background: Rectangle {
        color: root.darkMode ? "#1a1a1a" : "#ffffff"
        border.color: root.darkMode ? "#343434" : "#d8d8d8"
        radius: 0
    }

    contentItem: Column {
        // Popups sit outside the window's mirrored root, so they mirror here.
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        spacing: 12

        Label {
            text: root.deleted ? qsTr("File removed") : qsTr("File changed")
            color: root.strongTextColor
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(16 * root.textScale)
            font.bold: true
        }

        Label {
            width: parent.width
            text: root.deleted
                ? qsTr("This file was removed outside Omawrite. Keep your text as an unsaved document?")
                : (root.locallyModified
                   ? qsTr("This file changed outside Omawrite. Reloading will discard your changes.")
                   : qsTr("This file changed outside Omawrite."))
            color: root.textColor
            wrapMode: Text.Wrap
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(13 * root.textScale)
        }
    }

    footer: Item {
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        implicitHeight: dialogButtons.implicitHeight + 20

        Row {
            id: dialogButtons
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            SquareDialogButton {
                id: keepButton
                objectName: "keepButton"
                text: qsTr("Keep Mine")
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.deleted ? root.activeButtonLabelColor : root.textColor
                primary: root.deleted
                activeColor: root.activeButtonColor
                activeLabelColor: root.activeButtonLabelColor
                KeyNavigation.left: reloadButton
                KeyNavigation.right: reloadButton
                KeyNavigation.tab: reloadButton
                KeyNavigation.backtab: reloadButton
                onClicked: {
                    root.decided = true;
                    root.close();
                    root.keepRequested();
                }
            }

            SquareDialogButton {
                id: reloadButton
                objectName: "reloadButton"
                text: qsTr("Reload")
                // There is nothing to reload from a removed file. Should it
                // come back, the dialog turns into a change and this returns.
                visible: !root.deleted
                enabled: !root.deleted
                primary: true
                darkMode: root.darkMode
                textScale: root.textScale
                activeColor: root.activeButtonColor
                activeLabelColor: root.activeButtonLabelColor
                KeyNavigation.left: keepButton
                KeyNavigation.right: keepButton
                KeyNavigation.tab: keepButton
                KeyNavigation.backtab: keepButton
                onClicked: {
                    root.decided = true;
                    root.close();
                    root.reloadRequested();
                }
            }
        }
    }
}
