import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
// The only route to a real macOS menu bar from QML. On other platforms the
// items simply go into an in-window bar.
import Qt.labs.platform as Platform
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: backend.nativeMacChrome
           ? (backend.tabTitle + "[*]")
           : ((backend.modified ? "* " : "") + backend.tabTitle + " - Omawrite")

    SystemPalette {
        id: macPalette
        colorGroup: SystemPalette.Active
    }

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.nativeMacChrome ? macPalette.window : backend.themeBackground
    readonly property color textColor: backend.nativeMacChrome ? macPalette.windowText : backend.themeForeground
    readonly property color strongTextColor: backend.nativeMacChrome ? macPalette.windowText : backend.themeForeground
    readonly property color mutedColor: backend.nativeMacChrome
                                          ? macPalette.mid
                                          : (darkMode ? "#909191" : "#aeb1b5")
    readonly property color selectionFill: backend.nativeMacChrome ? macPalette.highlight : backend.themeSelection
    readonly property string chromeFontFamily: backend.nativeMacChrome
                                                 ? Qt.application.font.family
                                                 : backend.editorFontFamily
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(backend.editorFontSize)
    // Never wider than the Flickable's viewport, whatever the floor asks for:
    // a tiling compositor can resize the window below its minimum width.
    readonly property int editorWidth: Math.min(
        Math.round(writerFontMetrics.averageCharacterWidth * backend.editorMeasureChars),
        Math.max(360, width - Math.round(writerFontMetrics.averageCharacterWidth * 20)),
        Math.max(0, width - 48))
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    property url pendingOpenUrl
    property string pendingAction: ""
    property int pendingTabIndex: -1
    property bool replaceOpen: false
    property bool awaitingPendingSave: false
    property bool previewMode: false
    property string assembleMarkdown: ""
    property url assembleSuggestedMd
    property url assembleSuggestedPdf

    // Typora parity: Ctrl+/ swaps the source buffer for a rendered view.
    function togglePreview() {
        previewMode = !previewMode;
        if (!previewMode)
            editor.forceActiveFocus();
    }

    function openAssembleFolder() {
        const receipt = backend.assembleThisFolder();
        if (!receipt.ok)
            return;
        assembleMarkdown = receipt.markdown;
        assembleSuggestedMd = receipt.suggestedSaveUrl;
        assembleSuggestedPdf = receipt.suggestedPdfUrl;
        assembleFolderDialog.receipt = receipt;
        assembleFolderDialog.open();
    }

    Material.theme: backend.nativeMacChrome ? Material.System : (darkMode ? Material.Dark : Material.Light)
    Material.accent: backend.nativeMacChrome ? macPalette.highlight : backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        backend.persistSession();
        if (closeConfirmed)
            return;
        close.accepted = true;
    }

    function requestCloseTab(index) {
        var tab = backend.tabs[index];
        if (tab && tab.dirty) {
            pendingTabIndex = index;
            pendingAction = "closeTab";
            if (backend.nativeMacChrome)
                unsavedChangesAlert.open();
            else
                unsavedChangesDialog.open();
            return;
        }
        backend.closeTab(index);
    }

    // Every text operation returns the whole document plus a caret offset.
    // Routing it through the editor's own insert/remove keeps it one undo step.
    function applyTextOperation(operation) {
        if (!operation || operation.text === undefined
                || operation.text === editor.text) {
            return false;
        }

        EditorMutations.replaceRange(editor, 0, editor.text.length, operation.text);
        editor.cursorPosition = Math.max(0, Math.min(editor.text.length,
                                                     operation.cursor));
        return true;
    }

    function moveParagraph(delta) {
        win.applyTextOperation(
            backend.moveParagraph(editor.text, editor.cursorPosition, delta));
    }

    // One key for both directions: a paragraph sitting on one line explodes,
    // and one already split back into sentences collapses.
    function toggleSentenceLines() {
        if (win.applyTextOperation(
                backend.explodeSentences(editor.text, editor.cursorPosition))) {
            return;
        }

        win.applyTextOperation(
            backend.collapseSentences(editor.text, editor.cursorPosition));
    }

    function jumpToOutlineEntry(position) {
        outlineDialog.close();
        editor.forceActiveFocus();
        editor.cursorPosition = Math.max(0, Math.min(editor.text.length, position));
        editorFlick.ensureCursorVisible();
    }

    function completePendingAction() {
        var action = pendingAction;
        pendingAction = "";
        if (action === "close") {
            closeConfirmed = true;
            close();
        } else if (action === "open") {
            backend.open(pendingOpenUrl);
        } else if (action === "closeTab") {
            backend.closeTab(pendingTabIndex);
        }
    }

    FontMetrics {
        id: writerFontMetrics
        font.family: backend.editorFontFamily
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

    function updateSearch() {
        var matches = [];
        var query = searchField.text;
        if (query.length > 0) {
            var haystack = editor.text.toLocaleLowerCase();
            var needle = query.toLocaleLowerCase();
            var position = 0;
            while ((position = haystack.indexOf(needle, position)) !== -1) {
                matches.push(position);
                position += Math.max(1, needle.length);
            }
        }
        searchMatches = matches;
        searchMatchIndex = matches.length > 0 ? 0 : -1;
        showSearchMatch();
    }

    function showSearchMatch() {
        var start = searchMatchIndex >= 0 ? searchMatches[searchMatchIndex] : -1;
        searchUpdating = true;
        backend.setSearchHighlight(searchField.text, start);
        if (start >= 0) {
            editor.select(start, start + searchField.text.length);
            editorFlick.ensureCursorVisible();
        }
        searchUpdating = false;
    }

    function moveSearch(direction) {
        if (searchMatches.length === 0)
            return;
        searchMatchIndex = (searchMatchIndex + direction + searchMatches.length)
                           % searchMatches.length;
        showSearchMatch();
    }

    function closeSearch() {
        searchOpen = false;
        searchUpdating = true;
        backend.setSearchHighlight("", -1);
        editor.deselect();
        searchUpdating = false;
        replaceOpen = false;
        editor.forceActiveFocus();
    }

    Shortcut {
        id: saveShortcut
        sequences: [StandardKey.Save]
        context: Qt.ApplicationShortcut
        onActivated: backend.save()
    }

    Shortcut {
        id: replaceShortcut
        sequence: Qt.platform.os === "osx" ? "Ctrl+Alt+F" : "Ctrl+H"
        context: Qt.ApplicationShortcut
        onActivated: {
            searchOpen = true;
            replaceOpen = true;
            searchField.forceActiveFocus();
            searchField.selectAll();
        }
    }

    Shortcut {
        id: zoomInShortcut
        // StandardKey.ZoomIn alone misses the unshifted "=" that most keyboards
        // put the "+" on, which is the key writers actually reach for.
        sequences: [StandardKey.ZoomIn, "Ctrl+="]
        context: Qt.ApplicationShortcut
        onActivated: backend.editorFontSize += 2
    }

    Shortcut {
        id: zoomOutShortcut
        sequences: [StandardKey.ZoomOut]
        context: Qt.ApplicationShortcut
        onActivated: backend.editorFontSize -= 2
    }

    Shortcut {
        id: zoomResetShortcut
        sequence: "Ctrl+0"
        context: Qt.ApplicationShortcut
        onActivated: backend.resetEditorFontSize()
    }

    Shortcut {
        id: boldShortcut
        sequences: [StandardKey.Bold]
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("**", "**")
    }

    Shortcut {
        id: italicShortcut
        sequences: [StandardKey.Italic]
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("*", "*")
    }

    Shortcut {
        id: linkShortcut
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertLink()
    }

    AboutDialog {
        id: aboutDialog
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        accentColor: backend.themeAccent
        fontFamily: backend.editorFontFamily
        version: backend.appVersion
        commit: backend.appCommit
        commitUrl: backend.appCommitUrl
    }

    Shortcut {
        id: moveParagraphUpShortcut
        sequence: "Alt+Up"
        context: Qt.WindowShortcut
        onActivated: win.moveParagraph(-1)
    }

    Shortcut {
        id: moveParagraphDownShortcut
        sequence: "Alt+Down"
        context: Qt.WindowShortcut
        onActivated: win.moveParagraph(1)
    }

    Shortcut {
        id: sentenceLinesShortcut
        sequence: "Ctrl+L"
        context: Qt.WindowShortcut
        onActivated: win.toggleSentenceLines()
    }

    Shortcut {
        id: outlineShortcut
        sequence: "Ctrl+Shift+O"
        context: Qt.ApplicationShortcut
        onActivated: outlineDialog.open()
    }

    Shortcut {
        id: previewShortcut
        sequence: "Ctrl+/"
        context: Qt.ApplicationShortcut
        onActivated: win.togglePreview()
    }

    Shortcut {
        id: helpShortcut
        sequence: "Ctrl+?"
        context: Qt.ApplicationShortcut
        onActivated: shortcutsDialog.open()
    }

    Shortcut {
        id: openShortcut
        sequences: [StandardKey.Open]
        context: Qt.ApplicationShortcut
        onActivated: backend.openDialog()
    }

    Shortcut {
        id: newShortcut
        sequences: [StandardKey.New]
        context: Qt.ApplicationShortcut
        onActivated: backend.newTab()
    }

    Shortcut {
        id: newWindowShortcut
        sequence: "Ctrl+Shift+N"
        context: Qt.ApplicationShortcut
        onActivated: backend.newWindow()
    }

    Shortcut {
        id: closeTabShortcut
        sequences: [StandardKey.Close]
        context: Qt.WindowShortcut
        onActivated: win.requestCloseTab(backend.activeTabIndex)
    }

    Shortcut {
        id: saveAsShortcut
        sequences: [StandardKey.SaveAs]
        context: Qt.ApplicationShortcut
        onActivated: backend.saveAsDialog()
    }

    Shortcut {
        id: printShortcut
        sequences: [StandardKey.Print]
        context: Qt.ApplicationShortcut
        onActivated: backend.printDocument()
    }

    Shortcut {
        id: assembleFolderShortcut
        sequence: "Ctrl+Shift+P"
        context: Qt.ApplicationShortcut
        enabled: backend.canAssembleThisFolder
        onActivated: win.openAssembleFolder()
    }

    Shortcut {
        id: fullscreenShortcut
        sequences: Qt.platform.os === "osx"
            ? ["Ctrl+Meta+F"]
            : ["Meta+F", "F11"]
        context: Qt.ApplicationShortcut
        onActivated: toggleFullScreen()
    }

    Shortcut {
        id: undoShortcut
        sequences: [StandardKey.Undo]
        context: Qt.WindowShortcut
        onActivated: editor.undo()
    }

    Shortcut {
        id: redoShortcut
        // Qt drops Ctrl+Y from StandardKey.Redo under the GNOME keyboard
        // scheme, which is the one Omarchy's Qt sessions resolve to.
        sequences: Qt.platform.os === "osx"
            ? [StandardKey.Redo]
            : [StandardKey.Redo, "Ctrl+Y"]
        context: Qt.WindowShortcut
        onActivated: editor.redo()
    }

    Shortcut {
        id: findShortcut
        sequences: [StandardKey.Find]
        context: Qt.ApplicationShortcut
        onActivated: {
            searchOpen = true;
            searchField.forceActiveFocus();
            searchField.selectAll();
        }
    }

    Shortcut {
        id: findNextShortcut
        sequence: "Ctrl+G"
        context: Qt.ApplicationShortcut
        enabled: win.searchOpen
        onActivated: win.moveSearch(1)
    }

    Connections {
        target: backend

        function onCloseWindowRequested() {
            win.closeConfirmed = true;
            win.close();
        }

        function onOpenDialogRequested() {
            openFileDialog.open();
        }

        function onSaveDialogRequested(suggestedUrl) {
            saveFileDialog.selectedFile = suggestedUrl;
            saveFileDialog.open();
        }

        function onCloseAfterSave() {
            win.closeConfirmed = true;
            win.close();
        }

        function onSaveSucceeded() {
            win.awaitingPendingSave = false;
            if (win.pendingAction !== "")
                win.completePendingAction();
        }

        // A save that does not happen drops the intent with it, the way the
        // backend drops its own close latch. Otherwise the close stays pending
        // and any later successful save carries it out.
        function onSaveFailed() {
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.appeared = false;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }

        function onExternalFileAppeared(locallyModified) {
            // This save is not going to happen, so whatever it was for cannot
            // follow it. Leaving the intent standing lets an unrelated save
            // minutes later close the window or open another document.
            win.awaitingPendingSave = false;
            win.pendingAction = "";
            externalChangeDialog.deleted = false;
            externalChangeDialog.appeared = true;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }
    }

    // Menu items carry their own key equivalents. On macOS AppKit consumes
    // those before Qt sees them, so the Shortcut elements above stay as the
    // reference the shortcuts dialog reads; both call the same functions.
    Platform.MenuBar {
        Platform.Menu {
            title: "Omawrite"

            Platform.MenuItem {
                text: "About Omawrite"
                // AboutRole puts it where macOS keeps it: first in the
                // application menu, above the separator.
                role: Platform.MenuItem.AboutRole
                onTriggered: aboutDialog.open()
            }

            Platform.MenuItem {
                text: "Preferences\u2026"
                role: Platform.MenuItem.PreferencesRole
                shortcut: "Ctrl+,"
                onTriggered: preferencesDialog.open()
            }
        }

        Platform.Menu {
            title: "File"

            Platform.MenuItem {
                text: "New Tab"
                shortcut: StandardKey.New
                onTriggered: backend.newTab()
            }
            Platform.MenuItem {
                text: "New Window"
                shortcut: "Ctrl+Shift+N"
                onTriggered: backend.newWindow()
            }
            Platform.MenuItem {
                text: "Open\u2026"
                shortcut: StandardKey.Open
                onTriggered: backend.openDialog()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Close Tab"
                shortcut: StandardKey.Close
                onTriggered: win.requestCloseTab(backend.activeTabIndex)
            }
            Platform.MenuItem {
                text: "Close Window"
                shortcut: "Ctrl+Shift+W"
                onTriggered: win.close()
            }
            Platform.MenuItem {
                text: "Save"
                shortcut: StandardKey.Save
                onTriggered: backend.save()
            }
            Platform.MenuItem {
                text: "Save As\u2026"
                shortcut: StandardKey.SaveAs
                onTriggered: backend.saveAsDialog()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Print\u2026"
                shortcut: StandardKey.Print
                onTriggered: backend.printDocument()
            }
            Platform.MenuItem {
                text: "Assemble this folder\u2026"
                shortcut: "Ctrl+Shift+P"
                enabled: backend.canAssembleThisFolder
                onTriggered: win.openAssembleFolder()
            }
        }

        Platform.Menu {
            title: "Edit"

            Platform.MenuItem {
                text: "Undo"
                shortcut: StandardKey.Undo
                onTriggered: editor.undo()
            }
            Platform.MenuItem {
                text: "Redo"
                shortcut: StandardKey.Redo
                onTriggered: editor.redo()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Cut"
                shortcut: StandardKey.Cut
                onTriggered: editor.cut()
            }
            Platform.MenuItem {
                text: "Copy"
                shortcut: StandardKey.Copy
                onTriggered: editor.copy()
            }
            Platform.MenuItem {
                text: "Paste"
                shortcut: StandardKey.Paste
                onTriggered: editor.paste()
            }
            Platform.MenuItem {
                text: "Select All"
                shortcut: StandardKey.SelectAll
                onTriggered: editor.selectAll()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Bold"
                shortcut: StandardKey.Bold
                onTriggered: editor.wrapSelection("**", "**")
            }
            Platform.MenuItem {
                text: "Italic"
                shortcut: StandardKey.Italic
                onTriggered: editor.wrapSelection("*", "*")
            }
            Platform.MenuItem {
                text: "Link"
                shortcut: "Ctrl+K"
                onTriggered: editor.insertLink()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Find"
                shortcut: StandardKey.Find
                onTriggered: {
                    win.searchOpen = true;
                    searchField.forceActiveFocus();
                    searchField.selectAll();
                }
            }
            Platform.MenuItem {
                text: "Find and Replace"
                shortcut: replaceShortcut.sequence
                onTriggered: {
                    win.searchOpen = true;
                    win.replaceOpen = true;
                    searchField.forceActiveFocus();
                    searchField.selectAll();
                }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Move Paragraph Up"
                shortcut: "Alt+Up"
                onTriggered: win.moveParagraph(-1)
            }
            Platform.MenuItem {
                text: "Move Paragraph Down"
                shortcut: "Alt+Down"
                onTriggered: win.moveParagraph(1)
            }
            Platform.MenuItem {
                text: "Sentences on Their Own Lines"
                shortcut: "Ctrl+L"
                onTriggered: win.toggleSentenceLines()
            }
        }

        Platform.Menu {
            title: "View"

            Platform.MenuItem {
                text: "Preview"
                shortcut: "Ctrl+/"
                onTriggered: win.togglePreview()
            }
            Platform.MenuItem {
                text: "Outline"
                shortcut: "Ctrl+Shift+O"
                onTriggered: outlineDialog.open()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Increase Text Size"
                shortcut: StandardKey.ZoomIn
                onTriggered: backend.editorFontSize += 2
            }
            Platform.MenuItem {
                text: "Decrease Text Size"
                shortcut: StandardKey.ZoomOut
                onTriggered: backend.editorFontSize -= 2
            }
            Platform.MenuItem {
                text: "Reset Text Size"
                shortcut: "Ctrl+0"
                onTriggered: backend.resetEditorFontSize()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Full Screen"
                shortcut: fullscreenShortcut.sequence
                onTriggered: win.toggleFullScreen()
            }
        }

        Platform.Menu {
            title: "Window"

            Platform.MenuItem {
                text: "Minimize"
                shortcut: StandardKey.Minimize
                onTriggered: win.showMinimized()
            }
            Platform.MenuItem {
                text: "Zoom"
                onTriggered: {
                    if (win.visibility === Window.Maximized)
                        win.showNormal();
                    else
                        win.showMaximized();
                }
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: "Bring All to Front"
                onTriggered: backend.bringAllWindowsToFront()
            }
        }

        Platform.Menu {
            title: "Help"

            Platform.MenuItem {
                text: "Keyboard Shortcuts"
                shortcut: "Ctrl+?"
                onTriggered: shortcutsDialog.open()
            }
        }
    }

    PreferencesDialog {
        id: preferencesDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        mutedColor: win.mutedColor
        fontFamily: backend.editorFontFamily
        fontFamilies: backend.availableFontFamilies()
        maxContentHeight: Math.max(240, win.height - 200)
        preferredWidth: Math.max(320, win.width - 80)
    }

    Shortcut {
        id: preferencesShortcut
        sequence: "Ctrl+,"
        context: Qt.ApplicationShortcut
        onActivated: preferencesDialog.open()
    }

    Dialogs.FileDialog {
        id: openFileDialog
        title: "Open File"
        fileMode: Dialogs.FileDialog.OpenFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: win.requestOpen(selectedFile)
    }

    Dialogs.FileDialog {
        id: saveFileDialog
        title: "Save File"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: backend.saveAs(selectedFile)
        onRejected: {
            backend.fileDialogCanceled();
            win.awaitingPendingSave = false;
            win.pendingAction = "";
        }
    }

    AssembleFolderDialog {
        id: assembleFolderDialog
        darkMode: win.darkMode
        textColor: win.textColor
        strongTextColor: win.strongTextColor
        mutedColor: win.mutedColor
        fontFamily: backend.editorFontFamily
        textScale: win.textScale
        onPrintRequested: backend.printAssembledMarkdown(win.assembleMarkdown)
        onSaveMarkdownRequested: {
            assembleMarkdownDialog.selectedFile = win.assembleSuggestedMd;
            assembleMarkdownDialog.open();
        }
        onSavePdfRequested: {
            assemblePdfDialog.selectedFile = win.assembleSuggestedPdf;
            assemblePdfDialog.open();
        }
    }

    Dialogs.FileDialog {
        id: assembleMarkdownDialog
        title: "Save assembled Markdown"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["Markdown files (*.md *.markdown)", "All files (*)"]
        onAccepted: {
            backend.saveAssembledMarkdown(selectedFile, win.assembleMarkdown);
            assembleFolderDialog.close();
        }
    }

    Dialogs.FileDialog {
        id: assemblePdfDialog
        title: "Save assembled PDF"
        fileMode: Dialogs.FileDialog.SaveFile
        nameFilters: ["PDF files (*.pdf)", "All files (*)"]
        onAccepted: {
            backend.saveAssembledPdf(selectedFile, win.assembleMarkdown);
            assembleFolderDialog.close();
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        fileName: backend.tabTitle
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.strongTextColor
        activeButtonColor: backend.themeAccent
        containerWidth: win.width
        containerHeight: win.height

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

    Dialogs.MessageDialog {
        id: unsavedChangesAlert
        title: "Unsaved Changes"
        text: "Do you want to save the changes you made to the document “"
              + backend.tabTitle + "”?"
        buttons: Dialogs.MessageDialog.Save | Dialogs.MessageDialog.Discard
                 | Dialogs.MessageDialog.Cancel
        onButtonClicked: function(button, role) {
            switch (button) {
            case Dialogs.MessageDialog.Save:
                win.awaitingPendingSave = true;
                backend.save();
                break;
            case Dialogs.MessageDialog.Discard:
                backend.discardRecovery();
                win.completePendingAction();
                break;
            default:
                win.pendingAction = "";
                break;
            }
        }
    }

    ExternalChangeDialog {
        id: externalChangeDialog
        darkMode: win.darkMode
        textScale: win.textScale
        textColor: win.textColor
        strongTextColor: win.strongTextColor
        containerWidth: win.width
        containerHeight: win.height

        onKeepRequested: backend.keepExternalVersion()
        onReloadRequested: backend.reloadFromDisk()
    }

    Dialog {
        id: shortcutsDialog
        modal: true
        title: "Keyboard shortcuts"
        standardButtons: Dialog.Close
        anchors.centerIn: parent
        // Dialog derives contentWidth from contentItem.implicitWidth while the
        // contentItem's width comes back from availableWidth, which is a loop.
        // Naming the width breaks it.
        contentWidth: shortcutsLabel.implicitWidth
        contentItem: Label {
            id: shortcutsLabel
            text: saveShortcut.nativeText + "  Save\n"
                + saveAsShortcut.nativeText + "  Save As\n"
                + openShortcut.nativeText + "  Open\n"
                + newShortcut.nativeText + "  New Tab\n"
                + newWindowShortcut.nativeText + "  New Window\n"
                + closeTabShortcut.nativeText + "  Close Tab\n"
                + findShortcut.nativeText + "  Find\n"
                + replaceShortcut.nativeText + "  Find and Replace\n"
                + boldShortcut.nativeText + "  Bold\n"
                + italicShortcut.nativeText + "  Italic\n"
                + linkShortcut.nativeText + "  Link\n"
                + previewShortcut.nativeText + "  Preview\n"
                + moveParagraphUpShortcut.nativeText + " / "
                + moveParagraphDownShortcut.nativeText + "  Move paragraph\n"
                + sentenceLinesShortcut.nativeText + "  Sentences on their own lines\n"
                + outlineShortcut.nativeText + "  Outline\n"
                + zoomInShortcut.nativeText + "  Increase text size\n"
                + zoomOutShortcut.nativeText + "  Decrease text size\n"
                + zoomResetShortcut.nativeText + "  Reset text size\n"
                + printShortcut.nativeText + "  Print\n"
                + assembleFolderShortcut.nativeText + "  Assemble this folder\n"
                + fullscreenShortcut.nativeText + "  Fullscreen\n"
                + preferencesShortcut.nativeText + "  Preferences\n"
                + helpShortcut.nativeText + "  Shortcuts"
            lineHeight: 1.5
        }
    }

    Dialog {
        id: outlineDialog
        objectName: "outlineDialog"
        modal: true
        title: "Outline"
        standardButtons: Dialog.Close
        anchors.centerIn: parent
        width: Math.min(win.width - 80, 520)

        // Read on open rather than bound to the text: the list is a snapshot to
        // navigate by, and rebuilding it on every keystroke while the dialog is
        // shut is work nobody sees.
        property var entries: []
        onAboutToShow: entries = backend.outlineFor(editor.text)

        contentItem: ScrollView {
            clip: true
            implicitHeight: Math.min(win.height - 200, Math.max(60, outlineList.contentHeight))

            ListView {
                id: outlineList
                objectName: "outlineList"
                model: outlineDialog.entries
                spacing: 2

                delegate: ItemDelegate {
                    width: outlineList.width
                    // Nested headings step in, so the shape of the argument is
                    // visible at a glance.
                    leftPadding: 8 + (modelData.level - 1) * 16
                    text: modelData.title
                    font.family: backend.editorFontFamily
                    font.pixelSize: win.scaledSize(modelData.level === 1 ? 14 : 12)
                    font.weight: modelData.level === 1 ? Font.Bold : Font.Normal
                    onClicked: win.jumpToOutlineEntry(modelData.position)
                }
            }
        }

        Label {
            anchors.centerIn: parent
            visible: outlineDialog.entries.length === 0
            text: "No headings yet."
            color: win.mutedColor
            font.family: backend.editorFontFamily
            font.pixelSize: win.scaledSize(12)
        }
    }

    Item {
        anchors.fill: parent

        Rectangle {
            id: tabStrip
            objectName: "tabStrip"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.max(28, win.scaledSize(28))
            color: backend.nativeMacChrome ? macPalette.alternateBase : win.pageColor

            ListView {
                id: tabList
                objectName: "tabList"
                anchors.fill: parent
                anchors.leftMargin: 8
                orientation: ListView.Horizontal
                clip: true
                spacing: 2
                model: backend.tabs
                delegate: Item {
                    id: tabDelegate
                    required property int index
                    required property var modelData
                    width: Math.min(180, Math.max(88, titleLabel.implicitWidth + 36))
                    height: tabStrip.height

                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.color: tabDelegate.modelData.active ? backend.themeAccent : "transparent"
                        border.width: 0
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: tabDelegate.modelData.active
                                   ? (backend.nativeMacChrome ? macPalette.highlight : backend.themeAccent)
                                   : "transparent"
                        }
                    }

                    MouseArea {
                        id: tabMouse
                        objectName: "tabHandle"
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        onClicked: backend.setActiveTab(tabDelegate.index)
                        onReleased: function(mouse) {
                            var globalPoint = tabMouse.mapToGlobal(mouse.x, mouse.y)
                            var inStrip = tabList.mapFromItem(tabMouse, mouse.x, mouse.y)
                            if (inStrip.x >= 0 && inStrip.y >= 0
                                    && inStrip.x < tabList.width
                                    && inStrip.y < tabList.height) {
                                var at = tabList.indexAt(inStrip.x + tabList.contentX, inStrip.y)
                                if (at >= 0 && at !== tabDelegate.index) {
                                    backend.moveTab(tabDelegate.index, at)
                                    return
                                }
                            }
                            backend.finishTabDrag(tabDelegate.index, globalPoint.x, globalPoint.y)
                        }
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        spacing: 6
                        z: 1
                        Rectangle {
                            width: 6
                            height: 6
                            radius: 3
                            visible: tabDelegate.modelData.dirty
                            color: backend.nativeMacChrome ? macPalette.highlight : backend.themeAccent
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            id: titleLabel
                            text: tabDelegate.modelData.title
                            color: tabDelegate.modelData.active ? win.textColor : win.mutedColor
                            font.family: win.chromeFontFamily
                            font.pixelSize: win.scaledSize(backend.nativeMacChrome ? 13 : 12)
                            elide: Text.ElideRight
                            width: Math.min(140, implicitWidth)
                        }
                        Text {
                            text: "×"
                            color: win.mutedColor
                            font.pixelSize: win.scaledSize(14)
                            visible: !backend.nativeMacChrome
                                     || tabMouse.containsMouse
                                     || tabDelegate.modelData.active
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                onClicked: win.requestCloseTab(tabDelegate.index)
                            }
                        }
                    }
                }
            }
        }

        Flickable {
            id: editorFlick
            objectName: "editorViewport"
            anchors.top: tabStrip.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footer.top
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, (win.previewMode ? preview.y + preview.implicitHeight
                                                             : editor.y + editor.implicitHeight) + 220)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                // Wheel scrolling moves contentY directly rather than
                // flicking the Flickable, so the bar has to be told about
                // that activity; linger briefly after the last event.
                // The viewport already stops at footer.top, so the bar needs
                // no inset of its own -- PR #42 added one for a footer the
                // flickable used to run underneath.
                active: hovered || pressed || wheelScroll.running
                    || touchpadMomentum.running || scrollLinger.running
            }

            Timer {
                id: scrollLinger
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

            // Pixel-precise touchpad deltas follow the fingers directly at a
            // larger scale. Recent deltas provide the velocity for a short,
            // frame-rate-independent coast when the gesture ends.
            // macOS accelerates trackpad deltas itself before Qt sees them, so
            // doubling here would scroll twice as fast as every other app on
            // the machine. Elsewhere the deltas arrive raw and need the scale.
            readonly property real touchpadScale: Qt.platform.os === "osx" ? 1.0 : 2.0
            readonly property int touchpadEventGapMs: 80
            readonly property real touchpadVelocityBlend: 0.35
            readonly property real touchpadMomentumDecay: 0.90
            readonly property real touchpadMinVelocity: 20
            readonly property real touchpadMaxVelocity: 2400
            property bool touchpadGestureActive: false
            property bool platformMomentumActive: false
            property real touchpadVelocity: 0
            property double touchpadLastEventTime: 0

            Timer {
                id: touchpadEventGap
                interval: editorFlick.touchpadEventGapMs
                onTriggered: {
                    if (editorFlick.platformMomentumActive) {
                        // Some platforms omit ScrollEnd after their momentum
                        // phase. Do not let that suppress the next gesture.
                        editorFlick.platformMomentumActive = false;
                    } else {
                        editorFlick.finishTouchpadGesture();
                    }
                }
            }

            FrameAnimation {
                id: touchpadMomentum
                running: false

                property real velocity: 0
                property real previousElapsedTime: 0

                onTriggered: {
                    var dt = elapsedTime - previousElapsedTime;
                    previousElapsedTime = elapsedTime;
                    if (dt <= 0)
                        return;

                    var maxY = Math.max(0, editorFlick.contentHeight - editorFlick.height);
                    var nextY = editorFlick.clampContentY(editorFlick.contentY + velocity * dt);
                    if ((velocity < 0 && nextY <= 0)
                            || (velocity > 0 && nextY >= maxY)) {
                        editorFlick.contentY = nextY;
                        stop();
                        return;
                    }

                    editorFlick.contentY = editorFlick.snapToPixel(nextY);
                    velocity *= Math.pow(editorFlick.touchpadMomentumDecay, dt * 60);
                    if (Math.abs(velocity) < editorFlick.touchpadMinVelocity)
                        stop();
                }

                function begin(initialVelocity) {
                    velocity = Math.max(-editorFlick.touchpadMaxVelocity,
                                        Math.min(editorFlick.touchpadMaxVelocity,
                                                 initialVelocity));
                    previousElapsedTime = 0;
                    if (Math.abs(velocity) >= editorFlick.touchpadMinVelocity)
                        restart();
                }
            }

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
                    scrollLinger.restart();
                    if (wheel.pixelDelta.y !== 0 || wheel.phase === Qt.ScrollMomentum
                            || wheel.phase === Qt.ScrollEnd)
                        editorFlick.scrollByTouchpad(wheel);
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: stopAnimatedScrolling()

            function scrollByTouchpad(wheel) {
                if (wheel.phase === Qt.ScrollMomentum) {
                    touchpadMomentum.stop();
                    touchpadGestureActive = false;
                    platformMomentumActive = true;
                    touchpadEventGap.restart();
                    applyTouchpadDelta(wheel.pixelDelta.y);
                    return;
                }

                if (wheel.phase === Qt.ScrollBegin || !touchpadGestureActive) {
                    touchpadMomentum.stop();
                    platformMomentumActive = false;
                    touchpadGestureActive = true;
                    touchpadVelocity = 0;
                    touchpadLastEventTime = Date.now();
                }

                if (wheel.pixelDelta.y !== 0) {
                    var now = Date.now();
                    var movement = -wheel.pixelDelta.y * touchpadScale;
                    var elapsed = now - touchpadLastEventTime;
                    if (elapsed > 0 && elapsed <= touchpadEventGapMs) {
                        var measuredVelocity = movement * 1000 / elapsed;
                        touchpadVelocity += (measuredVelocity - touchpadVelocity)
                            * touchpadVelocityBlend;
                    }
                    touchpadLastEventTime = now;
                    applyTouchpadDelta(wheel.pixelDelta.y);
                    touchpadEventGap.restart();
                }

                if (wheel.phase === Qt.ScrollEnd) {
                    touchpadEventGap.stop();
                    finishTouchpadGesture();
                }
            }

            function applyTouchpadDelta(pixelDeltaY) {
                wheelScroll.stop();
                var maxY = Math.max(0, contentHeight - height);
                var target = clampContentY(contentY - pixelDeltaY * touchpadScale);
                contentY = target === 0 || target === maxY ? target : snapToPixel(target);
                if (target === 0 || target === maxY)
                    touchpadVelocity = 0;
            }

            function finishTouchpadGesture() {
                if (!touchpadGestureActive)
                    return;
                touchpadGestureActive = false;
                touchpadMomentum.begin(touchpadVelocity);
            }

            function stopTouchpadScrolling() {
                touchpadEventGap.stop();
                touchpadMomentum.stop();
                touchpadGestureActive = false;
                platformMomentumActive = false;
                touchpadVelocity = 0;
            }

            function stopAnimatedScrolling() {
                wheelScroll.stop();
                stopTouchpadScrolling();
            }

            function scrollByWheel(wheel) {
                stopTouchpadScrolling();
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
                stopAnimatedScrolling();
                contentY = snapToPixel(y);
            }

            // Keep the editing caret within the viewport so writing past the
            // bottom edge scrolls the page along with the text.
            function ensureCursorVisible() {
                var margin = win.editorFontPixelSize * 2;
                var cursorTop = editor.y + editor.cursorRectangle.y;
                var cursorBottom = cursorTop + editor.cursorRectangle.height;
                var maxContentY = Math.max(0, contentHeight - height);

                if (cursorBottom + margin > contentY + height)
                    scrollTo(Math.min(maxContentY, cursorBottom + margin - height));
                else if (cursorTop - margin < contentY)
                    scrollTo(Math.max(0, cursorTop - margin));
            }

            TextEdit {
                id: editor
                objectName: "sourceEditor"
                visible: !win.previewMode
                x: Math.round((editorFlick.width - width) / 2)
                y: Math.max(42, Math.round(win.height * 0.05))
                width: win.editorWidth
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                text: ""
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                persistentSelection: true
                activeFocusOnPress: true
                color: win.textColor
                selectedTextColor: win.strongTextColor
                selectionColor: win.selectionFill
                font.family: backend.editorFontFamily
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                // Native rendering hints glyphs to the pixel grid, which is
                // crispest at whole scale factors but misplaces and unevenly
                // rasterizes glyphs at fractional ones (and goes stale when
                // the compositor delivers the fractional scale after the
                // first frame). Fall back to Qt's scalable renderer there.
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                // A line caret is iA Writer's: a thin accent-coloured bar, not
                // a hairline in the text colour. A block caret covers the glyph
                // it sits on, so it is drawn translucent to keep it readable.
                // The panel behind fenced code. It is drawn here rather than
                // set on the document because Qt Quick's TextEdit paints
                // character backgrounds but ignores block ones -- a block
                // background is simply never rendered, which is why the code
                // came out striped: what showed was the character background
                // ending at the last glyph of each line.
                Repeater {
                    id: codePanels
                    objectName: "codePanels"
                    model: editor.fencedRanges

                    Rectangle {
                        // On the Rectangle, not the Repeater: a Repeater is
                        // not a visual parent, so its z reaches nothing. The
                        // panel has to sit behind the TextEdit's own text.
                        z: -1
                        // positionToRectangle() reports where the text sits
                        // now and says nothing when that moves, so these read
                        // the tick as well: without it the panel keeps the
                        // geometry the first layout happened to have.
                        readonly property rect head: {
                            editor.layoutTick;
                            return editor.positionToRectangle(modelData.start);
                        }
                        readonly property rect tail: {
                            editor.layoutTick;
                            return editor.positionToRectangle(modelData.end);
                        }
                        x: -8
                        width: editor.width + 16
                        y: head.y
                        height: Math.max(0, tail.y + tail.height - head.y)
                        color: backend.themeCodeBackground
                        radius: 2
                    }
                }

                // Recomputed when the text changes rather than bound to it:
                // the ranges come from the document, which QML cannot observe.
                property var fencedRanges: []
                // Bumped whenever the text is laid out again, so the panels
                // can depend on something that actually changes.
                property int layoutTick: 0
                onContentHeightChanged: layoutTick++
                onContentWidthChanged: layoutTick++
                function refreshFencedRanges() {
                    fencedRanges = backend.fencedRanges();
                    layoutTick++;
                }
                cursorDelegate: Rectangle {
                    id: caret
                    width: backend.caretStyle === "block"
                        ? Math.max(2, Math.round(writerFontMetrics.averageCharacterWidth))
                        : 2
                    color: backend.themeAccent
                    opacity: backend.caretStyle === "block" ? 0.45 : 1

                    // Qt does not blink a custom cursor delegate, so the blink
                    // is ours, on the desktop's own flash time.
                    SequentialAnimation on visible {
                        running: backend.caretBlink && editor.activeFocus
                        loops: Animation.Infinite
                        PropertyAction { value: true }
                        PauseAnimation { duration: Math.max(100, Qt.styleHints.cursorFlashTime / 2) }
                        PropertyAction { value: false }
                        PauseAnimation { duration: Math.max(100, Qt.styleHints.cursorFlashTime / 2) }
                        onStopped: caret.visible = true
                    }
                }
                onCursorRectangleChanged: editorFlick.ensureCursorVisible()

                function replaceSelectionWith(replacement) {
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    EditorMutations.replaceRange(editor, start, end, replacement);
                }

                function wrapSelection(before, after) {
                    forceActiveFocus();
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    var selected = text.slice(start, end);
                    EditorMutations.replaceRange(editor, start, end,
                                                 before + selected + after,
                                                 before.length,
                                                 before.length + selected.length);
                }

                function insertLink() {
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    var selected = text.slice(start, end);
                    var url = backend.clipboardUrl();
                    var label = selected.length > 0 ? selected : "link text";
                    var destination = url.length > 0 ? url : "https://";
                    var escapedLabel = escapeMarkdownLinkText(label);
                    var markdown = "[" + escapedLabel + "](" + escapeMarkdownLinkDestination(destination) + ")";
                    if (selected.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     1, 1 + escapedLabel.length);
                    } else if (url.length === 0) {
                        EditorMutations.replaceRange(editor, start, end, markdown,
                                                     escapedLabel.length + 3,
                                                     markdown.length - 1);
                    } else {
                        EditorMutations.replaceRange(editor, start, end, markdown);
                    }
                }

                function smartReturn(softBreak) {
                    if (softBreak) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    var lineStart = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                    var line = text.slice(lineStart, cursorPosition);
                    var before = text.slice(0, cursorPosition);
                    var fences = (before.match(/^\s*```/gm) || []).length;
                    if ((fences % 2) === 1) {
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
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    var head = text.slice(text.lastIndexOf("\n", start - 1) + 1, start);
                    var lineEnd = text.indexOf("\n", end);
                    var rest = lineEnd < 0 ? text.slice(end)
                                           : text.slice(end, lineEnd);
                    if (/^\s*$/.test(head) && /^\s*$/.test(rest)) {
                        replaceSelectionWith("\n");
                        return;
                    }
                    // Off by default: a Return that opens a whole paragraph
                    // surprises anyone who just wanted the next line, and the
                    // blank line is one more Return away when it is wanted.
                    replaceSelectionWith(backend.paragraphOnReturn ? "\n\n" : "\n");
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

                function pasteClipboardUrlAsMarkdownLink() {
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    if (start === end)
                        return false;

                    var url = backend.clipboardUrl();
                    if (url === "")
                        return false;

                    var selected = text.slice(start, end);
                    var leading = selected.match(/^\s*/)[0];
                    var trailing = selected.match(/\s*$/)[0];
                    var linkText = selected.slice(leading.length,
                                                  selected.length - trailing.length);
                    if (linkText === "")
                        return false;

                    replaceSelectionWith(leading + "[" + escapeMarkdownLinkText(linkText) + "]("
                                         + escapeMarkdownLinkDestination(url) + ")" + trailing);
                    return true;
                }

                function pasteClipboardAsPlainText() {
                    var pastedText = backend.clipboardText();
                    if (pastedText.length > 0)
                        replaceSelectionWith(pastedText);
                }

                function skipHiddenForward(position) {
                    var pos = position;
                    var ranges = backend.hiddenRangesAt(pos);
                    for (var i = 0; i < ranges.length; i++) {
                        if (pos >= ranges[i].start && pos < ranges[i].end) {
                            pos = ranges[i].end;
                            i = -1;
                        }
                    }
                    return pos;
                }

                function skipHiddenBackward(position) {
                    var pos = position;
                    var ranges = backend.hiddenRangesAt(pos);
                    for (var i = ranges.length - 1; i >= 0; i--) {
                        if (pos > ranges[i].start && pos <= ranges[i].end) {
                            pos = ranges[i].start;
                            i = ranges.length;
                        }
                    }
                    return pos;
                }

                function moveCursorVisibly(direction) {
                    if (selectionStart !== selectionEnd) {
                        cursorPosition = direction > 0
                            ? Math.max(selectionStart, selectionEnd)
                            : Math.min(selectionStart, selectionEnd);
                        return;
                    }

                    var pos = Math.max(0, Math.min(text.length, cursorPosition + direction));
                    cursorPosition = direction > 0
                        ? skipHiddenForward(pos)
                        : skipHiddenBackward(pos);
                }

                function movePage(direction, extendSelection) {
                    var pageStep = Math.max(win.editorFontPixelSize,
                                            editorFlick.height - win.editorFontPixelSize * 2);
                    var rect = cursorRectangle;
                    var targetY = rect.y + rect.height / 2 + direction * pageStep;
                    var target = positionAt(rect.x, Math.max(0, targetY));
                    if (extendSelection)
                        moveCursorSelection(target, TextEdit.SelectCharacters);
                    else
                        cursorPosition = target;
                }

                function deleteParagraphBreakBehindCursor() {
                    if (selectionStart !== selectionEnd || cursorPosition < 2)
                        return false;

                    if (text.slice(cursorPosition - 2, cursorPosition) !== "\n\n")
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

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    var pasteKey = (event.key === Qt.Key_V)
                        && (event.modifiers & Qt.ControlModifier)
                        && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier | Qt.ShiftModifier));
                    var shiftInsert = (event.key === Qt.Key_Insert)
                        && (event.modifiers & Qt.ShiftModifier)
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier));
                    if (pasteKey || shiftInsert) {
                        if (!pasteClipboardUrlAsMarkdownLink())
                            pasteClipboardAsPlainText();
                        event.accepted = true;
                        return;
                    }

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
                               && (event.key === Qt.Key_PageDown || event.key === Qt.Key_PageUp)) {
                        movePage(event.key === Qt.Key_PageDown ? 1 : -1,
                                 event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    }
                }

                onTextChanged: {
                    refreshFencedRanges();
                    if (win.searchUpdating)
                        return;
                    var contentChanged = backend.editorTextChanged();
                    if (win.searchOpen && contentChanged)
                        win.updateSearch();
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    text: "# Start writing"
                    visible: editor.text.length === 0 && !editor.activeFocus
                    color: win.mutedColor
                    font.family: editor.font.family
                    font.pixelSize: editor.font.pixelSize
                    font.weight: editor.font.weight
                }

                Component.onCompleted: {
                    backend.attachDocument(textDocument);
                    refreshFencedRanges();
                    forceActiveFocus();
                }
            }

            // Read-only rendered view. Qt renders the Markdown itself, so it
            // shows real heading sizes and list structure the highlighter can
            // only tint in place. It never calls attachDocument, so the
            // highlighter stays bound to the source editor alone.
            TextEdit {
                id: preview
                objectName: "previewView"
                visible: win.previewMode
                x: Math.round((editorFlick.width - width) / 2)
                y: editor.y
                width: win.editorWidth
                height: Math.max(editorFlick.height - y - 96, implicitHeight + 20)
                textFormat: TextEdit.RichText
                wrapMode: TextEdit.Wrap
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                color: win.textColor
                selectedTextColor: win.strongTextColor
                selectionColor: win.selectionFill
                font.family: "iA Writer Mono S"
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                onLinkActivated: function(link) { Qt.openUrlExternally(link); }

                function refresh() {
                    if (win.previewMode)
                        backend.renderPreview(textDocument);
                }

                onVisibleChanged: refresh()

                Connections {
                    target: editor
                    function onTextChanged() { preview.refresh(); }
                }
            }
        }

        Rectangle {
            id: footer
            objectName: "footer"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Math.max(36, win.scaledSize(36))
            color: win.pageColor

            Row {
                id: footerStatus
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 12
                anchors.bottomMargin: 10
                spacing: 12
                opacity: 0.55

                FooterIconButton {
                    objectName: "saveButton"
                    visible: !backend.nativeMacChrome
                    iconName: "save"
                    iconColor: win.mutedColor
                    tooltip: "Save"
                    onClicked: backend.save()
                }

                FooterIconButton {
                    objectName: "openButton"
                    visible: !backend.nativeMacChrome
                    iconName: "open"
                    iconColor: win.mutedColor
                    tooltip: "Open"
                    onClicked: backend.openDialog()
                }

                Label {
                    text: backend.status
                    color: win.mutedColor
                    font.family: win.chromeFontFamily
                    font.pixelSize: win.scaledSize(11)
                    visible: text !== ""
                    elide: Text.ElideRight
                    width: Math.min(360, win.width / 3)
                    height: win.scaledSize(16)
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // The right of the footer is what the document is doing: which
            // way you are looking at it, and how long it has got. File actions
            // stay on the left. The button had no anchors at all and sat on
            // top of the save icon.
            Row {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: 12
                anchors.bottomMargin: 10
                spacing: 12
                layoutDirection: Qt.RightToLeft

                Label {
                    objectName: "wordCountLabel"
                    // With a target set the count becomes progress against it, and
                    // names the draft goal, which is deliberately a quarter longer.
                    text: backend.wordTarget > 0
                        ? backend.wordCount + " / " + backend.wordTarget + " Words  ("
                          + backend.draftTargetFor(backend.wordTarget) + " draft)"
                        : backend.wordCount + (backend.wordCount === 1 ? " Word" : " Words")
                    color: win.mutedColor
                    opacity: 0.75
                    font.family: win.chromeFontFamily
                    font.pixelSize: win.scaledSize(11)
                    height: win.scaledSize(16)
                    verticalAlignment: Text.AlignVCenter
                }

                FooterIconButton {
                    objectName: "previewButton"
                    visible: !backend.nativeMacChrome
                    iconName: win.previewMode ? "edit" : "preview"
                    iconColor: win.previewMode ? backend.themeAccent : win.mutedColor
                    opacity: win.previewMode ? 0.9 : 0.55
                    tooltip: win.previewMode ? "Back to source" : "Preview"
                    onClicked: win.togglePreview()

                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }
            }
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
            Material.elevation: backend.nativeMacChrome ? 0 : 8

            background: Rectangle {
                radius: backend.nativeMacChrome ? 6 : 9
                color: backend.nativeMacChrome
                       ? macPalette.base
                       : (win.darkMode ? "#22221f" : "#fffef2")
            }

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: searchField
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: win.replaceOpen ? parent.height / 2 : parent.height
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.strongTextColor
                        font.pixelSize: win.scaledSize(17)
                        clip: true
                        onTextChanged: win.updateSearch()
                        Keys.onReturnPressed: function(event) {
                            win.moveSearch((event.modifiers & Qt.ShiftModifier) ? -1 : 1);
                            event.accepted = true;
                        }
                        Keys.onEscapePressed: function(event) {
                            win.closeSearch();
                            event.accepted = true;
                        }
                    }

                    TextInput {
                        id: replaceField
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height / 2
                        visible: win.replaceOpen
                        verticalAlignment: TextInput.AlignVCenter
                        color: win.textColor
                        selectionColor: win.selectionFill
                        selectedTextColor: win.strongTextColor
                        font.pixelSize: win.scaledSize(17)
                        Keys.onReturnPressed: replaceCurrentButton.clicked()
                    }

                    Label {
                        anchors.verticalCenter: replaceField.verticalCenter
                        text: "Replace with"
                        visible: win.replaceOpen && replaceField.text.length === 0
                        color: win.mutedColor
                        font.pixelSize: win.scaledSize(17)
                    }

                    Label {
                        anchors.verticalCenter: searchField.verticalCenter
                        text: "Find"
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
                    visible: win.replaceOpen
                    text: "Replace"
                    onClicked: {
                        if (win.searchMatchIndex < 0) return;
                        var start = win.searchMatches[win.searchMatchIndex];
                        EditorMutations.replaceRange(editor, start,
                                                     start + searchField.text.length,
                                                     replaceField.text);
                        win.updateSearch();
                    }
                }

                Button {
                    visible: win.replaceOpen
                    text: "All"
                    onClicked: {
                        if (searchField.text.length === 0) return;
                        for (var i = win.searchMatches.length - 1; i >= 0; --i) {
                            var start = win.searchMatches[i];
                            EditorMutations.replaceRange(editor, start,
                                                         start + searchField.text.length,
                                                         replaceField.text);
                        }
                        win.updateSearch();
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
        if (geometry.maximized) showMaximized();
    }

    Component.onDestruction: backend.saveWindowGeometry(x, y, width, height, visibility === Window.Maximized)

}
