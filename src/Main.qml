import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: (currentDocument.modified ? "* " : "") + currentDocument.fileName + " - " + qsTr("Write")

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
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
    property url pendingOpenUrl
    property string pendingAction: ""
    property bool awaitingPendingSave: false

    // The open documents, one DocumentTab each, in the order of the tab strip.
    property var tabs: []
    property var currentTab: null
    readonly property var currentDocument: currentTab ? currentTab.backend : backend
    // The tab an unsaved-changes prompt or a save dialog is about.
    property var pendingTab: null
    property var saveTarget: null
    // Set when the tab a Save As was opened for closes before it is answered.
    property bool saveTargetClosed: false
    readonly property var promptDocument: pendingTab ? pendingTab.backend : currentDocument
    // Files changed on disk under tabs other than the one being asked about.
    property var externalChanges: []

    readonly property string editorFont: backend.editorFont
    readonly property real averageCharacterWidth: writerFontMetrics.averageCharacterWidth
    readonly property int tabStripHeight: scaledSize(34)

    // The current tab's find bar, as the window's own.
    readonly property bool searchOpen: currentTab ? currentTab.searchOpen : false
    readonly property var searchMatches: currentTab ? currentTab.searchMatches : []
    readonly property int searchMatchIndex: currentTab ? currentTab.searchMatchIndex : -1
    function openSearch(withReplace) { currentTab.openSearch(withReplace); }
    function closeSearch() { currentTab.closeSearch(); }
    function moveSearch(direction) { currentTab.moveSearch(direction); }
    function flushSearch() { currentTab.flushSearch(); }
    function updateSearch(selectMatch) { currentTab.updateSearch(selectMatch); }

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    // Asks about each tab with unsaved changes in turn; the window closes
    // once every one of them is saved or discarded.
    onClosing: function(close) {
        if (closeConfirmed)
            return;
        var tab = nextUnresolvedTab();
        if (!tab) {
            discardResolvedDrafts();
            return;
        }

        close.accepted = false;
        selectTab(tab);
        pendingTab = tab;
        pendingAction = "close";
        if (!unsavedChangesDialog.opened)
            unsavedChangesDialog.open();
    }

    // The window is closing for good, so the drafts of tabs whose changes the
    // writer discarded on the way can go. Until then a Cancel keeps them, and
    // with them the crash recovery of those changes.
    function discardResolvedDrafts() {
        for (var i = 0; i < tabs.length; ++i) {
            if (tabs[i].closeResolved && tabs[i].backend.modified)
                tabs[i].backend.discardRecovery();
        }
    }

    function nextUnresolvedTab() {
        for (var i = 0; i < tabs.length; ++i) {
            if (tabs[i].backend.modified && !tabs[i].closeResolved)
                return tabs[i];
        }
        return null;
    }

    // Opens over the current tab's document, asking first about its changes.
    function requestOpen(url) {
        var tab = currentTab;
        if (!tab.backend.modified) {
            tab.backend.open(url);
            return;
        }
        pendingTab = tab;
        pendingOpenUrl = url;
        pendingAction = "open";
        unsavedChangesDialog.open();
    }

    // A file chosen in the picker gets a tab of its own, unless it is open
    // already or the current tab is still blank.
    function openInTab(url) {
        var existing = tabForUrl(url);
        if (existing) {
            selectTab(existing);
            return;
        }
        if (currentTab && currentTab.isBlank()) {
            currentTab.backend.open(url);
            return;
        }
        newTab(true).backend.open(url);
    }

    function tabForUrl(url) {
        for (var i = 0; i < tabs.length; ++i) {
            if (backend.sameFile(tabs[i].backend.fileUrl, url))
                return tabs[i];
        }
        return null;
    }

    function completePendingAction() {
        var action = pendingAction;
        var tab = pendingTab || currentTab;
        pendingAction = "";
        pendingTab = null;
        if (action === "close") {
            tab.closeResolved = true;
            // On to the next tab with changes, once this prompt has closed.
            Qt.callLater(win.close);
        } else if (action === "open") {
            tab.backend.open(pendingOpenUrl);
        } else if (action === "closeTab") {
            removeTab(tab);
        }
    }

    function cancelPendingAction() {
        awaitingPendingSave = false;
        pendingAction = "";
        pendingTab = null;
        for (var i = 0; i < tabs.length; ++i) {
            if (tabs[i].closeResolved) {
                tabs[i].closeResolved = false;
                tabs[i].backend.holdAutoSave(false);
            }
        }
    }

    // Whether a tab's changes were discarded on the way to closing the window.
    function closeResolvedDocument(document) {
        for (var i = 0; i < tabs.length; ++i) {
            if (tabs[i].backend === document)
                return tabs[i].closeResolved;
        }
        return false;
    }

    function pendingSaveSucceeded(tab) {
        // Only a save the writer chose in the prompt carries out the
        // pending close or open, never an auto-save that lands meanwhile.
        if (pendingTab && pendingTab !== tab)
            return;
        const confirmed = awaitingPendingSave;
        awaitingPendingSave = false;
        if (confirmed && pendingAction !== "")
            completePendingAction();
    }

    function pendingSaveFailed(tab) {
        // A failed save leaves the document as it was, so the close or
        // open it was for is off; a later save must not carry it out.
        if (pendingTab && pendingTab !== tab)
            return;
        cancelPendingAction();
    }

    function showOpenDialog() {
        if (typeof filePicker !== "undefined" && filePicker)
            filePicker.openFile(currentDocument.suggestedOpenDirectory());
    }

    function showSaveDialog(tab, suggestedUrl) {
        if (typeof filePicker === "undefined" || !filePicker)
            return;
        saveTarget = tab;
        saveTargetClosed = false;
        filePicker.saveFile(suggestedUrl);
    }

    function showExternalChange(tab, deleted, locallyModified) {
        if (externalChangeDialog.opened && externalChangeDialog.tab !== tab) {
            externalChanges = externalChanges.concat([
                { tab: tab, deleted: deleted, locallyModified: locallyModified }]);
            return;
        }
        selectTab(tab);
        externalChangeDialog.tab = tab;
        externalChangeDialog.deleted = deleted;
        externalChangeDialog.locallyModified = locallyModified;
        externalChangeDialog.open();
    }

    function showNextExternalChange() {
        while (externalChanges.length > 0 && !externalChangeDialog.opened) {
            var next = externalChanges[0];
            externalChanges = externalChanges.slice(1);
            if (tabs.indexOf(next.tab) >= 0) {
                showExternalChange(next.tab, next.deleted, next.locallyModified);
                return;
            }
        }
    }

    Component {
        id: tabComponent
        DocumentTab {}
    }

    // The first tab holds the window's own backend's document; each tab
    // after it gets a Backend of its own.
    function newTab(select, document) {
        var tab = tabComponent.createObject(tabHost, {
            win: win,
            backend: document || backend.createDocument()
        });
        tabs = tabs.concat([tab]);
        if (select || !currentTab)
            selectTab(tab);
        return tab;
    }

    function selectTab(tab) {
        if (!tab)
            return;
        currentTab = tab;
        tab.editor.forceActiveFocus();
    }

    function selectTabAt(index) {
        if (index >= 0 && index < tabs.length)
            selectTab(tabs[index]);
    }

    function cycleTab(direction) {
        var index = tabs.indexOf(currentTab);
        selectTabAt((index + direction + tabs.length) % tabs.length);
    }

    function closeTab(tab) {
        if (!tab)
            return;
        if (!tab.backend.modified) {
            removeTab(tab);
            return;
        }
        selectTab(tab);
        pendingTab = tab;
        pendingAction = "closeTab";
        unsavedChangesDialog.open();
    }

    // Closing the last tab closes the window.
    function removeTab(tab) {
        var index = tabs.indexOf(tab);
        if (index < 0)
            return;
        if (tabs.length === 1) {
            tab.closeResolved = true;
            Qt.callLater(win.close);
            return;
        }
        var remaining = tabs.slice();
        remaining.splice(index, 1);
        tabs = remaining;
        if (currentTab === tab)
            selectTab(remaining[Math.min(index, remaining.length - 1)]);
        // A Save As still open for this tab must not save another one.
        if (saveTarget === tab) {
            saveTarget = null;
            saveTargetClosed = true;
        }
        var document = tab.backend;
        tab.destroy();
        backend.releaseDocument(document);
    }

    // Drafts a crash left behind, beyond the one the first tab recovers,
    // each come back in a tab of their own.
    function openRecoveredDrafts() {
        var drafts = backend.orphanedDraftCount();
        for (var i = 0; i < drafts; ++i)
            newTab(false);
    }

    function increaseTextSize() { backend.increaseTextSize(); }
    function decreaseTextSize() { backend.decreaseTextSize(); }
    function setEditorFont(family) { backend.editorFont = family; }

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

    property int lastWindowVisibility: Window.Windowed

    onVisibilityChanged: {
        if (visibility !== Window.Hidden && visibility !== Window.Minimized)
            lastWindowVisibility = visibility;
    }

    Shortcut {
        sequence: "Ctrl+S"
        onActivated: win.currentDocument.save()
    }

    Shortcut {
        sequence: "Ctrl+H"
        onActivated: win.openSearch(true)
    }

    Shortcut {
        sequence: "Ctrl+B"
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.wrapSelection("**")
    }

    Shortcut {
        sequence: "Ctrl+I"
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.wrapSelection("*")
    }

    Shortcut {
        sequence: "Ctrl+Shift+X"
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.wrapSelection("~~")
    }

    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.insertLink()
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
        onActivated: win.showOpenDialog()
    }

    Shortcut {
        sequence: "Ctrl+N"
        onActivated: backend.newWindow()
    }

    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: win.currentDocument.saveAsDialog()
    }

    Shortcut {
        sequence: "Ctrl+P"
        onActivated: win.currentDocument.printDocument()
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: win.currentDocument.copyAsHtml()
    }

    Shortcut {
        sequences: ["Meta+F", "F11"]
        onActivated: toggleFullScreen()
    }

    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.WindowShortcut
        onActivated: win.currentTab.editor.redo()
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

    Shortcut {
        sequence: "Ctrl+T"
        onActivated: win.newTab(true)
    }

    Shortcut {
        sequence: "Ctrl+W"
        onActivated: win.closeTab(win.currentTab)
    }

    Shortcut {
        sequences: ["Ctrl+Tab", "Ctrl+PgDown"]
        onActivated: win.cycleTab(1)
    }

    Shortcut {
        // Qt reads Shift+Tab as Backtab, with Shift held or not.
        sequences: ["Ctrl+Shift+Tab", "Ctrl+Backtab", "Ctrl+Shift+Backtab", "Ctrl+PgUp"]
        onActivated: win.cycleTab(-1)
    }

    // Alt+1 to Alt+8 pick a tab by place, and Alt+9 the last one.
    Instantiator {
        model: 9
        delegate: Shortcut {
            required property int index
            sequence: "Alt+" + (index + 1)
            onActivated: index === 8 ? win.selectTabAt(win.tabs.length - 1)
                                     : win.selectTabAt(index)
        }
    }

    // The desktop portal's picker is the app's, so the window says which tab
    // a save is for.
    Connections {
        target: typeof filePicker !== "undefined" ? filePicker : null

        function onOpenSelected(url) {
            win.openInTab(url);
        }

        function onSaveSelected(url) {
            // The tab the chooser was opened for has closed since; saving the
            // one in front instead could write its text over the chosen file.
            if (win.saveTargetClosed) {
                win.saveTargetClosed = false;
                win.cancelPendingAction();
                win.currentDocument.setStatus(qsTr("Not saved: its tab was closed."));
                return;
            }
            var target = win.saveTarget || win.currentTab;
            win.saveTarget = null;
            target.backend.saveAs(url);
        }

        function onCancelled(forSave) {
            if (forSave) {
                win.saveTarget = null;
                win.saveTargetClosed = false;
                win.cancelPendingAction();
            }
        }

        function onFailed(message, forSave) {
            if (forSave) {
                win.saveTarget = null;
                win.saveTargetClosed = false;
                win.cancelPendingAction();
            }
            win.currentDocument.setStatus(message);
        }
    }

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        objectName: "unsavedChangesDialog"
        fileName: win.promptDocument.fileName
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
        property var heldDocument: null
        onAboutToShow: {
            heldDocument = win.promptDocument;
            heldDocument.holdAutoSave(true);
        }
        onClosed: {
            if (heldDocument && !win.closeResolvedDocument(heldDocument))
                heldDocument.holdAutoSave(false);
            heldDocument = null;
        }

        onDiscardRequested: {
            var document = win.promptDocument;
            if (win.pendingAction !== "close") {
                document.discardRecovery();
                win.completePendingAction();
                return;
            }
            // On the way to closing the window, the draft waits for it to
            // close, as a Cancel further on keeps every tab and its crash
            // recovery. Auto-save stays held meanwhile, so discarded changes
            // never reach the file.
            win.completePendingAction();
            document.holdAutoSave(true);
        }

        onSaveRequested: {
            win.awaitingPendingSave = true;
            win.promptDocument.save();
        }
        onCancelRequested: win.cancelPendingAction()
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

        property var tab: null
        onKeepRequested: if (tab) tab.backend.keepExternalVersion()
        onReloadRequested: if (tab) tab.backend.reloadFromDisk()
        onClosed: {
            tab = null;
            Qt.callLater(win.showNextExternalChange);
        }
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

        // Only shown once there is more than one tab, so a single document
        // looks as it always has.
        Item {
            id: tabStrip
            objectName: "tabStrip"
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: win.tabStripHeight
            visible: win.tabs.length > 1
            z: 20

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: win.mutedColor
                opacity: 0.25
            }

            ListView {
                id: tabList
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: 12
                width: Math.min(contentWidth, parent.width - newTabButton.width - 36)
                orientation: ListView.Horizontal
                layoutDirection: Qt.application.layoutDirection
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                model: win.tabs
                currentIndex: win.tabs.indexOf(win.currentTab)
                highlightFollowsCurrentItem: false
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

                delegate: Item {
                    id: tabButton
                    required property var modelData
                    required property int index
                    readonly property var document: modelData.backend
                    readonly property bool active: modelData === win.currentTab
                    width: Math.max(win.scaledSize(110),
                                    Math.min(win.scaledSize(220),
                                             tabLabel.implicitWidth + win.scaledSize(44)))
                    height: tabList.height

                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onClicked: function(mouse) {
                            if (mouse.button === Qt.MiddleButton)
                                win.closeTab(tabButton.modelData);
                            else
                                win.selectTab(tabButton.modelData);
                        }
                    }

                    Label {
                        id: tabLabel
                        anchors.left: parent.left
                        anchors.right: closeTabButton.left
                        anchors.leftMargin: win.scaledSize(12)
                        anchors.rightMargin: win.scaledSize(4)
                        anchors.verticalCenter: parent.verticalCenter
                        text: (tabButton.document.modified ? "* " : "") + tabButton.document.fileName
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: tabButton.active ? win.textColor : win.mutedColor
                        opacity: tabButton.active || tabMouse.containsMouse ? 1 : 0.75
                        font.family: "IBM Plex Mono"
                        font.pixelSize: win.scaledSize(12)
                    }

                    Label {
                        id: closeTabButton
                        anchors.right: parent.right
                        anchors.rightMargin: win.scaledSize(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: "×"
                        color: closeTabMouse.containsMouse ? win.textColor : win.mutedColor
                        opacity: tabButton.active || tabMouse.containsMouse || closeTabMouse.containsMouse ? 1 : 0
                        font.pixelSize: win.scaledSize(16)

                        MouseArea {
                            id: closeTabMouse
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            onClicked: win.closeTab(tabButton.modelData)
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: backend.themeAccent
                        visible: tabButton.active
                    }
                }
            }

            Label {
                id: newTabButton
                objectName: "newTabButton"
                anchors.left: tabList.right
                anchors.leftMargin: win.scaledSize(8)
                anchors.verticalCenter: parent.verticalCenter
                text: "+"
                color: newTabMouse.containsMouse ? win.textColor : win.mutedColor
                font.family: "IBM Plex Mono"
                font.pixelSize: win.scaledSize(18)

                MouseArea {
                    id: newTabMouse
                    anchors.fill: parent
                    anchors.margins: -6
                    hoverEnabled: true
                    onClicked: win.newTab(true)
                }

                ToolTip.visible: newTabMouse.containsMouse
                ToolTip.text: qsTr("New Tab")
            }
        }

        Item {
            id: tabHost
            anchors.fill: parent
            anchors.topMargin: tabStrip.visible ? tabStrip.height : 0
        }
    }

    Component.onCompleted: {
        var geometry = backend.windowGeometry();
        if (geometry.x >= 0) x = geometry.x;
        if (geometry.y >= 0) y = geometry.y;
        width = geometry.width;
        height = geometry.height;
        if (geometry.maximized) showMaximized();
        newTab(true, backend);
    }

    Component.onDestruction: {
        if (lastWindowVisibility !== Window.FullScreen)
            backend.saveWindowGeometry(x, y, width, height, lastWindowVisibility === Window.Maximized);
    }

}

