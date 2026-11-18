import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as Dialogs

Menu {
    id: root
    objectName: "attachmentMenu"
    required property var attachmentBackend
    required property var editorItem
    property string imagePath: ""
    property bool readOnly: false

    function openFor(path, px, py) {
        imagePath = path;
        popup(px, py);
    }

    MenuItem {
        objectName: "copyImageAction"
        text: "Copy Image"
        onTriggered: root.attachmentBackend.copyImage(root.imagePath)
    }
    MenuItem {
        objectName: "renameAttachmentAction"
        visible: !root.readOnly
        enabled: !root.readOnly
        height: visible ? implicitHeight : 0
        text: "Name / folder…"
        onTriggered: {
            pathField.text = root.imagePath;
            renameDialog.open();
            pathField.forceActiveFocus();
            pathField.selectAll();
        }
    }
    MenuItem {
        objectName: "locateAttachmentAction"
        visible: !root.readOnly
        enabled: !root.readOnly
        height: visible ? implicitHeight : 0
        text: "Locate image…"
        onTriggered: locateDialog.open()
    }

    Dialog {
        id: renameDialog
        objectName: "renameAttachmentDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(440, parent ? parent.width - 32 : 440)
        modal: true
        title: "Image name / folder"
        standardButtons: Dialog.Save | Dialog.Cancel
        contentItem: Column {
            spacing: 12
            Label {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Relative to this note, for example img/diagram.png. Updates this document only; the original file is kept for undo."
            }
            TextField {
                id: pathField
                objectName: "attachmentPathField"
                width: parent.width
                selectByMouse: true
                onAccepted: renameDialog.accept()
            }
        }
        onAccepted: root.attachmentBackend.renameAttachment(root.imagePath, pathField.text)
        onClosed: root.editorItem.forceActiveFocus()
    }
    Dialogs.FileDialog {
        id: locateDialog
        title: "Locate image"
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.svg)", "All files (*)"]
        onAccepted: {
            root.attachmentBackend.locateAttachment(root.imagePath, selectedFile);
            root.editorItem.forceActiveFocus();
        }
        onRejected: root.editorItem.forceActiveFocus()
    }
}
