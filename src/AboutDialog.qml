import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The mark, the version, and the commit the binary was actually built from.
// Nothing else: an About box is a place to check what you are running, not a
// page to read.
Dialog {
    id: root
    objectName: "aboutDialog"

    property real textScale: 1
    property color textColor: "#222324"
    property color mutedColor: "#909191"
    property color accentColor: "#2077b2"
    property string fontFamily: "iA Writer Quattro S"
    property string version: ""
    property string commit: ""
    property url commitUrl: ""

    function scaled(size) {
        return Math.round(size * root.textScale);
    }

    modal: true
    title: ""
    anchors.centerIn: parent
    padding: root.scaled(28)

    contentItem: ColumnLayout {
        spacing: root.scaled(6)

        Image {
            objectName: "aboutIcon"
            source: "qrc:/omawrite-icon.png"
            // The icon carries its own margin, so it is drawn a little larger
            // than its optical size to sit right above the name.
            sourceSize.width: root.scaled(128)
            sourceSize.height: root.scaled(128)
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: root.scaled(-6)
        }

        Label {
            objectName: "aboutName"
            text: "Omawrite"
            color: root.textColor
            font.family: root.fontFamily
            font.pixelSize: root.scaled(21)
            font.weight: Font.DemiBold
            Layout.alignment: Qt.AlignHCenter
        }

        Label {
            objectName: "aboutVersion"
            text: "Version " + root.version
            color: root.mutedColor
            font.family: root.fontFamily
            font.pixelSize: root.scaled(12)
            Layout.alignment: Qt.AlignHCenter
        }

        // The commit is the useful part when something behaves unexpectedly.
        // Selectable, so it can be pasted into a bug report, and a link when
        // there is a page to read it on.
        TextEdit {
            objectName: "aboutCommit"
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.RichText
            text: root.commitUrl.toString() !== ""
                ? "<a href=\"" + root.commitUrl + "\" style=\"color:"
                  + root.accentColor + ";text-decoration:none\">" + root.commit + "</a>"
                : root.commit
            color: root.accentColor
            font.family: root.fontFamily
            font.pixelSize: root.scaled(12)
            horizontalAlignment: Text.AlignHCenter
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: root.scaled(2)
            onLinkActivated: function(link) { backend.openExternalUrl(link); }

            HoverHandler {
                cursorShape: root.commitUrl.toString() !== ""
                    ? Qt.PointingHandCursor : Qt.IBeamCursor
            }
        }

        Label {
            text: "A dead-simple Markdown writing app"
            color: root.mutedColor
            opacity: 0.8
            font.family: root.fontFamily
            font.pixelSize: root.scaled(11)
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: root.scaled(10)
        }
    }
}
