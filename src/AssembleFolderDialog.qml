import QtQuick
import QtQuick.Controls

Dialog {
    id: root
    objectName: "assembleFolderDialog"

    property var receipt: ({})
    property bool darkMode: true
    property color textColor: darkMode ? "#d0d0d0" : "#42464c"
    property color strongTextColor: darkMode ? "#eeeeee" : "#222324"
    property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    property string fontFamily: "iA Writer Quattro S"
    property real textScale: 1

    readonly property int wordCount: receipt.wordCount || 0
    readonly property string directoryName: receipt.directoryName || ""
    readonly property bool usesUnsavedWork: receipt.usesUnsavedWork === true
    readonly property var roots: receipt.roots || []

    signal printRequested()
    signal saveMarkdownRequested()
    signal savePdfRequested()

    function rootLines() {
        var lines = [];
        for (var i = 0; i < roots.length; ++i) {
            var row = roots[i];
            var line = row.fileName;
            var includes = row.includes || [];
            if (includes.length)
                line += " (includes " + includes.join(", ") + ")";
            lines.push(line);
        }
        return lines.join("\n");
    }

    modal: true
    title: "Assemble this folder"
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 80 : 520, 520)
    standardButtons: Dialog.NoButton

    contentItem: Column {
        spacing: 12
        width: parent.width

        Label {
            width: parent.width
            text: directoryName.length
                  ? "Print or save " + directoryName + " as one document."
                  : "Print or save this folder as one document."
            color: root.strongTextColor
            wrapMode: Text.Wrap
            font.family: root.fontFamily
            font.pixelSize: Math.round(14 * root.textScale)
        }

        Label {
            objectName: "assembleFolderWordCount"
            width: parent.width
            text: wordCount + (wordCount === 1 ? " word" : " words")
            color: root.mutedColor
            font.family: root.fontFamily
            font.pixelSize: Math.round(12 * root.textScale)
        }

        Label {
            width: parent.width
            visible: usesUnsavedWork
            text: "Includes unsaved work from open tabs."
            color: root.mutedColor
            wrapMode: Text.Wrap
            font.family: root.fontFamily
            font.pixelSize: Math.round(12 * root.textScale)
        }

        Label {
            width: parent.width
            visible: roots.length > 20
            text: "This folder has " + roots.length + " documents."
            color: root.mutedColor
            wrapMode: Text.Wrap
            font.family: root.fontFamily
            font.pixelSize: Math.round(12 * root.textScale)
        }

        ScrollView {
            width: parent.width
            implicitHeight: Math.min(220, Math.max(48, rootList.contentHeight))
            clip: true

            Label {
                id: rootList
                objectName: "assembleFolderRoots"
                width: parent.width
                text: root.rootLines()
                color: root.textColor
                wrapMode: Text.Wrap
                font.family: root.fontFamily
                font.pixelSize: Math.round(13 * root.textScale)
            }
        }
    }

    footer: DialogButtonBox {
        alignment: Qt.AlignRight

        Button {
            objectName: "assemblePrintButton"
            text: "Print\u2026"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                root.printRequested();
                root.close();
            }
        }
        Button {
            objectName: "assemblePdfButton"
            text: "Save as PDF\u2026"
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.savePdfRequested()
        }
        Button {
            objectName: "assembleMarkdownButton"
            text: "Save Markdown\u2026"
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.saveMarkdownRequested()
        }
        Button {
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
    }
}
