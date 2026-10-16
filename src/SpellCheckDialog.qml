import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A review bar, like Find, so the writing remains the main surface.
Popup {
    id: root
    required property var checker
    property color panelColor: "#22221f"
    property color foregroundColor: "#eeeeee"
    property color mutedColor: "#909191"
    property color accentColor: "#428bca"
    property color selectionColor: "#454545"
    property string editorFont: "IBM Plex Mono"
    property real textScale: 1
    property real maximumHeight: 440
    property int scopeStart: 0
    property int scopeEnd: -1
    property bool choosingLanguages: false
    property int suggestionIndex: 0
    readonly property var issue: checker.current
    readonly property bool hasIssue: issue.word !== undefined
    readonly property var suggestions: issue.suggestions || []
    readonly property string languageSummary: checker.languages.map(languageName).join(", ")

    focus: true
    modal: false
    padding: scaled(12)
    height: review.implicitHeight + topPadding + bottomPadding
    closePolicy: Popup.CloseOnEscape
    Material.elevation: 8
    background: Rectangle { color: root.panelColor; radius: 9 }

    function scaled(value) { return Math.max(1, Math.round(value * textScale)); }
    function languageName(id) {
        for (var i = 0; i < checker.dictionaries.length; ++i) {
            var dictionary = checker.dictionaries[i];
            if (dictionary.id === id) return dictionary.name.split(" — ")[0];
        }
        return id;
    }
    function begin(from, to) {
        scopeStart = from;
        scopeEnd = to;
        checker.refreshDictionaries();
        open();
        checker.start(from, to);
        choosingLanguages = checker.languages.length === 0 || checker.dictionaries.length === 0;
    }
    function selectLanguage(id, enabled) {
        var languages = checker.languages.slice();
        var index = languages.indexOf(id);
        if (enabled && index < 0) languages.push(id);
        if (!enabled && index >= 0) languages.splice(index, 1);
        checker.languages = languages;
    }
    function preferLanguage(id) {
        var languages = checker.languages.slice();
        var index = languages.indexOf(id);
        if (index >= 0) languages.splice(index, 1);
        languages.unshift(id);
        checker.languages = languages;
    }
    function chooseSuggestion(index) {
        if (!suggestions.length) return;
        suggestionIndex = (index + suggestions.length) % suggestions.length;
        replacement.text = suggestions[suggestionIndex];
        replacement.forceActiveFocus();
    }
    Shortcut {
        sequence: "Escape"
        context: Qt.WindowShortcut
        enabled: root.opened
        onActivated: {
            if (root.choosingLanguages) root.choosingLanguages = false;
            else if (actions.opened) actions.close();
            else root.close();
        }
    }
    onClosed: {
        choosingLanguages = false;
        actions.close();
        checker.cancel();
    }
    Connections {
        target: root.checker
        function onTextInvalidated() { root.scopeStart = 0; root.scopeEnd = -1; }
        function onReplacementApplied(difference) {
            if (root.scopeEnd >= 0) root.scopeEnd += difference;
        }
        function onChanged() {
            root.suggestionIndex = 0;
            replacement.text = !root.hasIssue ? ""
                : root.suggestions.length ? root.suggestions[0] : root.issue.word;
            if (root.hasIssue && root.opened && !root.choosingLanguages)
                replacement.forceActiveFocus();
        }
    }

    component QuietButton: AbstractButton {
        id: control
        property bool emphasized: false
        property bool alignLeft: false
        implicitWidth: label.implicitWidth + leftPadding + rightPadding
        implicitHeight: root.scaled(30)
        leftPadding: root.scaled(7)
        rightPadding: root.scaled(7)
        activeFocusOnTab: true
        Accessible.name: text
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        contentItem: Text {
            id: label
            text: control.text
            color: !control.enabled ? root.mutedColor
                : control.emphasized ? root.accentColor : root.foregroundColor
            opacity: control.enabled ? 1 : 0.5
            font.family: root.editorFont
            font.pixelSize: root.scaled(12)
            horizontalAlignment: control.alignLeft ? Text.AlignLeft : Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 4
            color: root.foregroundColor
            opacity: control.down ? 0.15 : control.hovered || control.activeFocus ? 0.08 : 0
        }
    }

    contentItem: ColumnLayout {
        id: review
        spacing: root.scaled(4)
        RowLayout {
            spacing: root.scaled(4)
            Label {
                text: root.scopeEnd < 0 ? qsTr("Spelling") : qsTr("Spelling · selection")
                color: root.mutedColor
                font.family: root.editorFont
                font.pixelSize: root.scaled(11)
            }
            Item { Layout.fillWidth: true }
            QuietButton {
                objectName: "spellingLanguagesButton"
                text: root.languageSummary || qsTr("Choose language…")
                Layout.maximumWidth: root.availableWidth * 0.6
                onClicked: root.choosingLanguages = !root.choosingLanguages
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Spelling languages")
            }
            QuietButton {
                text: "×"
                Accessible.name: qsTr("Close spell check")
                onClicked: root.close()
            }
        }
        RowLayout {
            visible: root.hasIssue
            spacing: root.scaled(6)
            Label {
                text: root.issue.word || ""
                textFormat: Text.PlainText
                color: root.foregroundColor
                font.family: root.editorFont
                font.pixelSize: root.scaled(16)
                elide: Text.ElideRight
                Layout.maximumWidth: root.availableWidth * 0.23
                ToolTip.visible: wordHover.hovered
                ToolTip.text: text
                HoverHandler { id: wordHover }
            }
            Label {
                text: "→"
                color: root.mutedColor
                font.pixelSize: root.scaled(16)
            }
            Item {
                Layout.fillWidth: true
                Layout.minimumWidth: root.scaled(40)
                Layout.preferredHeight: root.scaled(32)
                TextInput {
                    id: replacement
                    objectName: "spellingReplacement"
                    anchors.fill: parent
                    anchors.bottomMargin: 2
                    verticalAlignment: TextInput.AlignVCenter
                    font.family: root.editorFont
                    font.pixelSize: root.scaled(16)
                    color: root.foregroundColor
                    selectionColor: root.selectionColor
                    selectByMouse: true
                    activeFocusOnTab: true
                    clip: true
                    Accessible.name: qsTr("Replacement")
                    Keys.onReturnPressed: if (text.length) root.checker.replace(text)
                    Keys.onEnterPressed: if (text.length) root.checker.replace(text)
                    Keys.onDownPressed: root.chooseSuggestion(root.suggestionIndex + 1)
                    Keys.onUpPressed: root.chooseSuggestion(root.suggestionIndex - 1)
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: root.mutedColor
                    opacity: replacement.activeFocus ? 0.7 : 0.25
                }
            }
            QuietButton {
                objectName: "replaceSpelling"
                text: qsTr("Replace")
                emphasized: true
                enabled: replacement.text.length > 0
                onClicked: root.checker.replace(replacement.text)
            }
            QuietButton { text: qsTr("Skip"); onClicked: root.checker.skip(false) }
            QuietButton {
                id: moreButton
                text: "⋯"
                Accessible.name: qsTr("More spelling actions")
                onClicked: actions.opened ? actions.close() : actions.open()
            }
        }
        Flickable {
            objectName: "spellingSuggestions"
            visible: root.hasIssue && root.suggestions.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: root.scaled(28)
            contentWidth: suggestionRow.width
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Row {
                id: suggestionRow
                spacing: root.scaled(4)
                Repeater {
                    model: root.suggestions
                    QuietButton {
                        required property string modelData
                        required property int index
                        text: modelData
                        emphasized: replacement.text === modelData
                        onClicked: root.chooseSuggestion(index)
                    }
                }
            }
        }
        RowLayout {
            visible: !root.hasIssue
            Label {
                objectName: "spellingStatus"
                text: root.checker.status
                color: root.foregroundColor
                font.family: root.editorFont
                font.pixelSize: root.scaled(13)
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            QuietButton {
                objectName: "startSpellCheck"
                text: qsTr("Check")
                visible: !root.checker.busy
                enabled: root.checker.languages.length > 0
                onClicked: root.checker.start(root.scopeStart, root.scopeEnd)
            }
        }
    }

    Popup {
        id: actions
        parent: root.contentItem
        x: parent.width - width
        y: parent.height + root.scaled(8)
        width: Math.min(root.availableWidth, root.scaled(300))
        padding: root.scaled(6)
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: root.panelColor; radius: 9; border.color: root.selectionColor }
        contentItem: Column {
            QuietButton {
                width: parent.width
                alignLeft: true
                text: qsTr("Ignore all occurrences")
                onClicked: { root.checker.skip(true); actions.close(); }
            }
            Repeater {
                model: root.checker.languages
                QuietButton {
                    required property string modelData
                    width: parent.width
                    alignLeft: true
                    text: qsTr("Add to %1").arg(root.languageName(modelData))
                    onClicked: { root.checker.addWord(modelData); actions.close(); }
                }
            }
        }
    }

    Popup {
        id: languages
        objectName: "spellingLanguagesPopup"
        parent: root.contentItem
        x: parent.width - width
        y: parent.height + root.scaled(8)
        width: Math.min(root.availableWidth, root.scaled(420))
        height: Math.min(root.maximumHeight, languageContent.implicitHeight + padding * 2)
        padding: root.scaled(12)
        visible: root.choosingLanguages
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root.choosingLanguages = false
        background: Rectangle { color: root.panelColor; radius: 9; border.color: root.selectionColor }
        contentItem: ScrollView {
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                id: languageContent
                width: parent.width
                spacing: root.scaled(5)
                Label {
                    text: qsTr("Check against")
                    font.family: root.editorFont
                    font.pixelSize: root.scaled(12)
                    color: root.mutedColor
                }
                Repeater {
                    model: root.checker.dictionaries
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        QuietButton {
                            Layout.fillWidth: true
                            alignLeft: true
                            text: (root.checker.languages.indexOf(modelData.id) >= 0 ? "✓  " : "   ")
                                + root.languageName(modelData.id)
                            onClicked: root.selectLanguage(modelData.id, root.checker.languages.indexOf(modelData.id) < 0)
                        }
                        QuietButton {
                            visible: root.checker.languages.indexOf(modelData.id) >= 0 && root.checker.languages.length > 1
                            text: root.checker.languages[0] === modelData.id ? qsTr("First") : qsTr("Prefer")
                            emphasized: root.checker.languages[0] === modelData.id
                            onClicked: root.preferLanguage(modelData.id)
                        }
                    }
                }
                Repeater {
                    model: root.checker.languages.filter(function(id) {
                        return !root.checker.dictionaries.some(function(d) { return d.id === id; });
                    })
                    QuietButton {
                        required property string modelData
                        Layout.fillWidth: true
                        alignLeft: true
                        text: qsTr("✓  %1 (missing — click to remove)").arg(modelData)
                        onClicked: root.selectLanguage(modelData, false)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Accept words from any selected language. Prefer a language to show its suggestions first.")
                    wrapMode: Text.Wrap
                    color: root.mutedColor
                    font.family: root.editorFont
                    font.pixelSize: root.scaled(11)
                }
                QuietButton {
                    id: setup
                    property bool expanded: root.checker.dictionaries.length === 0
                    text: expanded ? qsTr("Hide dictionary setup") : qsTr("Add languages…")
                    onClicked: expanded = !expanded
                }
                Label {
                    visible: setup.expanded
                    Layout.fillWidth: true
                    text: qsTr("Install Hunspell dictionaries, or put matching .aff and .dic files in %1. Reopen spelling to refresh the list. On Omarchy, English (US) is hunspell-en_us.").arg(root.checker.dictionaryDirectory)
                    wrapMode: Text.Wrap
                    color: root.mutedColor
                    font.family: root.editorFont
                    font.pixelSize: root.scaled(11)
                }
                QuietButton {
                    text: qsTr("Check spelling")
                    emphasized: true
                    enabled: root.checker.languages.length > 0
                    onClicked: {
                        root.choosingLanguages = false;
                        root.checker.start(root.scopeStart, root.scopeEnd);
                    }
                }
            }
        }
    }
}
