import QtQuick
import QtQuick.Controls

Dialog {
    id: root

    property string fileName: "Untitled.md"
    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color activeButtonColor: "#428bca"
    property color activeButtonLabelColor: "#ffffff"
    property int containerWidth: 420
    property int containerHeight: 320
    property real textScale: 1

    signal saveRequested()
    signal discardRequested()
    signal cancelRequested()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    onRejected: cancelRequested()

    onOpened: saveButton.forceActiveFocus()
    // Wide enough for the buttons, which run longer in some languages.
    width: Math.min(Math.max(420, dialogButtons.implicitWidth + 40), containerWidth - 48)
    x: Math.round((containerWidth - width) / 2)
    y: Math.round((containerHeight - height) / 2)
    padding: 20

    // Escape cancels; each button's key picks it, with or without Alt. There
    // is nothing to type into here, so the bare letter is enough.
    Shortcut {
        sequences: [cancelButton.mnemonic, "Alt+" + cancelButton.mnemonic]
        enabled: root.opened && cancelButton.mnemonic !== ""
        onActivated: cancelButton.clicked()
    }

    Shortcut {
        sequences: [discardButton.mnemonic, "Alt+" + discardButton.mnemonic]
        enabled: root.opened && discardButton.mnemonic !== ""
        onActivated: discardButton.clicked()
    }

    Shortcut {
        sequences: [saveButton.mnemonic, "Alt+" + saveButton.mnemonic]
        enabled: root.opened && saveButton.mnemonic !== ""
        onActivated: saveButton.clicked()
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
            text: qsTr("Unsaved changes")
            color: root.strongTextColor
            font.family: "IBM Plex Mono"
            font.pixelSize: Math.round(16 * root.textScale)
            font.bold: true
        }

        Label {
            width: parent.width
            text: qsTr("Save changes to %1 first?").arg(root.fileName)
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
                id: cancelButton
                mnemonicLabel: qsTr("&Cancel")
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.textColor
                KeyNavigation.left: saveButton
                KeyNavigation.right: discardButton
                KeyNavigation.tab: discardButton
                KeyNavigation.backtab: saveButton
                onClicked: root.reject()
            }

            SquareDialogButton {
                id: discardButton
                mnemonicLabel: qsTr("&Discard")
                darkMode: root.darkMode
                textScale: root.textScale
                labelColor: root.textColor
                KeyNavigation.left: cancelButton
                KeyNavigation.right: saveButton
                KeyNavigation.tab: saveButton
                KeyNavigation.backtab: cancelButton
                onClicked: {
                    root.close();
                    root.discardRequested();
                }
            }

            SquareDialogButton {
                id: saveButton
                mnemonicLabel: qsTr("&Save")
                primary: true
                darkMode: root.darkMode
                textScale: root.textScale
                activeColor: root.activeButtonColor
                activeLabelColor: root.activeButtonLabelColor
                KeyNavigation.left: discardButton
                KeyNavigation.right: cancelButton
                KeyNavigation.tab: cancelButton
                KeyNavigation.backtab: discardButton
                onClicked: {
                    root.close();
                    root.saveRequested();
                }
            }
        }
    }
}
