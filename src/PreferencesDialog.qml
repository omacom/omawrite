import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Everything the app remembers, in one panel. Kept in its own file so the
// editor's own QML stays the size it was.
Dialog {
    id: root
    objectName: "preferencesDialog"

    property bool darkMode: false
    property real textScale: 1
    property color textColor: "#222324"
    property color mutedColor: "#909191"
    property string fontFamily: "iA Writer Mono S"
    property var fontFamilies: []
    // Passed in rather than read off parent.height: a ScrollView whose implicit
    // height depends on its parent's height is a binding loop.
    property real maxContentHeight: 640
    property real preferredWidth: 520

    function scaled(size) {
        return Math.round(size * root.textScale);
    }

    modal: true
    title: "Preferences"
    standardButtons: Dialog.Close
    anchors.centerIn: parent
    width: Math.min(root.preferredWidth, 520)

    contentItem: ScrollView {
        clip: true
        implicitHeight: Math.min(root.maxContentHeight, layout.implicitHeight + 16)

        GridLayout {
            id: layout
            width: root.availableWidth
            columns: 2
            columnSpacing: 16
            rowSpacing: 10

            component SectionLabel: Label {
                Layout.columnSpan: 2
                Layout.topMargin: 8
                color: root.mutedColor
                font.family: root.fontFamily
                font.pixelSize: root.scaled(10)
                font.capitalization: Font.AllUppercase
            }

            component FieldLabel: Label {
                color: root.textColor
                font.family: root.fontFamily
                font.pixelSize: root.scaled(12)
            }

            SectionLabel { text: "Editor" }

            FieldLabel { text: "Typeface" }
            ComboBox {
                id: familyBox
                objectName: "fontFamilyBox"
                Layout.fillWidth: true
                model: root.fontFamilies
                font.family: root.fontFamily
                font.pixelSize: root.scaled(12)
                currentIndex: Math.max(0, root.fontFamilies.indexOf(backend.editorFontFamily))
                onActivated: backend.editorFontFamily = root.fontFamilies[currentIndex]
            }

            FieldLabel { text: "Characters per line" }
            SpinBox {
                objectName: "measureBox"
                Layout.fillWidth: true
                from: 20
                to: 200
                stepSize: 5
                editable: true
                value: backend.editorMeasureChars
                font.family: root.fontFamily
                onValueModified: backend.editorMeasureChars = value
            }

            FieldLabel { text: "Word target" }
            SpinBox {
                objectName: "wordTargetBox"
                Layout.fillWidth: true
                from: 0
                to: 100000
                stepSize: 50
                editable: true
                value: backend.wordTarget
                font.family: root.fontFamily
                // Zero is off, so say so rather than showing a bare 0.
                textFromValue: function(value) {
                    return value === 0 ? "Off" : String(value);
                }
                valueFromText: function(text) {
                    return text.toLowerCase() === "off" ? 0 : parseInt(text, 10) || 0;
                }
                onValueModified: backend.wordTarget = value
            }

            FieldLabel { text: "Printed text size" }
            SpinBox {
                objectName: "printFontPointSizeBox"
                from: 6; to: 32
                value: Math.round(backend.printFontPointSize)
                onValueModified: backend.printFontPointSize = value
            }

            FieldLabel { text: "Return opens a paragraph" }
            Switch {
                objectName: "paragraphOnReturnSwitch"
                checked: backend.paragraphOnReturn
                onToggled: backend.paragraphOnReturn = checked
            }

            SectionLabel { text: "Caret" }

            FieldLabel { text: "Shape" }
            ComboBox {
                objectName: "caretStyleBox"
                Layout.fillWidth: true
                model: ["Line", "Block"]
                font.family: root.fontFamily
                font.pixelSize: root.scaled(12)
                currentIndex: backend.caretStyle === "block" ? 1 : 0
                onActivated: backend.caretStyle = currentIndex === 1 ? "block" : "line"
            }

            FieldLabel { text: "Blink" }
            Switch {
                objectName: "caretBlinkSwitch"
                checked: backend.caretBlink
                onToggled: backend.caretBlink = checked
            }

            SectionLabel { text: "Saving" }

            FieldLabel { text: "Autosave" }
            Switch {
                objectName: "autosaveSwitch"
                checked: backend.autosave
                onToggled: backend.autosave = checked
            }

            FieldLabel { text: "Autosave delay (ms)" }
            SpinBox {
                objectName: "autosaveDelayBox"
                Layout.fillWidth: true
                from: 200
                to: 60000
                stepSize: 250
                editable: true
                enabled: backend.autosave
                value: backend.autosaveDelayMs
                font.family: root.fontFamily
                onValueModified: backend.autosaveDelayMs = value
            }

            SectionLabel { text: "Printing" }

            FieldLabel { text: "Page margin (mm)" }
            SpinBox {
                objectName: "printMarginBox"
                Layout.fillWidth: true
                from: 0
                to: 60
                stepSize: 5
                editable: true
                value: Math.round(backend.printMarginMm)
                font.family: root.fontFamily
                onValueModified: backend.printMarginMm = value
            }
        }
    }
}
