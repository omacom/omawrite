import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations
import Omawrite 1.0

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: !backend.routesFileOpen
    signal windowCloseAccepted()
    readonly property var visibleEditor: presentation
    property var activeTablePanel: null
    readonly property var activeTableCell: activeTablePanel ? activeTablePanel.activeEditor : null
    readonly property bool tableExpanded: tableDialog.visible
    readonly property int expandedTableId: tableDialog.tableData ? tableDialog.tableData.id : 0
    function commitTableCell() {
        if (tableDialog.visible && expandedTableLoader.item && !expandedTableLoader.item.commitCell()) return false;
        return foregroundDecorations.commitTables() && !livePreview.tableDraftsPending;
    }
    function expandTable(panel) {
        if (!commitTableCell()) return;
        tableDialog.tableData = panel.modelData;
        tableDialog.open();
    }
    function updateExpandedTable() {
        if (!tableDialog.visible) return;
        var panels = livePreview.tablePanels;
        for (var n = 0; n < panels.length; ++n) {
            if (panels[n].id === expandedTableId && panels[n].model === tableDialog.tableData.model) {
                tableDialog.tableData = panels[n]; return;
            }
        }
        // A removed/replaced session must never keep an editor on new source.
        if (expandedTableLoader.item) expandedTableLoader.item.discardCell();
        tableDialog.close();
    }
    Dialog {
        id: tableDialog
        objectName: "expandedTableDialog"
        property var tableData: null
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.NoAutoClose
        title: win.readMode ? "Table" : "Edit table"
        width: Math.max(1, win.width - 48)
        height: Math.max(1, win.height - 48)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        function requestClose() {
            if (expandedTableLoader.item && !expandedTableLoader.item.commitCell()) return;
            close();
        }
        onClosed: {
            foregroundDecorations.restorePositions();
            presentation.forceActiveFocus();
        }
        contentItem: Item {
            focus: true
            Keys.onEscapePressed: function(event) { tableDialog.requestClose(); event.accepted = true; }
            Loader {
                id: expandedTableLoader
                anchors.fill: parent
                active: tableDialog.visible
                sourceComponent: Component {
                    TableGrid {
                        id: expandedGrid
                        objectName: "expandedTableGrid"
                        modelData: tableDialog.tableData
                        preview: livePreview
                        readOnly: win.readMode
                        expanded: true
                        x: 0; y: 0
                        width: expandedTableLoader.width
                        height: expandedTableLoader.height
                        foreground: win.textColor
                        backgroundColor: win.darkMode ? "#252525" : "#eeeeee"
                        borderColor: Qt.rgba(win.textColor.r, win.textColor.g, win.textColor.b, 0.2)
                        editorFont: presentation.font
                        onSourceRequested: tableDialog.close()
                        onEditorChanged: function(editor) {
                            if (editor) win.activeTablePanel = expandedGrid;
                            else if (win.activeTablePanel === expandedGrid) win.activeTablePanel = null;
                        }
                    }
                }
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "closeExpandedTableButton"
                text: "Close"
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: tableDialog.requestClose()
            }
        }
    }
    Connections { target: livePreview; function onTablePanelsChanged() { win.updateExpandedTable(); } }
    ToolButton {
        objectName: "pendingTableDraftsButton"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.rightMargin: 64
        z: 8
        text: "Pending table edits"
        visible: livePreview.tableDraftsPending
        focusPolicy: Qt.NoFocus
        onClicked: tableDraftMenu.popup()
        Menu {
            id: tableDraftMenu
            MenuItem { text: "Copy pending cell drafts"; onTriggered: livePreview.copyTableDrafts(0) }
            MenuItem {
                text: "Discard pending cell drafts"
                onTriggered: {
                    if (win.activeTablePanel) win.activeTablePanel.discardCell();
                    livePreview.discardTableDrafts();
                }
            }
        }
    }
    title: (backend.modified ? "* " : "") + backend.fileName + " - " + qsTr("Write")

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
    // Keep prose softer than headings without fading links, selection or UI.
    readonly property color bodyTextColor: Qt.rgba(
        pageColor.r + (textColor.r - pageColor.r) * 0.76,
        pageColor.g + (textColor.g - pageColor.g) * 0.76,
        pageColor.b + (textColor.b - pageColor.b) * 0.76, 1)
    readonly property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    readonly property color selectionFill: backend.themeSelection
    readonly property color selectionTextColor: backend.themeSelectionForeground
    // The theme's shade for panels, which the search bar, font picker and
    // tooltips sit on.
    readonly property color panelColor: backend.themePanelBackground
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around, times the user's own
    // text size set with Ctrl+= / Ctrl+- / Ctrl+wheel.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(20)
    readonly property int footerHeight: scaledSize(32)
    // Never wider than the Flickable's viewport, whatever the floor asks for:
    // a tiling compositor can resize the window below its minimum width.
    readonly property int editorWidth: Math.min(
        Math.round(writerFontMetrics.averageCharacterWidth * 65),
        Math.max(360, width - Math.round(writerFontMetrics.averageCharacterWidth * 20)),
        Math.max(0, width - 48))
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    property url pendingOpenUrl
    property string pendingAction: ""
    property bool replaceOpen: false
    property bool awaitingPendingSave: false
    property alias readMode: livePreview.readMode
    onReadModeChanged: {
        if (presentation) presentation.restartCaret();
        selectionCountDelay.restart();
        if (positionReady && !restoringReadingPosition) positionSaveDelay.restart();
    }
    property bool projectionChanging: false
    property bool restoringReadingPosition: false
    property int scrollSourceAnchor: 0
    property real scrollAnchorOffset: 0
    property bool lastProjectionReadMode: false
    property bool useRelativeScrollAnchor: false
    property real scrollAnchorRatio: 0
    property url positionFileUrl: ""
    property bool positionReady: false
    property bool documentReplacing: false
    property var pendingReadingPosition: null
    property int positionEpoch: 0

    function rememberPosition() {
        if (!positionReady || projectionChanging || restoringReadingPosition) return;
        var top = presentation.positionAt(0, Math.max(0, editorFlick.contentY - presentation.y));
        backend.rememberReadingPosition(positionFileUrl,
            Math.max(0, Math.min(1, editorFlick.contentY / Math.max(1, editorFlick.contentHeight - editorFlick.height))),
            readMode, editor.cursorPosition, readMode ? -1 : livePreview.sourcePosition(top),
            editorFlick.contentY - presentation.y - presentation.positionToRectangle(top).y);
    }
    function restoreFilePosition() {
        documentReplacing = false;
        positionFileUrl = backend.fileUrl;
        pendingReadingPosition = backend.readingPosition(positionFileUrl);
        restoringReadingPosition = true;
        positionReady = false;
        editor.cursorPosition = Math.max(0, Math.min(editor.length, pendingReadingPosition.caret || 0));
        livePreview.readMode = pendingReadingPosition.readMode === true;
        livePreview.refresh();
    }
    function applyReadingPosition() {
        if (pendingReadingPosition === null) return;
        var ratio = pendingReadingPosition.ratio || 0;
        var target = ratio * Math.max(0, editorFlick.contentHeight - editorFlick.height);
        if (!readMode && pendingReadingPosition.anchor !== undefined && pendingReadingPosition.anchor >= 0) {
            var point = presentation.positionToRectangle(livePreview.viewPosition(pendingReadingPosition.anchor));
            target = presentation.y + point.y + (pendingReadingPosition.offset || 0);
        }
        editorFlick.scrollTo(editorFlick.clampContentY(target));
        pendingReadingPosition = null;
        positionReady = true;
        var epoch = positionEpoch;
        Qt.callLater(function() { if (epoch === win.positionEpoch) win.restoringReadingPosition = false; });
    }
    Timer {
        id: positionSaveDelay
        interval: 750
        onTriggered: win.rememberPosition()
    }
    Timer {
        id: selectionCountDelay
        interval: 120
        onTriggered: backend.countSelectedText(win.readMode ? presentation.selectedText : editor.selectedText)
    }

    LivePreview {
        id: livePreview
        objectName: "livePreviewController"
        resolver: backend
        spelling: backend.spelling
        onCopyHtmlRequested: win.copyHtml()
        onToggleReadModeRequested: win.toggleReadMode()
        onFormatCodeRequested: function(fenced) { editor.wrapCode(fenced); }
        onProjectionReady: win.applyReadingPosition()
        onProjectionAboutToChange: {
            win.projectionChanging = true;
            win.useRelativeScrollAnchor = readMode || win.lastProjectionReadMode;
            win.scrollAnchorRatio = editorFlick.contentY
                / Math.max(1, editorFlick.contentHeight - editorFlick.height);
            var top = Math.max(0, editorFlick.contentY - presentation.y);
            var position = presentation.positionAt(0, top);
            win.scrollSourceAnchor = sourcePosition(position);
            win.scrollAnchorOffset = editorFlick.contentY
                - presentation.y - presentation.positionToRectangle(position).y;
        }
        onProjectionChanged: {
            var position = viewPosition(win.scrollSourceAnchor);
            var targetY = win.useRelativeScrollAnchor
                ? win.scrollAnchorRatio * Math.max(0, editorFlick.contentHeight - editorFlick.height)
                : presentation.y + presentation.positionToRectangle(position).y + win.scrollAnchorOffset;
            editorFlick.scrollTo(editorFlick.clampContentY(targetY));
            win.lastProjectionReadMode = readMode;
            win.projectionChanging = false;
            win.applyReadingPosition();
        }
    }

    AttachmentMenu {
        id: attachmentMenu
        parent: presentation
        attachmentBackend: backend
        editorItem: presentation
        readOnly: win.readMode
    }

    function openAttachmentMenuAt(px, py) {
        // Native glyph hits handle both image halves and inline images whose
        // trailing insertion point wraps to another line. Blank space is not a hit.
        var path = livePreview.imagePathAtPoint(px, py);
        if (!path.length) return false;
        attachmentMenu.openFor(path, px, py);
        return true;
    }

    // Pointer hit-testing must not turn blank space into the nearest word.
    // Keyboard menus intentionally use the insertion point instead.
    function showEditorMenuAt(px, py, token) {
        if (openAttachmentMenuAt(px, py)) return true;
        editorContextMenu.word = token;
        editorContextMenu.linkUrl = win.readMode ? "" : presentation.linkUrlUnder(px, py);
        editorContextMenu.popup(px, py);
        return true;
    }
    function openEditorMenuAt(px, py) {
        return showEditorMenuAt(px, py, livePreview.spellingWordAtPoint(px, py));
    }
    function openEditorMenuForCaret() {
        var box = presentation.cursorRectangle;
        return showEditorMenuAt(box.x, box.y + box.height / 2,
                                livePreview.spellingWordAt(presentation.cursorPosition));
    }

    Menu {
        id: editorContextMenu
        objectName: "editorContextMenu"
        parent: presentation
        property var word: ({})
        property bool hasWord: word.word !== undefined
        property string linkUrl: ""
        MenuItem {
            objectName: "openLinkAction"
            text: "Open Link"
            visible: editorContextMenu.linkUrl.length > 0
            enabled: visible
            height: visible ? implicitHeight : 0
            onTriggered: presentation.activateLink(editorContextMenu.linkUrl)
        }
        MenuSeparator { visible: editorContextMenu.linkUrl.length > 0; height: visible ? implicitHeight : 0 }

        Instantiator {
            model: editorContextMenu.hasWord ? editorContextMenu.word.suggestions : []
            delegate: MenuItem {
                required property string modelData
                text: modelData
                onTriggered: backend.replaceSpelling(editorContextMenu.word, modelData)
            }
            onObjectAdded: (index, object) => editorContextMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => editorContextMenu.removeItem(object)
        }
        MenuItem {
            visible: editorContextMenu.hasWord
            enabled: false
            height: visible ? implicitHeight : 0
            text: editorContextMenu.word.pending ? "Finding suggestions…" : "Spelling suggestions"
        }
        MenuItem {
            objectName: "ignoreSpellingAction"
            visible: editorContextMenu.hasWord
            height: visible ? implicitHeight : 0
            text: "Ignore for this session"
            onTriggered: backend.spelling.ignore(editorContextMenu.word)
        }
        MenuItem {
            objectName: "learnSpellingAction"
            visible: editorContextMenu.hasWord
            height: visible ? implicitHeight : 0
            text: "Learn word"
            onTriggered: backend.spelling.learn(editorContextMenu.word)
        }
        MenuSeparator { visible: editorContextMenu.hasWord; height: visible ? implicitHeight : 0 }
        MenuItem { text: "Undo"; enabled: !win.readMode && editor.canUndo; onTriggered: editor.undo() }
        MenuItem { text: "Redo"; enabled: !win.readMode && editor.canRedo; onTriggered: editor.redo() }
        MenuSeparator {}
        MenuItem { text: "Cut"; enabled: !win.readMode && presentation.selectedText.length > 0; onTriggered: editor.cut() }
        MenuItem { objectName: "copyAction"; text: "Copy"; enabled: win.readMode || presentation.selectedText.length > 0; onTriggered: livePreview.copySource() }
        MenuItem { objectName: "copyHtmlAction"; text: "Copy as HTML"; onTriggered: win.copyHtml() }
        MenuItem {
            text: "Paste"
            enabled: !win.readMode && (presentation.canPaste || backend.clipboardImageAvailable)
            onTriggered: { livePreview.prepareEdit(); editor.pasteFromClipboard(); }
        }
        MenuItem { text: "Select All"; onTriggered: presentation.selectAll() }
        MenuItem {
            objectName: "revealCurrentFileAction"
            text: Qt.platform.os === "osx" ? "Reveal in Finder"
                : Qt.platform.os === "windows" ? "Reveal in File Explorer" : "Reveal in File Manager"
            enabled: backend.fileUrl.toString().length > 0
            onTriggered: backend.revealCurrentFile()
        }
        MenuSeparator {}
        Menu {
            title: "Spelling"
            objectName: "spellingMenu"
            MenuItem {
                objectName: "spellingEnabledAction"
                text: "Check spelling"
                checkable: true
                checked: backend.spelling.enabled
                onTriggered: backend.spelling.enabled = !backend.spelling.enabled
            }
            MenuItem {
                text: backend.spelling.languages.length ? "Choose dictionary" : "No dictionaries installed"
                enabled: false
            }
            Instantiator {
                model: backend.spelling.languages
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.label
                    checkable: true
                    checked: backend.spelling.language === modelData.id
                    onTriggered: backend.spelling.language = modelData.id
                }
                onObjectAdded: (index, object) => spellingLanguageMenu.insertItem(index + 2, object)
                onObjectRemoved: (index, object) => spellingLanguageMenu.removeItem(object)
            }
            id: spellingLanguageMenu
        }
        Connections {
            target: backend.spelling
            function onMenuChanged() {
                if (!editorContextMenu.visible || !editorContextMenu.hasWord) return;
                if (!backend.spelling.validate(editorContextMenu.word)) {
                    editorContextMenu.word = ({});
                    return;
                }
                editorContextMenu.word = backend.spelling.wordAt(editorContextMenu.word.start);
            }
        }
        onClosed: presentation.forceActiveFocus()
    }

    function toggleReadMode() {
        if (!commitTableCell()) return;
        if (livePreview.readMode) {
            // Restore the reading scroll first, then choose a NEW caret in
            // the visible text. Focusing the old off-screen caret must not
            // pull the Flickable back to where editing last stopped.
            win.restoringReadingPosition = true;
            livePreview.readMode = false;
            var centerY = editorFlick.contentY + editorFlick.height / 2 - presentation.y;
            presentation.cursorPosition = presentation.positionAt(presentation.width / 2, Math.max(0, centerY));
            presentation.forceActiveFocus();
            Qt.callLater(function() { win.restoringReadingPosition = false; });
        } else {
            livePreview.readMode = true;
            presentation.forceActiveFocus();
        }
    }

    Shortcut {
        sequence: "Ctrl+Shift+M"
        onActivated: win.toggleReadMode()
    }

    Connections {
        target: backend
        function onFileUrlChanged() {
            livePreview.refresh();
            if (!win.documentReplacing && win.positionReady) {
                win.rememberPosition(); // Save As keeps this viewport, not the destination's old position.
                win.positionFileUrl = backend.fileUrl;
                win.rememberPosition();
            }
        }
        function onDocumentAboutToReplace() {
            if (expandedTableLoader.item) expandedTableLoader.item.discardCell();
            foregroundDecorations.abandonEditors();
            livePreview.resetTableSessions();
            win.rememberPosition();
            ++win.positionEpoch;
            win.documentReplacing = true;
            win.positionReady = false;
            win.restoringReadingPosition = true;
            positionSaveDelay.stop();
        }
        function onDocumentLoaded() { win.restoreFilePosition(); }
        function onAttachmentUrlsChanged() { livePreview.refresh(); }
        function onThemeColorsChanged() { livePreview.refresh(); }
    }

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        if (!commitTableCell()) { close.accepted = false; return; }
        if (closeConfirmed || !backend.modified) {
            win.rememberPosition();
            backend.finishClosing();
            win.windowCloseAccepted();
            return;
        }

        close.accepted = false;
        pendingAction = "close";
        if (!unsavedChangesDialog.opened)
            unsavedChangesDialog.open();
    }

    function requestOpen(url) {
        if (!commitTableCell()) return;
        if (backend.routesFileOpen || !backend.modified) {
            backend.open(url);
            return;
        }
        pendingOpenUrl = url;
        pendingAction = "open";
        unsavedChangesDialog.open();
    }

    function completePendingAction() {
        var action = pendingAction;
        pendingAction = "";
        if (action === "close") {
            closeConfirmed = true;
            close();
        } else if (action === "open") {
            backend.open(pendingOpenUrl);
        }
    }

    FontMetrics {
        id: writerFontMetrics
        font.family: backend.editorFont
        font.pixelSize: win.editorFontPixelSize
    }

    // Every hardcoded size in the interface is expressed at text scale 1.
    function scaledSize(pixels) {
        return Math.max(1, Math.round(pixels * win.textScale));
    }

    function toggleFullScreen() {
        win.visibility = win.visibility === Window.FullScreen
            ? Window.Windowed
            : Window.FullScreen;
    }

    // Only the search field and the next and previous controls select a
    // match. When the document changes underneath, the matches follow along
    // without taking the writer's caret.
    function updateSearch(selectMatch) {
        searchDelay.stop();
        var previous = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : 0;
        searchMatches = backend.searchMatches(searchField.text);
        searchMatchIndex = selectMatch ? (searchMatches.length > 0 ? 0 : -1)
                                       : matchAtOrAfter(previous);
        showSearchMatch(selectMatch);
    }

    function matchAtOrAfter(position) {
        for (var i = 0; i < searchMatches.length; ++i) {
            if (searchMatches[i] >= position)
                return i;
        }
        return searchMatches.length > 0 ? 0 : -1;
    }

    function showSearchMatch(selectMatch) {
        var start = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : -1;
        searchUpdating = true;
        backend.setSearchHighlight(searchField.text, start);
        livePreview.setSearchHighlight(searchField.text, start);
        if (selectMatch && start >= 0) {
            // Search is source-based. Switch back to Edit so a match inside a
            // collapsed block is exposed before selecting it.
            if (win.readMode) win.readMode = false;
            editor.select(start, start + searchField.text.length);
            Qt.callLater(function() { editorFlick.ensureCursorVisible(); });
        }
        searchUpdating = false;
    }

    // Typing in the search field waits for a pause before searching, so
    // anything that acts on the matches catches up with the field first.
    function flushSearch() {
        if (searchDelay.running)
            updateSearch(true);
    }

    function moveSearch(direction) {
        // A pending search lands on its first match, which is where the
        // writer meant to go, rather than one past it.
        if (searchDelay.running) {
            updateSearch(true);
            return;
        }
        if (searchMatches.length === 0)
            return;
        searchMatchIndex = (searchMatchIndex + direction + searchMatches.length)
                           % searchMatches.length;
        showSearchMatch(true);
    }

    // The matches stop following the field and the text while the bar is
    // closed, and a search still waiting for a pause is dropped on closing,
    // so opening the bar searches again for what the field holds.
    function openSearch(withReplace) {
        if (!commitTableCell()) return;
        searchOpen = true;
        if (withReplace)
            replaceOpen = true;
        searchField.forceActiveFocus();
        searchField.selectAll();
        // A search still waiting in an open bar lands on its first match.
        updateSearch(searchDelay.running);
    }

    function closeSearch() {
        searchDelay.stop();
        // Hand focus to the editor before hiding the bar. Hiding the field
        // that holds focus leaves the input method with nothing focused for
        // a moment, and fcitx5 can then miss the next Ctrl+F.
        Qt.inputMethod.reset();
        presentation.forceActiveFocus();
        searchOpen = false;
        searchUpdating = true;
        backend.setSearchHighlight("", -1);
        livePreview.setSearchHighlight("", -1);
        editor.deselect();
        searchUpdating = false;
        replaceOpen = false;
    }

    // Restyling a large document for each letter typed makes typing a word
    // stall, so the search waits until the typing pauses.
    Timer {
        id: searchDelay
        interval: 150
        onTriggered: win.updateSearch(true)
    }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: if (win.commitTableCell()) backend.save()
    }

    Shortcut {
        sequence: Qt.platform.os === "osx" ? "Ctrl+Alt+F" : "Ctrl+H"
        onActivated: win.openSearch(true)
    }

    Shortcut {
        sequence: "Ctrl+B"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("**")
    }

    Shortcut {
        sequence: "Ctrl+I"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("*")
    }

    Shortcut {
        sequence: "Ctrl+U"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("<u>")
    }

    Shortcut {
        sequence: "Ctrl+Shift+X"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("~~")
    }

    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertLink()
    }

    Shortcut {
        sequence: "Ctrl+Shift+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertReference(false)
    }

    Shortcut {
        sequence: "Ctrl+Alt+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertReference(true)
    }

    Shortcut {
        sequence: "Ctrl+T"
        context: Qt.WindowShortcut
        onActivated: editor.wrapCode(false)
    }

    Shortcut {
        sequence: "Ctrl+Shift+T"
        context: Qt.WindowShortcut
        onActivated: editor.wrapCode(true)
    }

    Shortcut {
        sequences: ["Ctrl+?", "Ctrl+/", "F1"]
        onActivated: shortcutsDialog.open()
    }

    Shortcut {
        // Ctrl++ is a different shortcut to Qt, not a shifted Ctrl+=, and it is
        // the only one a layout that puts + on its own key can reach.
        sequences: ["Ctrl+=", "Ctrl++"]
        onActivated: backend.increaseTextSize()
    }

    Shortcut {
        sequence: "Ctrl+-"
        onActivated: backend.decreaseTextSize()
    }

    Shortcut {
        sequence: "Ctrl+0"
        onActivated: backend.resetTextSize()
    }

    Shortcut {
        sequence: "Ctrl+O"
        onActivated: if (win.commitTableCell()) backend.openDialog()
    }

    Shortcut {
        sequence: "Ctrl+N"
        onActivated: backend.newWindow()
    }

    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: if (win.commitTableCell()) backend.saveAsDialog()
    }

    Shortcut {
        sequence: "Ctrl+P"
        onActivated: if (win.commitTableCell()) backend.printDocument()
    }

    function copyHtml() {
        if (!commitTableCell()) return;
        if (win.readMode && presentation.selectionStart !== presentation.selectionEnd) {
            if (!livePreview.copyReadSelection(true))
                backend.copyAsHtml(presentation.selectionStart, presentation.selectionEnd, presentation.textDocument);
        }
        else if (!win.readMode && editor.selectionStart !== editor.selectionEnd)
            backend.copyAsHtml(editor.selectionStart, editor.selectionEnd);
        else backend.copyAsHtml();
    }
    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: win.copyHtml()
    }

    Shortcut {
        sequences: Qt.platform.os === "osx" ? ["Ctrl+Meta+F", "F11"] : ["Meta+F", "F11"]
        onActivated: toggleFullScreen()
    }

    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !win.readMode && !win.activeTableCell
        context: Qt.WindowShortcut
        onActivated: editor.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: !win.readMode && !win.activeTableCell
        context: Qt.WindowShortcut
        onActivated: editor.redo()
    }

    Shortcut {
        sequence: "Ctrl+F"
        onActivated: win.openSearch(false)
    }

    Shortcut {
        sequence: "Ctrl+G"
        enabled: win.searchOpen
        onActivated: win.moveSearch(1)
    }

    Connections {
        target: backend

        function onOpenDialogRequested() {
            openFileDialog.open();
        }

        function onSaveDialogRequested(suggestedUrl) {
            saveFileDialog.selectedFile = suggestedUrl;
            saveFileDialog.open();
        }

        function onSaveSucceeded() {
            // Only a save the writer chose in the prompt carries out the
            // pending close or open, never an auto-save that lands meanwhile.
            const confirmed = win.awaitingPendingSave;
            win.awaitingPendingSave = false;
            if (confirmed && win.pendingAction !== "")
                win.completePendingAction();
        }

        function onSaveFailed() {
            // A failed save leaves the document as it was, so the close or
            // open it was for is off; a later save must not carry it out.
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }
    }

    Dialogs.FileDialog {
        id: openFileDialog
        title: qsTr("Open File")
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: [qsTr("Markdown files") + " (*.md *.markdown)", qsTr("All files") + " (*)"]
        onAccepted: win.requestOpen(selectedFile)
    }

    Dialogs.FileDialog {
        id: saveFileDialog
        title: qsTr("Save File")
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: [qsTr("Markdown files") + " (*.md *.markdown)", qsTr("All files") + " (*)"]
        onAccepted: if (win.commitTableCell()) backend.saveAs(selectedFile)
        onRejected: {
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        fileName: backend.fileName
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        activeButtonLabelColor: backend.themeAccentForeground
        containerWidth: win.width
        containerHeight: win.height

        // Discard must still mean discard, so nothing is auto-saved while
        // the writer decides.
        onAboutToShow: backend.holdAutoSave(true)
        onClosed: backend.holdAutoSave(false)

        onDiscardRequested: {
            backend.discardRecovery();
            win.completePendingAction();
        }

        onSaveRequested: {
            win.awaitingPendingSave = true;
            backend.save();
        }
        onCancelRequested: win.pendingAction = ""
    }

    ExternalChangeDialog {
        id: externalChangeDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.textColor
        activeButtonColor: backend.themeAccent
        activeButtonLabelColor: backend.themeAccentForeground
        containerWidth: win.width
        containerHeight: win.height

        onKeepRequested: backend.keepExternalVersion()
        onReloadRequested: if (win.commitTableCell()) backend.reloadFromDisk()
    }

    ShortcutsDialog {
        id: shortcutsDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        activeButtonColor: backend.themeAccent
        activeButtonLabelColor: backend.themeAccentForeground
        containerWidth: win.width
        containerHeight: win.height
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Flickable {
            id: editorFlick
            objectName: "editorScroll"
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            // Inset the scrolling area by the footer strip at both ends, so
            // scrolled text never runs under the footer buttons, status, or
            // word count, and stops just as far short of the top edge.
            anchors.topMargin: win.footerHeight
            anchors.bottomMargin: win.footerHeight
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, presentation.y + presentation.implicitHeight + 220)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                // Wheel scrolling moves contentY directly rather than
                // flicking the Flickable, so the bar has to be told about
                // that activity; linger briefly after the last event.
                active: hovered || pressed || wheelScroll.running || scrollLinger.running
            }

            // Also lingers after paging through the document from the
            // keyboard, so the bar shows where the jump landed. Writing past
            // the bottom edge scrolls too, but leaves the bar hidden.
            Timer {
                id: scrollLinger
                objectName: "scrollLinger"
                interval: 600
            }

            // Flickable turns a wheel notch into a flick sized by the small
            // application font, which crawls next to a browser. Reproduce
            // Chromium's wheel physics instead (cc::ScrollOffsetAnimationCurve):
            // each notch moves 3 lines of 40px towards a running target, the
            // animation gets shorter as the outstanding distance grows, and a
            // notch landing mid-animation carries the current velocity into
            // the new curve, so sustained spinning keeps picking up speed.
            readonly property real wheelStep: win.scaledSize(120)

            FrameAnimation {
                id: wheelScroll
                running: false

                property real startY: 0
                property real targetY: 0
                property real duration: 0.2
                // Cubic bezier easing; ease-in-out (0.42, 0, 0.58, 1) for a
                // fresh scroll, with y1 tilted on retarget so the curve's
                // initial slope matches the velocity it inherits.
                property real cx1: 0.42
                property real cy1: 0
                readonly property real cx2: 0.58
                readonly property real cy2: 1

                onTriggered: {
                    var x = elapsedTime / duration;
                    if (x >= 1) {
                        editorFlick.contentY = editorFlick.snapToPixel(targetY);
                        stop();
                        return;
                    }
                    editorFlick.contentY = editorFlick.snapToPixel(
                        startY + (targetY - startY) * curveY(solveCurve(x)));
                }

                function begin(from, to, dur, slope) {
                    startY = from;
                    targetY = to;
                    duration = dur;
                    cx1 = 0.42;
                    cy1 = 0.42 * Math.max(-1000, Math.min(1000, slope));
                    restart();
                }

                function retarget(newTarget) {
                    var s = solveCurve(Math.min(1, elapsedTime / duration));
                    var pos = startY + (targetY - startY) * curveY(s);
                    var delta = newTarget - pos;
                    if (Math.abs(delta) < 0.5) {
                        editorFlick.contentY = newTarget;
                        stop();
                        return;
                    }

                    var velocity = curveDY(s) / Math.max(1e-6, curveDX(s))
                        * (targetY - startY) / duration;
                    var dur = editorFlick.wheelDuration(delta);
                    // When already moving faster than the eased curve would,
                    // bound the duration by the time to target at the current
                    // velocity; the 2.5x covers the ease-out tail.
                    if (velocity !== 0 && delta / velocity > 0)
                        dur = Math.min(dur, delta / velocity * 2.5);
                    begin(pos, newTarget, dur, velocity * dur / delta);
                }

                // Cubic bezier through (0,0), (cx1,cy1), (cx2,cy2), (1,1),
                // evaluated by Newton-solving the curve parameter from x.
                function curveX(s) { return 3 * s * (1 - s) * ((1 - s) * cx1 + s * cx2) + s * s * s; }
                function curveY(s) { return 3 * s * (1 - s) * ((1 - s) * cy1 + s * cy2) + s * s * s; }
                function curveDX(s) { return 3 * (1 - s) * (1 - s) * cx1 + 6 * (1 - s) * s * (cx2 - cx1) + 3 * s * s * (1 - cx2); }
                function curveDY(s) { return 3 * (1 - s) * (1 - s) * cy1 + 6 * (1 - s) * s * (cy2 - cy1) + 3 * s * s * (1 - cy2); }

                function solveCurve(x) {
                    var s = x;
                    for (var i = 0; i < 8; ++i) {
                        var error = curveX(s) - x;
                        if (Math.abs(error) < 0.001)
                            break;
                        var d = curveDX(s);
                        if (Math.abs(d) < 1e-6)
                            break;
                        s = Math.max(0, Math.min(1, s - error / d));
                    }
                    return s;
                }
            }

            WheelHandler {
                // Wayland compositors route every pointer's scroll through
                // one seat device that Qt classifies as a touchpad, so the
                // device type cannot tell a mouse wheel from two-finger
                // scrolling. Distinguish by event shape instead: discrete
                // wheel notches arrive with only angleDelta set, while
                // finger scrolling carries pixel-precise pixelDelta.
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function(wheel) {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        editorFlick.zoomByWheel(wheel);
                        wheel.accepted = true;
                        return;
                    }

                    scrollLinger.restart();
                    if (wheel.pixelDelta.y !== 0)
                        editorFlick.scrollTo(editorFlick.clampContentY(editorFlick.contentY - wheel.pixelDelta.y));
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: wheelScroll.stop()
            onContentYChanged: {
                editor.hoveredLinkUrl = "";
                if (win.positionReady && !win.projectionChanging && !win.restoringReadingPosition) positionSaveDelay.restart();
            }

            // Ctrl+wheel changes the text size, as it does in a browser. A
            // wheel notch is a whole step, but a touchpad trickles in pixels,
            // so bank the fractions and step once a whole one is scrolled.
            property real zoomNotches: 0

            function zoomByWheel(wheel) {
                // Finger scrolling synthesizes an angleDelta too, so pixelDelta
                // is the one to test first, as in onWheel.
                var notches = wheel.pixelDelta.y !== 0
                    ? wheel.pixelDelta.y / 50
                    : wheel.angleDelta.y / 120;
                if (zoomNotches * notches < 0)
                    zoomNotches = 0;

                zoomNotches += notches;
                while (zoomNotches >= 1) {
                    backend.increaseTextSize();
                    zoomNotches -= 1;
                }
                while (zoomNotches <= -1) {
                    backend.decreaseTextSize();
                    zoomNotches += 1;
                }
            }

            function scrollByWheel(wheel) {
                // High-resolution wheels report fractional notches; feed
                // those through the same animated path, like Chromium does
                // for every wheel-source event.
                var notches = wheel.angleDelta.y / 120;
                if (notches === 0)
                    return;

                if (wheelScroll.running) {
                    wheelScroll.retarget(clampContentY(wheelScroll.targetY - notches * wheelStep));
                    return;
                }

                var target = clampContentY(contentY - notches * wheelStep);
                if (target !== contentY)
                    wheelScroll.begin(contentY, target, wheelDuration(target - contentY), 0);
            }

            // Chromium's inverse-delta duration: 200ms for a single notch,
            // ramping down to 100ms once 480px are outstanding.
            function wheelDuration(delta) {
                var pixels = Math.abs(delta) / win.textScale;
                return Math.max(6, Math.min(12, 14 - pixels / 60)) / 60;
            }

            function clampContentY(y) {
                return Math.max(0, Math.min(Math.max(0, contentHeight - height), y));
            }

            // Whole device pixels keep natively hinted glyphs from
            // re-rasterizing mid-animation, which reads as shimmer.
            function snapToPixel(y) {
                return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio;
            }

            // Jump to a position, abandoning any wheel animation still running.
            function scrollTo(y) {
                wheelScroll.stop();
                contentY = snapToPixel(y);
            }

            // Keep the editing caret within the viewport so writing past the
            // bottom edge scrolls the page along with the text.
            function ensureCursorVisible() {
                var margin = win.editorFontPixelSize * 2;
                var cursorTop = presentation.y + presentation.cursorRectangle.y;
                var cursorBottom = cursorTop + presentation.cursorRectangle.height;
                var maxContentY = Math.max(0, contentHeight - height);

                if (cursorBottom + margin > contentY + height)
                    scrollTo(Math.min(maxContentY, cursorBottom + margin - height));
                else if (cursorTop - margin < contentY)
                    scrollTo(Math.max(0, cursorTop - margin));
            }

            TextEdit {
                id: editor
                objectName: "sourceEditor"
                visible: false
                x: Math.round((editorFlick.width - width) / 2)
                // The resting position counts from the window's top edge, so
                // take back the top inset the Flickable already adds.
                y: Math.max(0, Math.max(42, Math.round(win.height * 0.05)) - win.footerHeight)
                width: win.editorWidth
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                text: ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                activeFocusOnPress: true
                color: win.bodyTextColor
                selectedTextColor: win.selectionTextColor
                selectionColor: win.selectionFill
                font.family: backend.editorFont
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                // Native rendering hints glyphs to the pixel grid, which is
                // crispest at whole scale factors but misplaces and unevenly
                // rasterizes glyphs at fractional ones (and goes stale when
                // the compositor delivers the fractional scale after the
                // first frame). Fall back to Qt's scalable renderer there.
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                cursorDelegate: Rectangle {
                    width: 1
                    color: win.textColor
                }
                // Hidden source geometry is not the visible projection.
                onCursorRectangleChanged: {}
                onCursorPositionChanged: backend.setCaretPosition(cursorPosition)
                onFontChanged: backend.documentFontChanged()

                // The document's own characters, where `text` turns non-breaking
                // spaces into spaces; read this for text that is written back.
                function rawText() {
                    return backend.documentText();
                }

                // One edit command, so a single undo takes the change back.
                function replaceText(start, end, replacement) {
                    if (win.readMode) return;
                    backend.replaceRange(start, end, replacement);
                }

                function replaceSelectionWith(replacement) {
                    EditorMutations.replaceRange(editor, selectionStart, selectionEnd, replacement);
                }

                function wrapSelection(marker) {
                    if (win.activeTableCell) return;
                    if (win.readMode) return;
                    presentation.forceActiveFocus();
                    if (marker === "<u>") livePreview.prepareFormatting();
                    var span = backend.emphasisSpanAt(cursorPosition, marker);
                    if (marker === "<u>") EditorMutations.toggleUnderline(editor, span);
                    else EditorMutations.toggleWrap(editor, marker, span);
                }

                function wrapCode(fenced) {
                    if (win.activeTableCell) return;
                    if (win.readMode) return;
                    presentation.forceActiveFocus();
                    livePreview.prepareFormatting();
                    if (fenced) EditorMutations.toggleCodeBlock(editor);
                    else EditorMutations.toggleInlineCode(editor);
                }

                function insertLink() {
                    if (win.activeTableCell) return;
                    if (win.readMode) return;
                    presentation.forceActiveFocus();
                    var start = selectionStart;
                    var end = selectionEnd;
                    var selected = rawText().slice(start, end);
                    var url = backend.clipboardUrl();
                    var label = selected.length > 0 ? selected : qsTr("link text");
                    var markdown = markdownLink(label, url.length > 0 ? url : "https://");
                    var labelLength = escapeMarkdownLinkText(label).length;
                    if (selected.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     1, 1 + labelLength);
                    } else if (url.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     labelLength + 3,
                                                     markdown.length - 1);
                    } else {
                        EditorMutations.replaceRange(editor, start, end, markdown);
                    }
                }

                function insertReference(footnote) {
                    if (win.activeTableCell || win.readMode) return;
                    presentation.forceActiveFocus();
                    EditorMutations.insertReference(editor, backend.nextReferenceLabel(footnote), footnote,
                        backend.clipboardUrl(), backend.clipboardText(), qsTr("link text"), qsTr("Footnote text"),
                        backend.insideFence(Math.max(0, rawText().length - 1)));
                }

                function smartReturn(softBreak) {
                    if (softBreak) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var lineStart = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                    // Raw, as its indentation is copied onto the next line.
                    var line = rawText().slice(lineStart, cursorPosition);
                    if (backend.insideFence(cursorPosition)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var match = line.match(/^(\s*)([-+*]|\d+[.)]|>+)\s+(.*)$/);
                    if (match) {
                        if (match[3].length === 0) {
                            EditorMutations.replaceRange(editor, lineStart,
                                                         cursorPosition, "\n");
                        } else {
                            var marker = match[2];
                            if (/^\d/.test(marker))
                                marker = (parseInt(marker) + 1) + marker.slice(-1);
                            replaceSelectionWith("\n" + match[1] + marker + " ");
                        }
                        return;
                    }
                    // A new paragraph stands apart from the one above it by a
                    // blank line, which is the second break here. A line that
                    // is already blank has nothing to stand apart from, so
                    // that break would only be a gap nobody asked for.
                    // The break lands on what the selection leaves behind, which
                    // is not the caret's line when it was dragged right to left.
                    var start = selectionStart;
                    var end = selectionEnd;
                    var head = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                    var lineEnd = text.indexOf("\n", end);
                    var rest = lineEnd < 0 ? text.slice(end)
                                           : text.slice(end, lineEnd);
                    if (/^\s*$/.test(head) && /^\s*$/.test(rest)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    replaceSelectionWith("\n\n");
                }

                // Tab nests the list items the caret or selection touches four
                // spaces deeper, and Shift+Tab lifts them back out by the same
                // step. Lines that are neither items, their indented
                // continuations, nor blank, and code fences, leave Tab alone.
                function indentListItems(outdent) {
                    // "* * *" and "- - -" are rules, not items.
                    var listItem = function(line) {
                        return /^[ \t]*(?:[-+*]|\d+[.)])[ \t]/.test(line)
                            && !/^ {0,3}([-*])([ \t]*\1){2,}[ \t]*$/.test(line);
                    };
                    // A tab runs to the next four-column stop.
                    var columns = function(line) {
                        return line.match(/^[ \t]*/)[0].split("").reduce(function(column, c) {
                            return c === "\t" ? column + 4 - column % 4 : column + 1;
                        }, 0);
                    };

                    // A selection that ends at the start of a line leaves it be.
                    // lastIndexOf would read -1 as 0, a newline right there.
                    var start = selectionStart > 0 ? text.lastIndexOf("\n", selectionStart - 1) + 1 : 0;
                    var end = text.indexOf("\n", Math.max(start, selectionEnd - 1));
                    if (end < 0)
                        end = text.length;
                    // A line separator breaks a line on screen as a newline does,
                    // and each line keeps the one it had.
                    var parts = rawText().slice(start, end).split(/(\n|\u2028)/);
                    var lines = parts.filter(function(part, i) { return i % 2 === 0; });
                    // The selection starts at an item and holds only lines that
                    // belong to one: other items, blanks, and indented lines.
                    // A fence indented under an item moves along with it whole;
                    // a selection that starts or ends inside code would split it.
                    var filled = lines.filter(function(line) { return /\S/.test(line); });
                    if (!listItem(filled[0] || "")
                        || filled.some(function(line) { return /^\S/.test(line) && !listItem(line); })
                        || backend.insideFence(start)
                        || backend.insideFence(end))
                        return false;

                    // Every line moves by the same step, so the nesting within
                    // the selection is kept. Four spaces nest under any marker
                    // up to "99.", and Shift+Tab lifts no further than the
                    // shallowest line can go.
                    var step = outdent ? -Math.min.apply(null, [4].concat(filled.map(columns))) : 4;
                    if (step === 0)
                        return true;
                    var moved = lines.map(function(line) {
                        var lead = line.match(/^[ \t]*/)[0];
                        return /\S/.test(line)
                            ? " ".repeat(columns(line) + step) + line.slice(lead.length)
                            : line;
                    });
                    var nested = moved.map(function(line, i) {
                        return i > 0 ? parts[2 * i - 1] + line : line;
                    }).join("");
                    var from = Math.max(start, selectionStart + moved[0].length - lines[0].length);
                    // Neither end of the selection leaves its line, even when
                    // the indent it sat in is gone.
                    var lastStart = start + nested.length - moved[moved.length - 1].length;
                    var to = Math.max(from, lastStart, selectionEnd + nested.length - (end - start));
                    var forward = cursorPosition === selectionEnd;
                    replaceText(start, end, nested);
                    select(forward ? from : to, forward ? to : from);
                    return true;
                }

                function escapeMarkdownLinkText(linkText) {
                    return linkText.replace(/\\/g, "\\\\")
                                   .replace(/\[/g, "\\[")
                                   .replace(/\]/g, "\\]");
                }

                function escapeMarkdownLinkDestination(linkUrl) {
                    return linkUrl.replace(/\\/g, "\\\\")
                                  .replace(/\(/g, "\\(")
                                  .replace(/\)/g, "\\)");
                }

                function markdownLink(label, url) {
                    return "[" + escapeMarkdownLinkText(label) + "]("
                        + escapeMarkdownLinkDestination(url) + ")";
                }

                function pasteClipboardUrlAsMarkdownLink() {
                    var start = selectionStart;
                    var end = selectionEnd;
                    if (start === end)
                        return false;

                    var url = backend.clipboardUrl();
                    if (url === "")
                        return false;

                    var selected = rawText().slice(start, end);
                    var leading = selected.match(/^\s*/)[0];
                    var trailing = selected.match(/\s*$/)[0];
                    var linkText = selected.slice(leading.length,
                                                  selected.length - trailing.length);
                    if (linkText === "")
                        return false;

                    replaceSelectionWith(leading + markdownLink(linkText, url) + trailing);
                    return true;
                }

                function pasteClipboardAsPlainText() {
                    var pastedText = backend.clipboardText();
                    if (pastedText.length > 0)
                        replaceSelectionWith(pastedText);
                }

                function moveCursorVisibly(direction) {
                    if (selectionStart !== selectionEnd) {
                        cursorPosition = direction > 0 ? selectionEnd : selectionStart;
                        return;
                    }

                    var pos = backend.cursorPositionStep(cursorPosition, direction);
                    cursorPosition = Math.max(0, Math.min(length, pos));
                }

                function movePage(direction, extendSelection) {
                    var pageStep = Math.max(win.editorFontPixelSize,
                                            editorFlick.height - win.editorFontPixelSize * 2);
                    var rect = presentation.cursorRectangle;
                    var targetY = rect.y + rect.height / 2 + direction * pageStep;
                    var target = livePreview.sourcePosition(presentation.positionAt(rect.x, Math.max(0, targetY)));
                    if (extendSelection)
                        moveCursorSelection(target, TextEdit.SelectCharacters);
                    else
                        cursorPosition = target;
                }

                function deleteParagraphBreakBehindCursor() {
                    if (selectionStart !== selectionEnd || cursorPosition < 2)
                        return false;

                    // getText reads just the two characters, where text would
                    // copy the whole document on every Backspace.
                    if (getText(cursorPosition - 2, cursorPosition) !== "\n\n")
                        return false;

                    var start = cursorPosition - 2;
                    var lineEnd = text.indexOf("\n", cursorPosition);
                    var line = lineEnd < 0 ? text.slice(cursorPosition)
                                           : text.slice(cursorPosition, lineEnd);
                    // Something on the caret's line and the pair above it is a
                    // paragraph break and nothing else, either the one Return
                    // wrote to end a paragraph or the one the writer is now
                    // closing to join what it separates. Both breaks go.
                    if (/^\s*$/.test(line)) {
                        // On a blank line Return writes a single break, so up
                        // here the pair may be that break and one that was
                        // already in the document. Above a blank line Return
                        // writes a single break too, and so wrote neither of
                        // these.
                        var above = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                        if (/^\s*$/.test(above))
                            return false;
                        // Past that the two Returns leave the same text and the
                        // same caret, and no reading of either says which was
                        // pressed. What decides instead is that the gap is left
                        // standing: both breaks go only while a blank line of
                        // it survives them. The end of the document below the
                        // caret leaves it none...
                        if (lineEnd < 0)
                            return false;
                        // ...and so does the next paragraph, which the pair
                        // would otherwise be pulled up against.
                        var belowEnd = text.indexOf("\n", lineEnd + 1);
                        var below = belowEnd < 0 ? text.slice(lineEnd + 1)
                                                 : text.slice(lineEnd + 1, belowEnd);
                        if (!/^\s*$/.test(below))
                            return false;
                    }

                    remove(start, cursorPosition);
                    cursorPosition = start;
                    return true;
                }

                function pasteFromClipboard() {
                    var imageMarkdown = backend.pasteClipboardImage();
                    if (imageMarkdown.length > 0)
                        replaceSelectionWith(imageMarkdown);
                    else if (!pasteClipboardUrlAsMarkdownLink())
                        pasteClipboardAsPlainText();
                }

                function handleKey(event) {
                    var pasteKey = (event.key === Qt.Key_V)
                        && (event.modifiers & Qt.ControlModifier)
                        && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier));
                    var shiftInsert = (event.key === Qt.Key_Insert)
                        && (event.modifiers & Qt.ShiftModifier)
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier));
                    if (pasteKey || shiftInsert) {
                        pasteFromClipboard();
                        event.accepted = true;
                        return;
                    }

                    var pageKey = event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp;
                    var documentEdgeKey = (event.key === Qt.Key_Home || event.key === Qt.Key_End)
                        && (event.modifiers & Qt.ControlModifier);
                    if (pageKey || documentEdgeKey)
                        scrollLinger.restart();

                    var returnKey = event.key === Qt.Key_Return || event.key === Qt.Key_Enter;
                    var commandModifier = event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier);
                    if (returnKey && !commandModifier) {
                        smartReturn(event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    } else if (!commandModifier && event.key === Qt.Key_Backspace
                               && deleteParagraphBreakBehindCursor()) {
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Right) {
                        moveCursorVisibly(1);
                        event.accepted = true;
                    } else if (!commandModifier && !(event.modifiers & Qt.ShiftModifier)
                               && event.key === Qt.Key_Left) {
                        moveCursorVisibly(-1);
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab)
                               && indentListItems(event.key === Qt.Key_Backtab
                                                  || (event.modifiers & Qt.ShiftModifier))) {
                        event.accepted = true;
                    } else if (!commandModifier
                               && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
                        movePage(event.key === Qt.Key_PageDown ? 1 : -1,
                                 event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    }
                }

                onTextChanged: {
                    hoveredLinkUrl = "";
                    if (win.searchUpdating)
                        return;
                    var contentChanged = backend.editorTextChanged();
                    if (win.searchOpen && contentChanged)
                        win.updateSearch(false);
                }

                property string hoveredLinkUrl: ""
                property real linkHoverX: 0
                property real linkHoverY: 0

                // positionAt() snaps to the nearest caret, even past the end of a
                // line, so only count a character the pointer is inside: the one
                // on either side of that caret, whichever way the text runs.
                function linkUrlUnder(x, y) {
                    var caret = positionAt(x, y);
                    for (var position = Math.max(0, caret - 1);
                         position <= caret && position < length; ++position) {
                        var box = positionToRectangle(position);
                        var next = positionToRectangle(position + 1);
                        // A hidden marker has no width; only a line's last
                        // character needs an estimate.
                        var edge = next.y === box.y
                            ? next.x : box.x + writerFontMetrics.averageCharacterWidth;
                        if (y >= box.y && y < box.y + box.height
                                && x >= Math.min(box.x, edge) && x < Math.max(box.x, edge))
                            return backend.linkUrlAt(position);
                    }
                    return "";
                }

                Component.onCompleted: {
                    backend.attachDocument(textDocument);
                }
            }

            TextEdit {
                id: presentation
                objectName: "previewEditor"
                x: editor.x
                y: editor.y
                width: editor.width
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                readOnly: win.readMode
                color: win.bodyTextColor
                selectedTextColor: win.selectionTextColor
                selectionColor: win.selectionFill
                font: editor.font
                renderType: editor.renderType
                property color previewBackground: win.pageColor
                property color previewAccent: backend.themeAccent
                property color previewForeground: win.textColor
                property bool previewDark: win.darkMode
                onFontChanged: livePreview.refresh()
                onSelectionStartChanged: selectionCountDelay.restart()
                onSelectionEndChanged: selectionCountDelay.restart()
                onSelectedTextChanged: selectionCountDelay.restart()
                // The native viewport observer latches its internal caret
                // invisible when scrolling away. A separate item is not
                // hidden by that private control state on a distant click.
                cursorDelegate: Item {}
                clip: true
                property bool caretOn: true
                readonly property rect displayCursorRectangle: {
                    var offsets = livePreview.blockOffsets; // Re-evaluate after native horizontal layout.
                    return livePreview.visualCursorRectangle(cursorRectangle);
                }
                function restartCaret() {
                    caretOn = true;
                    if (activeFocus && !win.readMode && Qt.styleHints.cursorFlashTime > 0) caretBlink.restart();
                    else caretBlink.stop();
                }
                onActiveFocusChanged: restartCaret()
                onCursorRectangleChanged: {
                    restartCaret();
                    if (!win.projectionChanging && !win.restoringReadingPosition && !livePreview.readMode)
                        editorFlick.ensureCursorVisible();
                }
                Timer {
                    id: caretBlink
                    interval: Math.max(1, Qt.styleHints.cursorFlashTime / 2)
                    repeat: true
                    running: presentation.activeFocus && !win.readMode && Qt.styleHints.cursorFlashTime > 0
                    onTriggered: presentation.caretOn = !presentation.caretOn
                }
                Rectangle {
                    objectName: "visibleEditorCaret"
                    z: 3
                    x: presentation.displayCursorRectangle.x
                    y: presentation.displayCursorRectangle.y
                    width: 1
                    height: presentation.displayCursorRectangle.height
                    color: win.textColor
                    visible: presentation.activeFocus && !win.readMode && presentation.caretOn && livePreview.imeCursorVisible
                        && presentation.selectionStart === presentation.selectionEnd
                        && y + height > editorFlick.contentY - presentation.y
                        && y < editorFlick.contentY - presentation.y + editorFlick.height
                }
                function activateLink(link) {
                    if (link.indexOf("write-footnote:") !== 0) {
                        Qt.openUrlExternally(link);
                        return;
                    }
                    if (win.readMode) {
                        var position = livePreview.footnoteViewPosition(link);
                        if (position >= 0) {
                            var box = positionToRectangle(position);
                            editorFlick.scrollTo(editorFlick.clampContentY(y + box.y - 24));
                        }
                    } else {
                        var sourcePosition = backend.footnoteSourcePosition(link);
                        if (sourcePosition >= 0) editor.cursorPosition = sourcePosition;
                        forceActiveFocus();
                        Qt.callLater(function() { editorFlick.ensureCursorVisible(); });
                    }
                }
                onLinkActivated: function(link) { if (win.readMode) activateLink(link); }
                function updateViewport() {
                    livePreview.setViewport(editorFlick.contentY - y, editorFlick.height);
                }
                onYChanged: updateViewport()
                Connections {
                    target: editorFlick
                    function onContentYChanged() { presentation.updateViewport(); }
                    function onHeightChanged() { presentation.updateViewport(); }
                }

                Text {
                    // Reserve room for the independently drawn insertion caret.
                    // This is decorative only: never pad the canonical document.
                    x: presentation.displayCursorRectangle.x + 1 + win.scaledSize(2)
                    y: presentation.displayCursorRectangle.y
                    objectName: "emptyDocumentPlaceholder"
                    text: "# " + qsTr("Start writing")
                    visible: editor.length === 0 && !win.readMode && !presentation.inputMethodComposing
                    color: win.mutedColor
                    font: presentation.font
                }

                function linkUrlUnder(x, y) {
                    if (win.readMode) return linkAt(x, y);
                    return livePreview.linkAtPoint(x, y);
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            mouse.accepted = win.openEditorMenuAt(mouse.x, mouse.y);
                            return;
                        }
                        var commandClick = !win.readMode && (mouse.modifiers & Qt.ControlModifier)
                            && !(mouse.modifiers & (Qt.AltModifier | Qt.ShiftModifier | Qt.MetaModifier));
                        var link = commandClick ? presentation.linkUrlUnder(mouse.x, mouse.y) : "";
                        mouse.accepted = link.length > 0;
                        if (mouse.accepted) presentation.activateLink(link);
                    }
                    hoverEnabled: true
                    cursorShape: Qt.IBeamCursor
                    onPositionChanged: (mouse) => {
                        editor.linkHoverX = mouse.x;
                        editor.linkHoverY = mouse.y;
                        editor.hoveredLinkUrl = presentation.linkUrlUnder(mouse.x, mouse.y);
                    }
                    onExited: editor.hoveredLinkUrl = ""
                }

                ToolTip {
                    visible: editor.hoveredLinkUrl.length > 0 && tooltipLabel.text.length > 0
                    x: Math.max(0, Math.min(editor.linkHoverX + 12, presentation.width - width))
                    y: editor.linkHoverY + 20
                    contentItem: Label {
                        id: tooltipLabel
                        objectName: "linkTooltipText"
                        textFormat: Text.PlainText
                        text: backend.linkHoverText(editor.hoveredLinkUrl)
                        font.family: backend.editorFont
                        font.pixelSize: win.scaledSize(12)
                        wrapMode: Text.Wrap
                    }
                    width: Math.min(implicitWidth, 520 * win.textScale, win.width - 32)
                }

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Menu
                            || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                        if (win.openEditorMenuForCaret()) {
                            event.accepted = true;
                            return;
                        }
                    }
                    if (!win.readMode)
                        editor.handleKey(event);
                }
                Component.onCompleted: Qt.callLater(function() {
                    livePreview.attach(editor, presentation);
                    presentation.updateViewport();
                    presentation.forceActiveFocus();
                })
            }
            EditorDecorations { preview: livePreview; presentation: win.visibleEditor; sourceEditor: editor; host: win; foreground: false }
            EditorDecorations { id: foregroundDecorations; preview: livePreview; presentation: win.visibleEditor; sourceEditor: editor; host: win; foreground: true }
        }

        PreviewToggle {
            objectName: "readEditToggle"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: win.scaledSize(12)
            z: 5
            readMode: win.readMode
            iconColor: win.mutedColor
            onClicked: win.toggleReadMode()
        }

        Row {
            id: footerStatus
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 12
            anchors.bottomMargin: 10
            spacing: win.scaledSize(12)

            // Brighter while there are unsaved changes, as in Plain.
            FooterIconButton {
                objectName: "saveButton"
                iconName: "save"
                size: win.scaledSize(16)
                iconColor: backend.modified ? win.textColor : win.mutedColor
                opacity: backend.modified ? 0.9 : 0.55
                tooltip: qsTr("Save")
                tooltipColor: win.panelColor
                tooltipTextColor: win.textColor
                onClicked: if (win.commitTableCell()) backend.save()
            }

            FooterIconButton {
                objectName: "openButton"
                iconName: "open"
                size: win.scaledSize(16)
                iconColor: win.mutedColor
                opacity: 0.55
                tooltip: qsTr("Open")
                tooltipColor: win.panelColor
                tooltipTextColor: win.textColor
                onClicked: if (win.commitTableCell()) backend.openDialog()
            }

            FooterIconButton {
                id: fontButton
                objectName: "fontButton"
                iconName: "font"
                size: win.scaledSize(16)
                iconColor: win.mutedColor
                opacity: 0.55
                // Its tooltip would sit on the list it just opened.
                tooltip: fontPicker.opened ? "" : qsTr("Font")
                tooltipColor: win.panelColor
                tooltipTextColor: win.textColor
                onClicked: fontPicker.opened ? fontPicker.close() : fontPicker.open()
            }

            Label {
                text: backend.status
                color: win.mutedColor
                opacity: 0.55
                font.family: "IBM Plex Mono"
                font.pixelSize: win.scaledSize(11)
                visible: text !== ""
                elide: Text.ElideRight
                width: Math.min(360, win.width / 3)
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        FontPicker {
            id: fontPicker
            objectName: "fontPicker"
            // Parented to its button, so a press on the button is not a press
            // outside that closes it only for the click to open it again.
            parent: fontButton
            // A popup is not mirrored with its parent, so it opens towards the
            // window's middle from the footer's end it sits at.
            x: Qt.application.layoutDirection === Qt.RightToLeft ? fontButton.width - width + 8 : -8
            y: -height - 10
            width: win.scaledSize(320)
            maximumHeight: Math.min(win.scaledSize(440), win.height - 80)
            fonts: backend.availableFonts
            currentFont: backend.editorFont
            defaultFont: "IBM Plex Mono"
            textScale: win.textScale
            panelColor: win.panelColor
            textColor: win.textColor
            mutedColor: win.mutedColor
            highlightColor: backend.themeAccent
            onFontChosen: function(family) { backend.editorFont = family; }
            onClosed: presentation.forceActiveFocus()
        }

        Label {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 12
            anchors.bottomMargin: 10
            objectName: "wordCountLabel"
            text: (presentation.selectionStart !== presentation.selectionEnd
                ? qsTr("%1 selected · ").arg(backend.selectionWordCount) : "")
                + qsTr("%n Words", "", backend.wordCount)
            color: win.mutedColor
            opacity: 0.75
            font.family: "IBM Plex Mono"
            font.pixelSize: win.scaledSize(11)
        }


        Pane {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: win.scaledSize(win.replaceOpen ? 104 : 56)
            visible: win.searchOpen
            z: 10
            leftPadding: 16
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0
            Material.elevation: 8
            // Reaches here from either field or the buttons beside them.
            Keys.onEscapePressed: function(event) {
                win.closeSearch();
                event.accepted = true;
            }

            background: Rectangle {
                radius: 9
                color: win.panelColor
            }

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchField
                        objectName: "searchField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: win.replaceOpen ? parent.height / 2 : parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        clip: true
                        onTextChanged: searchDelay.restart()
                        Keys.onReturnPressed: function(event) {
                            win.moveSearch((event.modifiers & Qt.ShiftModifier) ? -1 : 1);
                            event.accepted = true;
                        }
                    }

                    TextInput {
                        id: replaceField
                        objectName: "replaceField"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height / 2
                        visible: win.replaceOpen
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        activeFocusOnTab: true
                        clip: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.selectionTextColor
                        font.pixelSize: win.scaledSize(17)
                        Keys.onReturnPressed: replaceCurrentButton.clicked()
                    }

                    Label {
                        anchors.verticalCenter: replaceField.verticalCenter
                        text: qsTr("Replace with")
                        visible: win.replaceOpen && replaceField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }

                    Label {
                        anchors.verticalCenter: searchField.verticalCenter
                        text: qsTr("Find")
                        visible: searchField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }
                }

                Label {
                    Layout.preferredWidth: win.scaledSize(58)
                    Layout.fillHeight: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: win.searchMatches.length === 0
                        ? "0/0"
                        : (win.searchMatchIndex + 1) + "/" + win.searchMatches.length
                    color: win.darkMode ? win.textColor : "#62635f"
                    font.pixelSize: win.scaledSize(16)
                }

                Button {
                    id: replaceCurrentButton
                    objectName: "replaceButton"
                    visible: win.replaceOpen
                    text: qsTr("Replace")
                    onClicked: {
                        win.flushSearch();
                        if (win.searchMatchIndex < 0) return;
                        var start = win.searchMatches[win.searchMatchIndex];
                        var replacement = replaceField.text;
                        EditorMutations.replaceRange(editor, start,
                                                     start + searchField.text.length,
                                                     replacement);
                        // Carry on past what was just written, so a replacement
                        // the query also matches is not found again.
                        win.searchMatches = backend.searchMatches(searchField.text);
                        win.searchMatchIndex = win.matchAtOrAfter(start + replacement.length);
                        win.showSearchMatch(true);
                    }
                }

                Button {
                    objectName: "replaceAllButton"
                    visible: win.replaceOpen
                    text: qsTr("All")
                    onClicked: {
                        win.flushSearch();
                        var query = searchField.text;
                        var matches = win.searchMatches;
                        if (query.length === 0 || matches.length === 0) return;
                        // One replacement over the span of every match, so a
                        // single undo takes them all back.
                        var first = matches[0];
                        var text = editor.rawText();
                        var replaced = "";
                        var position = first;
                        for (var i = 0; i < matches.length; ++i) {
                            replaced += text.slice(position, matches[i]) + replaceField.text;
                            position = matches[i] + query.length;
                        }
                        EditorMutations.replaceRange(editor, first, position, replaced);
                        win.updateSearch(false);
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 34
                    color: win.darkMode ? "#6f6f62" : "#d5d56e"
                }

                SearchIconButton {
                    iconName: "up"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(-1)
                }

                SearchIconButton {
                    iconName: "down"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.moveSearch(1)
                }

                SearchIconButton {
                    iconName: "close"
                    iconColor: win.darkMode ? win.textColor : "#62635f"
                    onClicked: win.closeSearch()
                }
            }
        }
    }

    Component.onCompleted: {
        var geometry = backend.windowGeometry();
        if (geometry.x >= 0) x = geometry.x;
        if (geometry.y >= 0) y = geometry.y;
        width = geometry.width;
        height = geometry.height;
        if (geometry.maximized && !backend.routesFileOpen) showMaximized();
        Qt.callLater(function() { if (!win.positionReady && win.pendingReadingPosition === null) win.restoreFilePosition(); });
    }

    Component.onDestruction: {
        win.rememberPosition();
        backend.saveWindowGeometry(x, y, width, height, visibility === Window.Maximized);
    }

}
