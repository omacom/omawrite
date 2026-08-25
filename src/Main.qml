import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "EditorMutations.js" as EditorMutations
import "Vim.js" as Vim

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    title: (backend.modified ? "* " : "") + backend.fileName + " - Omawrite"

    readonly property bool darkMode: backend.darkMode
    readonly property color pageColor: backend.themeBackground
    readonly property color textColor: backend.themeForeground
    readonly property color strongTextColor: backend.themeForeground
    readonly property color mutedColor: darkMode ? "#909191" : "#aeb1b5"
    readonly property color selectionFill: backend.themeSelection
    // The desktop's text size knob (GNOME's text-scaling-factor, which
    // `omarchy display text size` drives) anchored so its 12px default leaves
    // the app at the sizes it was designed around.
    readonly property real textScale: backend.textScale
    readonly property int editorFontPixelSize: scaledSize(20)
    readonly property int editorWidth: Math.min(
        Math.round(writerFontMetrics.averageCharacterWidth * 65),
        Math.max(360, width - Math.round(writerFontMetrics.averageCharacterWidth * 20)))
    property bool closeConfirmed: false
    property bool searchOpen: false
    property bool searchUpdating: false
    property var searchMatches: []
    property int searchMatchIndex: -1
    property url pendingOpenUrl
    property string pendingAction: ""
    property bool replaceOpen: false
    property bool awaitingPendingSave: false
    readonly property bool vimMode: backend.vimMode
    property string vimStatus: ""
    property string vimMessage: ""
    property bool commandOpen: false

    Material.theme: darkMode ? Material.Dark : Material.Light
    Material.accent: backend.themeAccent
    color: pageColor

    onClosing: function(close) {
        if (closeConfirmed || !backend.modified)
            return;

        close.accepted = false;
        pendingAction = "close";
        if (!unsavedChangesDialog.opened)
            unsavedChangesDialog.open();
    }

    function requestOpen(url) {
        if (!backend.modified) {
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
        font.family: "iA Writer Mono S"
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
        editor.leaveDetour();
    }

    function openCommandLine(prefill) {
        vimMessage = "";
        commandField.text = prefill;
        commandOpen = true;
        commandField.forceActiveFocus();
        commandField.cursorPosition = commandField.text.length;
    }

    function closeCommandLine() {
        commandOpen = false;
        commandField.text = "";
        editor.forceActiveFocus();
        editor.leaveDetour();
    }

    function runCommandLine() {
        var typed = commandField.text;
        closeCommandLine();
        var result = Vim.runCommand(editor.vimState, editor.vimHost, typed);
        editor.publishVimStatus();
        vimMessage = result.message;
    }

    Shortcut {
        sequence: "Ctrl+Alt+V"
        context: Qt.ApplicationShortcut
        onActivated: {
            backend.vimMode = !backend.vimMode;
            editor.forceActiveFocus();
        }
    }

    Shortcut {
        sequence: "Ctrl+S"
        context: Qt.ApplicationShortcut
        onActivated: backend.save()
    }

    Shortcut {
        sequence: "Ctrl+H"
        context: Qt.ApplicationShortcut
        onActivated: {
            searchOpen = true;
            replaceOpen = true;
            searchField.forceActiveFocus();
            searchField.selectAll();
        }
    }

    Shortcut {
        sequence: "Ctrl+B"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("**", "**")
    }

    Shortcut {
        sequence: "Ctrl+I"
        context: Qt.WindowShortcut
        onActivated: editor.wrapSelection("*", "*")
    }

    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.WindowShortcut
        onActivated: editor.insertLink()
    }

    Shortcut {
        sequence: "Ctrl+?"
        context: Qt.ApplicationShortcut
        onActivated: shortcutsDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+O"
        context: Qt.ApplicationShortcut
        onActivated: backend.openDialog()
    }

    Shortcut {
        sequence: "Ctrl+N"
        context: Qt.ApplicationShortcut
        onActivated: backend.newWindow()
    }

    Shortcut {
        sequence: "Ctrl+Shift+S"
        context: Qt.ApplicationShortcut
        onActivated: backend.saveAsDialog()
    }

    Shortcut {
        sequence: "Ctrl+P"
        context: Qt.ApplicationShortcut
        onActivated: backend.printDocument()
    }

    Shortcut {
        sequences: ["Meta+F", "F11"]
        context: Qt.ApplicationShortcut
        onActivated: toggleFullScreen()
    }

    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.WindowShortcut
        onActivated: editor.undoEdit()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.WindowShortcut
        onActivated: editor.redoEdit()
    }

    Shortcut {
        sequence: "Ctrl+F"
        context: Qt.ApplicationShortcut
        onActivated: {
            searchOpen = true;
            searchField.forceActiveFocus();
            searchField.selectAll();
        }
    }

    Shortcut {
        sequence: "Ctrl+G"
        context: Qt.ApplicationShortcut
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

        function onCloseAfterSave() {
            win.closeConfirmed = true;
            win.close();
        }

        function onSaveSucceeded() {
            win.awaitingPendingSave = false;
            if (win.pendingAction !== "")
                win.completePendingAction();
        }

        // Vim opens a file on its first line, so put the caret there instead of
        // leaving it wherever replacing the text dropped it, which is the end
        // of the document and reads as a caret adrift below the last line.
        function onDocumentLoaded() {
            if (!win.vimMode)
                return;
            editor.cursorPosition = 0;
            editor.resetVim();
        }

        function onExternalChangeDetected(deleted, locallyModified) {
            externalChangeDialog.deleted = deleted;
            externalChangeDialog.locallyModified = locallyModified;
            externalChangeDialog.open();
        }

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

    UnsavedChangesDialog {
        id: unsavedChangesDialog
        fileName: backend.fileName
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
        contentItem: Label {
            text: "Ctrl+S  Save\nCtrl+Shift+S  Save As\nCtrl+O  Open\nCtrl+N  New Window\nCtrl+F  Find\nCtrl+H  Find and Replace\nCtrl+B  Bold\nCtrl+I  Italic\nCtrl+K  Link\nCtrl+P  Print\nF11 / Super+F  Fullscreen\nCtrl+Alt+V  Vim mode\nCtrl+?  Shortcuts"
            lineHeight: 1.5
        }
    }

    Item {
        anchors.fill: parent

        Flickable {
            id: editorFlick
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            clip: true
            contentWidth: width
            contentHeight: Math.max(height, editor.y + editor.implicitHeight + 220)
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                // Wheel scrolling moves contentY directly rather than
                // flicking the Flickable, so the bar has to be told about
                // that activity; linger briefly after the last event.
                active: hovered || pressed || wheelScroll.running || scrollLinger.running
                // Stop above the footer strip so the bar doesn't overlap
                // the word count in the bottom-right corner. Padding and
                // inset, not anchors: the attached-ScrollBar layout overrides
                // anchors. Padding stops the thumb, the inset the track.
                bottomPadding: win.scaledSize(32)
                bottomInset: win.scaledSize(32)
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
                    if (wheel.pixelDelta.y !== 0)
                        editorFlick.scrollTo(editorFlick.clampContentY(editorFlick.contentY - wheel.pixelDelta.y));
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: wheelScroll.stop()

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
                font.family: "iA Writer Mono S"
                font.pixelSize: win.editorFontPixelSize
                font.weight: Font.Normal
                // Native rendering hints glyphs to the pixel grid, which is
                // crispest at whole scale factors but misplaces and unevenly
                // rasterizes glyphs at fractional ones (and goes stale when
                // the compositor delivers the fractional scale after the
                // first frame). Fall back to Qt's scalable renderer there.
                renderType: Screen.devicePixelRatio % 1 === 0 ? TextEdit.NativeRendering : TextEdit.QtRendering
                // Normal mode sits on a character rather than between two, so
                // it gets the block caret vim writers read the mode from.
                cursorDelegate: Rectangle {
                    width: editor.vimNormalMode
                        ? Math.max(2, Math.round(writerFontMetrics.averageCharacterWidth))
                        : 1
                    color: win.strongTextColor
                    opacity: editor.vimNormalMode ? 0.45 : 1
                }
                onCursorRectangleChanged: editorFlick.ensureCursorVisible()

                property var vimState: Vim.createState()
                // How many of the engine's edit blocks are open. While any is,
                // the editor's own text property lags the document, so both
                // the engine and EditorMutations have to ask the document.
                property int vimEditDepth: 0

                function liveLength() {
                    return vimEditDepth > 0 ? backend.documentText().length : text.length;
                }

                property var vimHost: Vim.createHost(editor, {
                    text: function() {
                        return editor.vimEditDepth > 0 ? backend.documentText() : editor.text;
                    },
                    beginChange: function() {
                        editor.vimEditDepth++;
                        backend.beginEditBlock();
                    },
                    endChange: function() {
                        backend.endEditBlock();
                        editor.vimEditDepth = Math.max(0, editor.vimEditDepth - 1);
                    },
                    settle: function(position, direction) {
                        return direction >= 0
                            ? editor.skipHiddenForward(position)
                            : editor.skipHiddenBackward(position);
                    },
                    openLine: function(below) {
                        return editor.openLineForVim(below);
                    },
                    linkPaste: function(start, end, payload, register) {
                        editor.select(start, end);
                        // "+ and "* ask the clipboard they name, which carries
                        // a uri-list its plain text does not. Every other
                        // register hands its own contents over.
                        var systemRegister = register === "+" || register === "*";
                        var wrapped = editor.pasteUrlAsMarkdownLink(
                            systemRegister ? undefined : payload, register === "*");
                        if (!wrapped) {
                            editor.deselect();
                            return false;
                        }
                        // The replacement leaves the caret past the link, and
                        // normal mode sits on a character rather than after it.
                        editor.cursorPosition = Math.max(start, editor.cursorPosition - 1);
                        return true;
                    },
                    clipboard: function(selection) {
                        return backend.clipboardText(selection);
                    },
                    setClipboard: function(text, selection) {
                        backend.setClipboardText(text, selection);
                    },
                    page: function(direction) { editor.movePage(direction, false); },
                    search: function() {
                        win.searchOpen = true;
                        searchField.forceActiveFocus();
                        searchField.selectAll();
                    },
                    searchNext: function(direction) {
                        win.moveSearch(direction);
                        if (win.searchMatchIndex >= 0) {
                            editor.deselect();
                            editor.cursorPosition = win.searchMatches[win.searchMatchIndex];
                        }
                    },
                    commandLine: function(prefill) { win.openCommandLine(prefill); },
                    save: function() { backend.save(); },
                    saveAs: function(path) { backend.saveAs(backend.resolvePath(path)); },
                    saveAndQuit: function() { backend.saveForClose(); },
                    quit: function(force) {
                        if (force) {
                            backend.discardRecovery();
                            win.closeConfirmed = true;
                        }
                        win.close();
                    },
                    open: function(path, force) {
                        if (path === "") {
                            if (force)
                                backend.reloadFromDisk();
                            else
                                backend.openDialog();
                            return;
                        }
                        var url = backend.resolvePath(path);
                        if (force)
                            backend.open(url);
                        else
                            win.requestOpen(url);
                    },
                    clearSearch: function() {
                        win.searchUpdating = true;
                        backend.setSearchHighlight("", -1);
                        win.searchUpdating = false;
                    }
                })
                readonly property bool vimNormalMode: win.vimMode && vimState.mode !== "insert"

                function resetVim() {
                    vimState = Vim.createState();
                    win.vimMessage = "";
                    if (win.commandOpen)
                        win.closeCommandLine();
                    deselect();
                    if (win.vimMode)
                        cursorPosition = Vim.clampNormal(text, cursorPosition);
                    publishVimStatus();
                }

                // Back from the search bar or the command line. Unlike
                // resetVim this keeps the registers, the last change and the
                // last search, none of which the detour invalidated.
                function leaveDetour() {
                    Vim.returnToNormal(vimState);
                    win.vimMessage = "";
                    deselect();
                    if (win.vimMode)
                        cursorPosition = Vim.clampNormal(text, cursorPosition);
                    publishVimStatus();
                }

                function publishVimStatus() {
                    win.vimStatus = win.vimMode ? Vim.statusText(vimState) : "";
                    // vimState is mutated in place, so nudge the bindings that
                    // read the mode off it.
                    vimStateChanged();
                }

                // Qt key events, named the way the vim engine expects.
                function vimKeyName(event) {
                    switch (event.key) {
                    case Qt.Key_Escape: return "Escape";
                    case Qt.Key_Return:
                    case Qt.Key_Enter: return "Return";
                    case Qt.Key_Backspace: return "Backspace";
                    case Qt.Key_Delete: return "Delete";
                    case Qt.Key_Left: return "Left";
                    case Qt.Key_Right: return "Right";
                    case Qt.Key_Up: return "Up";
                    case Qt.Key_Down: return "Down";
                    case Qt.Key_Home: return "Home";
                    case Qt.Key_End: return "End";
                    case Qt.Key_PageUp: return "PageUp";
                    case Qt.Key_PageDown: return "PageDown";
                    case Qt.Key_Space: return " ";
                    case Qt.Key_Tab: return "Tab";
                    }

                    if (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier | Qt.AltModifier)) {
                        var controlOnly = (event.modifiers & Qt.ControlModifier)
                            && !(event.modifiers & (Qt.MetaModifier | Qt.AltModifier));
                        if (controlOnly && event.key >= Qt.Key_A && event.key <= Qt.Key_Z)
                            return "C-" + String.fromCharCode(event.key).toLowerCase();
                        // Ctrl+[ is the other Escape, and it is not a letter.
                        if (controlOnly && event.key === Qt.Key_BracketLeft)
                            return "C-[";
                        return "";
                    }

                    return event.text.length === 1 && event.text.charCodeAt(0) >= 0x20
                        ? event.text
                        : "";
                }

                // The backend ends an undo run by re-applying formatting the
                // text already has, which keeps undo word-sized but leaves
                // steps that change nothing on screen. Walk past those so every
                // press moves the writing, not just the undo stack.
                //
                // The document is asked rather than the text property, which
                // stands still inside an open edit block: vim runs u inside
                // one, and a caller reading its own text there would see no
                // change and undo the whole stack away.
                function undoEdit() {
                    backend.beginHistoryNavigation();
                    var before = backend.documentText();
                    do {
                        undo();
                    } while (canUndo && backend.documentText() === before);
                    backend.endHistoryNavigation();
                }

                function redoEdit() {
                    backend.beginHistoryNavigation();
                    var before = backend.documentText();
                    do {
                        redo();
                    } while (canRedo && backend.documentText() === before);
                    backend.endHistoryNavigation();
                }

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
                    var match = line.match(listMarkerPattern);
                    if (match) {
                        if (match[3].length === 0)
                            EditorMutations.replaceRange(editor, lineStart,
                                                         cursorPosition, "\n");
                        else
                            replaceSelectionWith("\n" + continuationMarker(line, true));
                        return;
                    }
                    replaceSelectionWith("\n\n");
                }

                readonly property var listMarkerPattern: /^(\s*)([-+*]|\d+[.)]|>+)\s+(.*)$/

                // The bullet, number or quote a new line beside this one should
                // carry, so the list keeps going. Numbers count on downwards,
                // but a line opened above keeps the number that was there.
                function continuationMarker(line, advance) {
                    var match = line.match(listMarkerPattern);
                    if (!match || match[3].length === 0)
                        return "";

                    var marker = match[2];
                    if (advance && /^\d/.test(marker))
                        marker = (parseInt(marker) + 1) + marker.slice(-1);
                    return match[1] + marker + " ";
                }

                // Where vim's o and O go through the editor's own idea of a new
                // line, so a list item or a quote carries its marker the way it
                // does when you press Return. Returns where insert should start.
                function openLineForVim(below) {
                    var start = text.lastIndexOf("\n", cursorPosition - 1) + 1;
                    var end = text.indexOf("\n", cursorPosition);
                    if (end < 0)
                        end = text.length;

                    if (below) {
                        cursorPosition = end;
                        smartReturn(false);
                        return cursorPosition;
                    }

                    var marker = continuationMarker(text.slice(start, end), false);
                    EditorMutations.replaceRange(editor, start, start, marker + "\n");
                    return start + marker.length;
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
                    return pasteUrlAsMarkdownLink(undefined);
                }

                // Wrap the selection as a Markdown link. With no payload the
                // URL comes from the clipboard, which carries a uri-list its
                // plain text does not; vim's registers hand theirs over.
                function pasteUrlAsMarkdownLink(payload, fromSelection) {
                    var start = Math.min(selectionStart, selectionEnd);
                    var end = Math.max(selectionStart, selectionEnd);
                    if (start === end)
                        return false;

                    var url = payload === undefined
                        ? backend.clipboardUrl(fromSelection === true)
                        : backend.normalizedLinkUrl(payload);
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
                    remove(start, cursorPosition);
                    cursorPosition = start;
                    return true;
                }

                Keys.priority: Keys.BeforeItem
                Keys.onPressed: function(event) {
                    // Mid-composition the keys belong to the input method, not
                    // to vim, or a dead key would run a command.
                    if (win.vimMode && !editor.inputMethodComposing) {
                        win.vimMessage = "";
                        var consumed = Vim.handleKey(vimState, vimHost, vimKeyName(event));
                        publishVimStatus();
                        if (consumed) {
                            event.accepted = true;
                            return;
                        }
                    }

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

                    // TextEdit claims the undo and redo keys as built-in text
                    // editing shortcuts, so the Shortcut items above never see
                    // them while the editor has focus. Take them here instead,
                    // where they can go through undoEdit()/redoEdit().
                    var undoModifier = (event.modifiers & Qt.ControlModifier)
                        && !(event.modifiers & (Qt.AltModifier | Qt.MetaModifier));
                    if (undoModifier && event.key === Qt.Key_Z) {
                        if (event.modifiers & Qt.ShiftModifier)
                            redoEdit();
                        else
                            undoEdit();
                        event.accepted = true;
                        return;
                    }
                    if (undoModifier && !(event.modifiers & Qt.ShiftModifier)
                        && event.key === Qt.Key_Y) {
                        redoEdit();
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
                    if (win.searchUpdating)
                        return;
                    // Closing one of vim's edit blocks makes the document
                    // announce itself whether or not anything changed, so
                    // every keystroke arrives here. Only a real edit counts.
                    var contentChanged = backend.editorTextChanged();
                    // An edit from outside the engine, like the formatting
                    // shortcuts, leaves a visual selection's anchors pointing
                    // at text that has since moved.
                    if (contentChanged && win.vimMode && Vim.cancelVisual(vimState))
                        publishVimStatus();
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

                Connections {
                    target: win
                    function onVimModeChanged() { editor.resetVim(); }
                }

                Component.onCompleted: {
                    backend.attachDocument(textDocument);
                    forceActiveFocus();
                    resetVim();
                }
            }
        }

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
                iconName: "save"
                iconColor: win.mutedColor
                tooltip: "Save"
                onClicked: backend.save()
            }

            FooterIconButton {
                objectName: "openButton"
                iconName: "open"
                iconColor: win.mutedColor
                tooltip: "Open"
                onClicked: backend.openDialog()
            }

            FooterIconButton {
                objectName: "vimButton"
                iconName: "vim"
                iconColor: backend.vimMode ? backend.themeAccent : win.mutedColor
                tooltip: backend.vimMode ? "Vim mode on" : "Vim mode off"
                onClicked: backend.vimMode = !backend.vimMode
            }

            Label {
                objectName: "vimStatus"
                text: win.vimStatus
                visible: win.vimMode
                color: win.mutedColor
                font.family: "iA Writer Mono S"
                font.pixelSize: win.scaledSize(11)
                height: win.scaledSize(16)
                verticalAlignment: Text.AlignVCenter
            }

            Label {
                objectName: "vimMessage"
                text: win.vimMessage
                visible: text !== ""
                color: win.mutedColor
                font.family: "iA Writer Mono S"
                font.pixelSize: win.scaledSize(11)
                height: win.scaledSize(16)
                elide: Text.ElideRight
                width: Math.min(420, win.width / 3)
                verticalAlignment: Text.AlignVCenter
            }

            Label {
                text: backend.status
                color: win.mutedColor
                font.family: "iA Writer Mono S"
                font.pixelSize: win.scaledSize(11)
                visible: text !== ""
                elide: Text.ElideRight
                width: Math.min(360, win.width / 3)
                height: win.scaledSize(16)
                verticalAlignment: Text.AlignVCenter
            }
        }

        Label {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 12
            anchors.bottomMargin: 10
            text: backend.wordCount + (backend.wordCount === 1 ? " Word" : " Words")
            color: win.mutedColor
            opacity: 0.75
            font.family: "iA Writer Mono S"
            font.pixelSize: win.scaledSize(11)
        }


        // The : command line, along the bottom edge over the footer strip.
        Rectangle {
            objectName: "commandLine"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: win.scaledSize(30)
            visible: win.commandOpen
            z: 10
            color: win.pageColor

            Row {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 0

                Label {
                    text: ":"
                    color: win.textColor
                    font.family: "iA Writer Mono S"
                    font.pixelSize: win.scaledSize(15)
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                }

                TextInput {
                    id: commandField
                    objectName: "commandField"
                    width: parent.width - win.scaledSize(15)
                    height: parent.height
                    verticalAlignment: TextInput.AlignVCenter
                    selectByMouse: true
                    color: win.textColor
                    selectionColor: win.selectionFill
                    selectedTextColor: win.strongTextColor
                    font.family: "iA Writer Mono S"
                    font.pixelSize: win.scaledSize(15)
                    clip: true

                    Keys.onReturnPressed: function(event) {
                        win.runCommandLine();
                        event.accepted = true;
                    }
                    Keys.onEscapePressed: function(event) {
                        win.closeCommandLine();
                        event.accepted = true;
                    }
                    // Rubbing out the last character leaves the command line,
                    // the way backspacing past the : does in vim.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Backspace && text.length === 0) {
                            win.closeCommandLine();
                            event.accepted = true;
                        }
                    }
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
            Material.elevation: 8

            background: Rectangle {
                radius: 9
                color: win.darkMode ? "#22221f" : "#fffef2"
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
