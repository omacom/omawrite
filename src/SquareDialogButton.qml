import QtQuick
import QtQuick.Controls

Button {
    id: control

    property bool primary: false
    property bool darkMode: true
    property color labelColor: primary ? "#ffffff" : "#d0d0d0"
    property color activeColor: "#428bca"
    property real textScale: 1
    // A label that marks its access key with "&", as Qt writes them ("&Save",
    // "&&" for an ampersand), so each translation picks its own. The dialog
    // binds Alt and the key; the button underlines it.
    property string mnemonicLabel: ""
    readonly property string mnemonic: {
        var match = /&([^&\s])/.exec(mnemonicLabel.replace(/&&/g, ""));
        return match ? match[1].toUpperCase() : "";
    }

    text: mnemonicLabel.replace(/&(&?)/g, "$1")
    // The label as rich text, its access key underlined.
    property string formattedText: {
        var escape = function(c) {
            return c === "&" ? "&amp;" : c === "<" ? "&lt;" : c === ">" ? "&gt;" : c;
        };
        var out = "", marked = false;
        for (var i = 0; i < mnemonicLabel.length; ++i) {
            var c = mnemonicLabel[i];
            if (c === "&" && i + 1 < mnemonicLabel.length) {
                c = mnemonicLabel[++i];
                if (c !== "&" && !marked) {
                    out += "<u>" + escape(c) + "</u>";
                    marked = true;
                    continue;
                }
            }
            out += escape(c);
        }
        return out;
    }

    leftPadding: 16
    rightPadding: 16
    topPadding: 7
    bottomPadding: 7

    Keys.onReturnPressed: clicked()
    Keys.onEnterPressed: clicked()

    contentItem: Label {
        text: control.formattedText.length > 0 ? control.formattedText : control.text
        textFormat: control.formattedText.length > 0 ? Text.StyledText : Text.PlainText
        color: control.labelColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        font.family: "IBM Plex Mono"
        font.pixelSize: Math.round(12 * control.textScale)
    }

    background: Rectangle {
        implicitWidth: 88
        implicitHeight: 34
        radius: 0
        color: control.primary
            ? (control.down ? "#347ab3" : control.hovered ? "#4b96d0" : control.activeColor)
            : control.down
                ? (control.darkMode ? "#2a2a2a" : "#dedede")
                : control.hovered
                    ? (control.darkMode ? "#242424" : "#eeeeee")
                    : (control.darkMode ? "#202020" : "#f6f6f6")
        border.color: control.activeFocus
            ? (control.darkMode ? "#eeeeee" : "#222324")
            : control.primary
                ? "#367eb7"
                : (control.darkMode ? "#424242" : "#c8c8c8")
    }
}
