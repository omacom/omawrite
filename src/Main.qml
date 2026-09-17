import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs as Dialogs
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
        onActivated: editor.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.WindowShortcut
        onActivated: editor.redo()
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
            text: "Ctrl+S  Save\nCtrl+Shift+S  Save As\nCtrl+O  Open\nCtrl+N  New Window\nCtrl+F  Find\nCtrl+H  Find and Replace\nCtrl+B  Bold\nCtrl+I  Italic\nCtrl+K  Link\nCtrl+P  Print\nF11 / Super+F  Fullscreen\nCtrl+?  Shortcuts"
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
                active: hovered || pressed || wheelScroll.running
                        || pixelFling.running || scrollLinger.running
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

            // Finger-lift is ScrollEnd on Wayland; compositors that never
            // send it still start a fling after a short gap in pixel events.
            // 16ms after End lets ScrollMomentum (macOS, some compositors)
            // claim the coast instead of stacking on our fling.
            Timer {
                id: flingFallback
                interval: 48
                repeat: false
                onTriggered: editorFlick.beginFling()
            }

            // Two Chrome/macOS paths, because Flickable's own wheel flick is
            // sized by the tiny application font and touchpad pixel events
            // would otherwise rebound to the drag origin:
            // - mouse notches: ease-out toward a running 120px target
            // - touchpad: accelerated tracking, then an exponential fling
            readonly property real wheelStep: win.scaledSize(120)
            readonly property real flingTau: 0.46
            readonly property real minFlingVelocity: 150
            readonly property real flingBoost: 1.2
            property var velocitySamples: []
            property bool compositorMomentum: false
            property bool inPixelGesture: false
            property real lastFingerAt: 0

            Timer {
                id: pixelGestureIdle
                interval: 200
                repeat: false
                onTriggered: editorFlick.inPixelGesture = false
            }

            FrameAnimation {
                id: wheelScroll
                running: false

                property real startY: 0
                property real targetY: 0
                property real duration: 0.3
                // Ease-out (0.2, 0.7, 0.22, 1): starts promptly without the
                // Impulse curve's front-loaded jump, so a notch glides.
                // Retarget tilts the first handle to inherit velocity.
                property real cx1: 0.2
                property real cy1: 0.7
                property real cx2: 0.22
                property real cy2: 1

                onTriggered: {
                    var x = elapsedTime / duration;
                    if (x >= 1) {
                        editorFlick.contentY = editorFlick.snapToPixel(targetY);
                        stop();
                        return;
                    }
                    editorFlick.contentY =
                        startY + (targetY - startY) * curveY(solveCurve(x));
                }

                function begin(from, to, dur, slope) {
                    startY = from;
                    targetY = to;
                    duration = dur;
                    cx1 = 0.2;
                    cy1 = 0.7;
                    cx2 = 0.22;
                    cy2 = 1;
                    if (slope > 0) {
                        if (cx1 * slope < 1)
                            cy1 = cx1 * slope;
                        else
                            cx1 = cy1 / slope;
                    }
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
                    var slope = 0;
                    if (velocity !== 0 && delta / velocity > 0) {
                        dur = Math.min(dur, delta / velocity * 4);
                        slope = velocity * dur / delta;
                    }
                    begin(pos, newTarget, dur, slope);
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

            FrameAnimation {
                id: pixelFling
                running: false

                property real startY: 0
                property real velocity: 0

                onTriggered: {
                    var decay = Math.exp(-elapsedTime / editorFlick.flingTau);
                    var next = startY + velocity * editorFlick.flingTau * (1 - decay);
                    var clamped = editorFlick.clampContentY(next);
                    if (Math.abs(velocity * decay) < 40 || clamped !== next) {
                        editorFlick.contentY = editorFlick.snapToPixel(clamped);
                        stop();
                        return;
                    }
                    editorFlick.contentY = clamped;
                }
            }

            WheelHandler {
                // Wayland compositors route every pointer's scroll through
                // one seat device that Qt classifies as a touchpad, so the
                // device type cannot tell a mouse wheel from two-finger
                // scrolling. Distinguish by event shape: pixelDelta is a
                // touchpad, angle-only notches are a mouse wheel.
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function(wheel) {
                    scrollLinger.restart();
                    if (editorFlick.isPreciseScroll(wheel))
                        editorFlick.scrollByPixels(wheel);
                    else
                        editorFlick.scrollByWheel(wheel);
                    wheel.accepted = true;
                }
            }

            onMovementStarted: {
                wheelScroll.stop();
                pixelFling.stop();
                flingFallback.stop();
            }

            function isPreciseScroll(wheel) {
                if (wheel.pixelDelta.y !== 0)
                    return true;
                // Stay on the pixel path until the finger gesture goes idle
                // so ScrollEnd can start a fling. Mouse notches never set
                // inPixelGesture and must not take this path.
                return inPixelGesture;
            }

            function scrollByPixels(wheel) {
                var phase = wheel.phase;
                var now = Date.now();

                if (wheel.pixelDelta.y !== 0) {
                    inPixelGesture = true;
                    pixelGestureIdle.restart();
                }

                // Catch a coast only after a real pause or a new contact,
                // never on the next delta of the same swipe.
                if (pixelFling.running
                    && (phase === Qt.ScrollBegin || now - lastFingerAt > 140)) {
                    pixelFling.stop();
                    flingFallback.stop();
                    velocitySamples = [];
                    contentY = snapToPixel(contentY);
                }

                if (phase === Qt.ScrollBegin)
                    compositorMomentum = false;

                if (phase === Qt.ScrollMomentum) {
                    compositorMomentum = true;
                    pixelFling.stop();
                    flingFallback.stop();
                    if (wheel.pixelDelta.y !== 0)
                        applyPixelDelta(wheel.pixelDelta.y, false);
                    lastFingerAt = now;
                    return;
                }

                if (phase === Qt.ScrollEnd) {
                    lastFingerAt = now;
                    pixelGestureIdle.restart();
                    if (!compositorMomentum) {
                        flingFallback.interval = 16;
                        flingFallback.restart();
                    }
                    return;
                }

                if (wheel.pixelDelta.y !== 0) {
                    applyPixelDelta(wheel.pixelDelta.y, true);
                    lastFingerAt = now;
                }

                if (!compositorMomentum) {
                    flingFallback.interval = 48;
                    flingFallback.restart();
                }
            }

            function applyPixelDelta(dy, accelerate) {
                wheelScroll.stop();
                pixelFling.stop();
                var now = Date.now();
                var samples = velocitySamples;
                var applied = dy;
                if (accelerate) {
                    var dt = samples.length > 0 ? now - samples[samples.length - 1].t : 16;
                    dt = Math.max(8, Math.min(48, dt));
                    applied = dy * pixelGain(Math.abs(dy) / dt * 1000);
                }
                samples.push({ t: now, dy: applied });
                while (samples.length > 1 && now - samples[0].t > 80)
                    samples.shift();
                if (samples.length > 12)
                    samples.shift();
                velocitySamples = samples;
                contentY = clampContentY(contentY - applied);
            }

            // Slow two-finger moves stay 1:1; faster flicks ramp toward ~2.25x
            // the way a Mac trackpad covers more ground as you throw it.
            function pixelGain(speed) {
                var t = Math.max(0, Math.min(1, (speed - 80) / 2000));
                t = t * t * (3 - 2 * t);
                return 1 + 1.25 * t;
            }

            function beginFling() {
                if (compositorMomentum)
                    return;

                var samples = velocitySamples;
                velocitySamples = [];
                if (samples.length < 2)
                    return;

                var dt = samples[samples.length - 1].t - samples[0].t;
                if (dt < 24)
                    return;
                if (Date.now() - samples[samples.length - 1].t > 80)
                    return;

                var sum = 0;
                for (var i = 0; i < samples.length; i++)
                    sum += samples[i].dy;
                var velocity = -sum / dt * 1000 * flingBoost;
                if (Math.abs(velocity) < minFlingVelocity)
                    return;

                var cap = 9500;
                if (velocity > cap)
                    velocity = cap;
                else if (velocity < -cap)
                    velocity = -cap;

                pixelFling.startY = contentY;
                pixelFling.velocity = velocity;
                pixelFling.restart();
            }

            function scrollByWheel(wheel) {
                // High-resolution wheels report fractional notches; feed
                // those through the same animated path, like Chromium does
                // for every wheel-source event.
                var notches = wheel.angleDelta.y / 120;
                if (notches === 0)
                    return;

                pixelFling.stop();
                flingFallback.stop();
                compositorMomentum = false;
                inPixelGesture = false;
                velocitySamples = [];

                if (wheelScroll.running) {
                    wheelScroll.retarget(clampContentY(wheelScroll.targetY - notches * wheelStep));
                    return;
                }

                var target = clampContentY(contentY - notches * wheelStep);
                if (target !== contentY)
                    wheelScroll.begin(contentY, target, wheelDuration(target - contentY), 0);
            }

            // ~300ms for a single notch so the ease-out has room to glide,
            // capped so stacked notches still feel like one motion.
            function wheelDuration(delta) {
                return Math.max(0.3, Math.min(0.55, 0.0022 * Math.abs(delta)));
            }

            function clampContentY(y) {
                return Math.max(0, Math.min(Math.max(0, contentHeight - height), y));
            }

            // Whole device pixels keep natively hinted glyphs from
            // re-rasterizing mid-animation, which reads as shimmer.
            function snapToPixel(y) {
                return Math.round(y * Screen.devicePixelRatio) / Screen.devicePixelRatio;
            }

            // Jump to a position, abandoning any scroll animation still running.
            function scrollTo(y) {
                wheelScroll.stop();
                pixelFling.stop();
                flingFallback.stop();
                compositorMomentum = false;
                velocitySamples = [];
                inPixelGesture = false;
                lastFingerAt = 0;
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
                cursorDelegate: Rectangle {
                    width: 1
                    color: win.strongTextColor
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
                    replaceSelectionWith("\n\n");
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
                    forceActiveFocus();
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
