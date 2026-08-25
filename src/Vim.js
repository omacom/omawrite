.pragma library
.import "EditorMutations.js" as EditorMutations

// A small vim engine for the editor. Main.qml turns a key event into a name
// (see vimKeyName there) and hands it to handleKey(), which reports whether it
// consumed the key. Insert mode consumes nothing but Escape, so smart returns,
// Markdown paste and every other editor behaviour keep working while typing.

// ---------------------------------------------------------------- state

function createState() {
    return {
        mode: "normal",     // normal | insert | visual | vline
        count: "",          // digits typed before an operator
        operatorCount: "",  // digits typed between an operator and its motion
        operator: "",       // d, c or y awaiting a motion
        pending: "",        // key awaiting an argument: g, r, f, F, t, T
        anchor: -1,         // fixed end of the visual selection
        head: -1,           // moving end of the visual selection
        column: -1,         // column gj/gk aim for across short lines
        goalX: -1,          // x j/k aim for across short display lines
        register: {text: "", linewise: false},
        registers: {},      // "a to "z, written alongside the unnamed one
        pendingRegister: "", // register named by " for the next command
        lastFind: null,     // {command, character} for ; and ,
        lastChange: null,   // {keys, insert} replayed by .
        lastVisual: null,   // {start, end} lines behind the '<,'> range
        lastSubstitute: "", // pattern reused by an empty :s//
        insertSession: null,
        keys: [],
        replaying: false
    };
}

function statusText(state) {
    var label = state.mode === "insert" ? "INSERT"
        : state.mode === "visual" ? "VISUAL"
        : state.mode === "vline" ? "V-LINE"
        : "NORMAL";
    var pending = (state.pendingRegister === "" ? "" : "\"" + state.pendingRegister)
        + state.count + state.operator + state.operatorCount + state.pending;
    return pending.length > 0 ? label + " " + pending : label;
}

// The engine only ever touches the editor through this wrapper, so the tests
// can drive a bare TextEdit while Main.qml adds the application hooks.
function createHost(editor, hooks) {
    hooks = hooks || {};

    // Everything that needs the document goes through here, never through
    // editor.text: partway through a command's own edits the editor's copy
    // still lags the document, and clamping a position against the short
    // version drags the caret back to where the edit started.
    function currentText() {
        return hooks.text ? hooks.text() : editor.text;
    }

    return {
        text: currentText,
        cursor: function() { return editor.cursorPosition; },
        setCursor: function(position) {
            editor.deselect();
            editor.cursorPosition = clamp(currentText(), position);
        },
        select: function(from, to) {
            var text = currentText();
            editor.cursorPosition = clamp(text, from);
            editor.moveCursorSelection(clamp(text, to));
        },
        deselect: function() { editor.deselect(); },
        selection: function() {
            return {start: Math.min(editor.selectionStart, editor.selectionEnd),
                    end: Math.max(editor.selectionStart, editor.selectionEnd)};
        },
        replace: function(start, end, replacement) {
            EditorMutations.replaceRange(editor, start, end, replacement);
        },
        // The editor's own undo, not the document's: undo runs are ended by
        // formatting-only edits that u would otherwise stop on, spending a
        // press without changing a character.
        undo: function() { editor.undoEdit(); },
        redo: function() { editor.redoEdit(); },
        // A screen line up or down, which is what j and k follow so a wrapped
        // paragraph moves the way it reads. The document is set in 140% line
        // spacing, and the leading between lines is dead space where
        // positionAt resolves a column badly, so scan for the neighbouring
        // line's rectangle first and then read the column at its centre.
        // Finding no further line means the edge of the document, and the
        // caret stays where it is.
        displayLine: function(position, direction, count, goalX) {
            var x = goalX >= 0 ? goalX : editor.positionToRectangle(position).x;
            var at = position;
            for (var i = 0; i < count; i++) {
                var rect = editor.positionToRectangle(at);
                if (rect.height <= 0)
                    break;
                var step = Math.max(2, rect.height / 2);
                var found = -1;
                for (var probe = step; probe <= rect.height * 4; probe += step) {
                    var y = direction > 0 ? rect.y + rect.height + probe : rect.y - probe;
                    if (y < 0)
                        break;
                    var line = editor.positionToRectangle(editor.positionAt(x, y));
                    if (line.y !== rect.y) {
                        found = editor.positionAt(x, line.y + line.height / 2);
                        break;
                    }
                }
                if (found < 0)
                    break;
                at = found;
            }
            return {position: at, goalX: x};
        },
        // Grouping every command into one undo step keeps u the inverse of
        // the command that ran, rather than of the edits it happened to make.
        beginChange: function() { if (hooks.beginChange) hooks.beginChange(); },
        endChange: function() { if (hooks.endChange) hooks.endChange(); },
        // Where a motion may leave the caret: markdown markers are drawn at
        // no width, so resting on one would look like the caret stopped
        // moving. The application hook skips them the way the arrow keys do.
        settle: function(position, direction) {
            return hooks.settle ? hooks.settle(position, direction) : position;
        },
        // Two places where the editor knows more about Markdown than the
        // grammar does, and the grammar should defer rather than guess: what
        // a new line beside a list item looks like, and what pasting a URL
        // over a selection means. Both report where they left things, or
        // that they did nothing, so the plain behaviour can stand in.
        openLine: function(below) {
            return hooks.openLine ? hooks.openLine(below) : -1;
        },
        linkPaste: function(start, end, payload, register) {
            return hooks.linkPaste
                ? hooks.linkPaste(start, end, payload, register)
                : false;
        },
        // "+ and "* reach outside the application, so they are the one pair
        // of registers the engine cannot hold itself.
        clipboard: function(selection) {
            return hooks.clipboard ? hooks.clipboard(selection) : "";
        },
        setClipboard: function(text, selection) {
            if (hooks.setClipboard) hooks.setClipboard(text, selection);
        },
        page: function(direction) { if (hooks.page) hooks.page(direction); },
        search: function() { if (hooks.search) hooks.search(); },
        searchNext: function(direction) { if (hooks.searchNext) hooks.searchNext(direction); },
        commandLine: function(prefill) { if (hooks.commandLine) hooks.commandLine(prefill); },
        save: function() { if (hooks.save) hooks.save(); },
        saveAs: function(path) { if (hooks.saveAs) hooks.saveAs(path); },
        saveAndQuit: function() { if (hooks.saveAndQuit) hooks.saveAndQuit(); },
        quit: function(force) { if (hooks.quit) hooks.quit(force); },
        open: function(path, force) { if (hooks.open) hooks.open(path, force); },
        clearSearch: function() { if (hooks.clearSearch) hooks.clearSearch(); }
    };
}

// ---------------------------------------------------------------- text

function clamp(text, position) {
    return Math.max(0, Math.min(text.length, position));
}

// Characters outside the basic plane take two UTF-16 units, so a single
// column step has to move over both or an emoji comes apart.
function stepForward(text, position, count) {
    var i = clamp(text, position);
    while (count-- > 0 && i < text.length) {
        var code = text.charCodeAt(i);
        i += (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) ? 2 : 1;
    }
    return i;
}

function stepBackward(text, position, count) {
    var i = clamp(text, position);
    while (count-- > 0 && i > 0) {
        var code = text.charCodeAt(i - 1);
        i -= (code >= 0xDC00 && code <= 0xDFFF && i >= 2) ? 2 : 1;
    }
    return i;
}

function characterCount(text, start, end) {
    var characters = 0;
    for (var i = start; i < end; i = stepForward(text, i, 1))
        characters++;
    return characters;
}

function lineStart(text, position) {
    if (position <= 0)
        return 0;
    return text.lastIndexOf("\n", position - 1) + 1;
}

function lineEnd(text, position) {
    var index = text.indexOf("\n", clamp(text, position));
    return index < 0 ? text.length : index;
}

function isBlank(character) {
    return character === " " || character === "\t";
}

function firstNonBlank(text, position) {
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    var i = start;
    while (i < end && isBlank(text.charAt(i)))
        i++;
    return i < end ? i : start;
}

function lineIsEmpty(text, position) {
    var start = lineStart(text, position);
    return lineEnd(text, start) === start;
}

function indentOf(text, position) {
    var start = lineStart(text, position);
    return text.slice(start, firstNonBlank(text, position));
}

function lineNumberPosition(text, number) {
    var position = 0;
    for (var line = 1; line < number; line++) {
        var end = lineEnd(text, position);
        if (end >= text.length)
            return position;
        position = end + 1;
    }
    return position;
}

// Vim keeps the caret on a character, never on the line break past it.
function clampNormal(text, position) {
    position = clamp(text, position);
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    return position > start && position >= end
        ? Math.max(start, stepBackward(text, end, 1))
        : position;
}

// 0 blank, 1 punctuation, 2 word. W and B treat every non-blank alike.
function charClass(character, big) {
    if (character === "" || /\s/.test(character))
        return 0;
    if (big)
        return 2;
    return /[A-Za-z0-9_\u00c0-\uffff]/.test(character) ? 2 : 1;
}

function wordForward(text, position, big) {
    var i = clamp(text, position);
    var start = charClass(text.charAt(i), big);
    while (i < text.length && charClass(text.charAt(i), big) === start && start !== 0)
        i++;
    while (i < text.length && charClass(text.charAt(i), big) === 0)
        i++;
    return i;
}

function wordBackward(text, position, big) {
    var i = clamp(text, position) - 1;
    while (i >= 0 && charClass(text.charAt(i), big) === 0)
        i--;
    if (i < 0)
        return 0;
    var start = charClass(text.charAt(i), big);
    while (i >= 0 && charClass(text.charAt(i), big) === start)
        i--;
    return i + 1;
}

function wordEnd(text, position, big) {
    var i = stepForward(text, position, 1);
    while (i < text.length && charClass(text.charAt(i), big) === 0)
        i = stepForward(text, i, 1);
    if (i >= text.length)
        return stepBackward(text, text.length, 1);
    var start = charClass(text.charAt(i), big);
    var next = stepForward(text, i, 1);
    while (next < text.length && charClass(text.charAt(next), big) === start) {
        i = next;
        next = stepForward(text, i, 1);
    }
    return i;
}

function wordEndBackward(text, position, big) {
    var i = stepBackward(text, position, 1);
    var start = charClass(text.charAt(position), big);
    if (start !== 0)
        while (i > 0 && charClass(text.charAt(i), big) === start)
            i = stepBackward(text, i, 1);
    while (i > 0 && charClass(text.charAt(i), big) === 0)
        i = stepBackward(text, i, 1);
    return Math.max(0, i);
}

// Unlike e, cw on the last character of a word stops there instead of
// running on to the end of the next one.
function changeWordEnd(text, position, big) {
    var here = charClass(text.charAt(position), big);
    if (here !== 0 && charClass(text.charAt(stepForward(text, position, 1)), big) !== here)
        return position;
    return wordEnd(text, position, big);
}

function paragraphForward(text, position) {
    var i = lineEnd(text, position);
    while (i < text.length) {
        i = i + 1;
        if (lineEnd(text, i) === i)
            return i;
        i = lineEnd(text, i);
    }
    return text.length;
}

function paragraphBackward(text, position) {
    var i = lineStart(text, position);
    while (i > 0) {
        i = lineStart(text, i - 1);
        if (lineEnd(text, i) === i)
            return i;
    }
    return 0;
}

// A sentence ends at . ! or ?, past any closing quote or bracket, followed by
// whitespace. Close enough to vim's definition for prose, and it never runs
// past the end of the paragraph.
var SENTENCE_END = /[.!?][)\]"'’]*(?=\s|$)/g;

function sentenceForward(text, position) {
    var paragraph = paragraphForward(text, position);
    SENTENCE_END.lastIndex = position;
    var match = SENTENCE_END.exec(text);
    if (!match || match.index >= paragraph)
        return paragraph;
    var i = match.index + match[0].length;
    while (i < text.length && /\s/.test(text.charAt(i)))
        i++;
    return Math.min(i, paragraph);
}

function sentenceBackward(text, position) {
    var from = paragraphBackward(text, position);
    var starts = [firstNonBlank(text, from === 0 ? 0 : from + 1)];
    SENTENCE_END.lastIndex = from;
    var match;
    while ((match = SENTENCE_END.exec(text)) !== null) {
        var i = match.index + match[0].length;
        while (i < text.length && /\s/.test(text.charAt(i)))
            i++;
        if (i >= position)
            break;
        starts.push(i);
    }
    for (var s = starts.length - 1; s >= 0; s--) {
        if (starts[s] < position)
            return starts[s];
    }
    return Math.max(0, from);
}

function findInLine(text, position, command, character, count, repeated) {
    var forward = command === "f" || command === "t";
    var start = lineStart(text, position);
    var end = lineEnd(text, position);
    var i = position;
    for (var found = 0; found < count; found++) {
        var from = forward ? stepForward(text, i, 1) : stepBackward(text, i, 1);
        // A repeated t or T already sits one short of its target, so it would
        // find the same one again and stand still. A fresh press would not.
        if (repeated && command === "t" && text.charAt(from) === character)
            from = stepForward(text, i, 2);
        else if (repeated && command === "T" && text.charAt(from) === character)
            from = stepBackward(text, i, 2);
        if (forward) {
            i = text.indexOf(character, from);
            if (i < 0 || i >= end)
                return -1;
        } else {
            i = text.lastIndexOf(character, from);
            if (i < 0 || i < start)
                return -1;
        }
    }
    // One character short of the target, not one code unit: the caret has to
    // land on a character or the next x writes half of one to the document.
    if (command === "t")
        return stepBackward(text, i, 1);
    if (command === "T")
        return stepForward(text, i, 1);
    return i;
}

function repeatString(value, count) {
    var result = "";
    for (var i = 0; i < count; i++)
        result += value;
    return result;
}

// ---------------------------------------------------------------- objects

// The spans an operator can take without a motion: iw / aw, ip / ap, the
// quotes and the bracket pairs. The a-form swallows the separator too —
// trailing whitespace for a word, the blank lines under a paragraph, the
// quotes or brackets themselves.
function textObject(text, position, around, kind) {
    if (kind === "w" || kind === "W")
        return wordObject(text, position, around, kind === "W");
    if (kind === "p")
        return paragraphObject(text, position, around);
    if (kind === "\"" || kind === "'" || kind === "`")
        return quoteObject(text, position, around, kind);
    return pairObject(text, position, around, kind);
}

// A run of one character class, which is why iw on a space takes the run of
// spaces. Words never cross a line break.
function wordObject(text, position, around, big) {
    var cls = charClass(text.charAt(position), big);
    var start = position;
    var end = position;
    while (start > 0 && text.charAt(start - 1) !== "\n"
           && charClass(text.charAt(start - 1), big) === cls)
        start--;
    while (end < text.length && text.charAt(end) !== "\n"
           && charClass(text.charAt(end), big) === cls)
        end++;
    if (around) {
        var trailing = end;
        while (trailing < text.length && isBlank(text.charAt(trailing)))
            trailing++;
        // aw takes the space after the word, or the space before it when the
        // word ends the line and there is none after.
        if (trailing > end) {
            end = trailing;
        } else {
            while (start > 0 && isBlank(text.charAt(start - 1)))
                start--;
        }
    }
    return {start: start, end: end, linewise: false};
}

// A run of lines that are all blank or all not, so ip inside a paragraph
// takes the paragraph and ip on the gap between two takes the gap.
function paragraphObject(text, position, around) {
    var first = lineStart(text, position);
    var blank = lineIsEmpty(text, first);
    var last = first;
    while (first > 0 && lineIsEmpty(text, lineStart(text, first - 1)) === blank)
        first = lineStart(text, first - 1);
    while (lineEnd(text, last) < text.length
           && lineIsEmpty(text, lineEnd(text, last) + 1) === blank)
        last = lineEnd(text, last) + 1;

    var end = Math.min(text.length, lineEnd(text, last) + 1);
    if (around) {
        var grown = end;
        while (grown < text.length && lineIsEmpty(text, grown))
            grown = lineEnd(text, grown) + 1;
        if (grown > end)
            end = Math.min(text.length, grown);
        else
            while (first > 0 && lineIsEmpty(text, lineStart(text, first - 1)))
                first = lineStart(text, first - 1);
    }
    return {start: first, end: end, linewise: true};
}

// Quotes pair off from the start of the line, so the caret between the second
// and third quote on a line is outside the first pair, not inside it.
function quoteObject(text, position, around, quote) {
    var open = -1;
    var end = lineEnd(text, position);
    for (var i = lineStart(text, position); i < end; i++) {
        if (text.charAt(i) !== quote)
            continue;
        if (open < 0)
            open = i;
        else if (i >= position)
            return around ? {start: open, end: i + 1, linewise: false}
                          : {start: open + 1, end: i, linewise: false};
        else
            open = -1;
    }
    return null;
}

var OBJECT_PAIRS = {"(": "()", ")": "()", b: "()",
                    "[": "[]", "]": "[]",
                    "{": "{}", "}": "{}", B: "{}",
                    "<": "<>", ">": "<>"};

function pairObject(text, position, around, kind) {
    var pair = OBJECT_PAIRS[kind];
    if (!pair)
        return null;

    var depth = 0;
    var open = -1;
    for (var back = position; back >= 0; back--) {
        if (text.charAt(back) === pair.charAt(1) && back !== position) {
            depth++;
        } else if (text.charAt(back) === pair.charAt(0)) {
            if (depth === 0) {
                open = back;
                break;
            }
            depth--;
        }
    }
    if (open < 0)
        return null;

    depth = 0;
    for (var forward = open + 1; forward < text.length; forward++) {
        if (text.charAt(forward) === pair.charAt(0)) {
            depth++;
        } else if (text.charAt(forward) === pair.charAt(1)) {
            if (depth === 0)
                return around ? {start: open, end: forward + 1, linewise: false}
                              : {start: open + 1, end: forward, linewise: false};
            depth--;
        }
    }
    return null;
}

// ---------------------------------------------------------------- caret

function caret(state, host) {
    return state.mode === "visual" || state.mode === "vline"
        ? state.head
        : host.cursor();
}

function moveCaret(state, host, position) {
    var text = host.text();
    if (state.mode === "visual" || state.mode === "vline") {
        state.head = clampNormal(text, position);
        showSelection(state, host);
        return;
    }
    host.setCursor(clampNormal(text, position));
}

function showSelection(state, host) {
    var text = host.text();
    var head = state.head;
    var anchor = state.anchor;
    if (state.mode === "vline") {
        var from = lineStart(text, Math.min(anchor, head));
        var to = Math.min(text.length, lineEnd(text, Math.max(anchor, head)) + 1);
        if (head <= anchor)
            host.select(to, from);
        else
            host.select(from, to);
    } else if (head >= anchor) {
        host.select(anchor, stepForward(text, head, 1));
    } else {
        host.select(stepForward(text, anchor, 1), head);
    }
}

function selectionRange(state, host) {
    var text = host.text();
    var from = Math.min(state.anchor, state.head);
    var to = Math.max(state.anchor, state.head);
    if (state.mode === "vline")
        return {start: from, end: to, linewise: true};
    return {start: from, end: stepForward(text, to, 1), linewise: false};
}

// ---------------------------------------------------------------- modes

function enterInsert(state, host, position) {
    host.setCursor(position);
    state.mode = "insert";
    state.insertSession = {
        keys: state.keys.slice(),
        start: host.cursor(),
        text: host.text()
    };
    resetPending(state);
}

function leaveInsert(state, host) {
    var session = state.insertSession;
    state.insertSession = null;
    state.mode = "normal";
    if (session && !state.replaying)
        state.lastChange = {keys: session.keys,
                            insert: insertDelta(session.text, host.text(), session.start)};
    stepBackOntoLastCharacter(host);
    resetPending(state);
}

// What an insert session did to the document, as "delete this many characters
// behind the caret, then type this". Replaying the result rather than the
// keystrokes keeps `.` honest about list continuation and Markdown paste,
// which rewrite what a key would otherwise have typed.
function insertDelta(before, after, start) {
    var shared = Math.min(before.length, after.length);
    var prefix = 0;
    while (prefix < shared && before.charAt(prefix) === after.charAt(prefix))
        prefix++;
    var suffix = 0;
    while (suffix < shared - prefix
           && before.charAt(before.length - 1 - suffix) === after.charAt(after.length - 1 - suffix))
        suffix++;
    // Characters rather than code units: the count is measured here and spent
    // somewhere else entirely, so a unit count would delete half a character
    // wherever the repeat happens to land.
    var back = Math.max(0, Math.min(start, start - prefix));
    return {back: characterCount(before, start - back, start),
            text: after.slice(prefix, after.length - suffix)};
}

// Leaving insert puts the caret back on the last character typed, which is
// where normal mode expects it. At column zero there is nothing to step back
// onto, and the caret stays rather than jumping to the line above.
function stepBackOntoLastCharacter(host) {
    var text = host.text();
    var position = host.cursor();
    var start = lineStart(text, position);
    host.setCursor(clampNormal(text, position > start
        ? Math.max(start, stepBackward(text, position, 1))
        : position));
}

function enterVisual(state, host, mode) {
    if (state.mode === mode) {
        leaveVisual(state, host);
        return;
    }
    if (state.mode !== "visual" && state.mode !== "vline") {
        state.anchor = host.cursor();
        state.head = host.cursor();
    }
    state.mode = mode;
    showSelection(state, host);
}

function visualLines(state, host) {
    var text = host.text();
    return {start: lineNumberAt(text, Math.min(state.anchor, state.head)),
            end: lineNumberAt(text, Math.max(state.anchor, state.head))};
}

// Coming back from the search bar or the : line, where the keys belonged to
// something else for a while. The mode and any half-typed command are stale;
// the registers, the last change and the last search are not, and a writer
// who yanked a paragraph before going looking for where it belongs would not
// thank us for emptying them.
function returnToNormal(state) {
    state.mode = "normal";
    state.insertSession = null;
    state.anchor = -1;
    state.head = -1;
    resetPending(state);
}

// An edit from outside the engine leaves the visual anchors pointing at text
// that has moved, so drop the selection. The registers and the last change
// survive: nothing about them went stale.
function cancelVisual(state) {
    if (state.mode !== "visual" && state.mode !== "vline")
        return false;

    state.mode = "normal";
    state.anchor = -1;
    state.head = -1;
    resetPending(state);
    return true;
}

function leaveVisual(state, host) {
    var head = state.head;
    state.lastVisual = visualLines(state, host);
    state.mode = "normal";
    state.anchor = -1;
    state.head = -1;
    host.setCursor(clampNormal(host.text(), head));
}

function resetPending(state) {
    state.count = "";
    state.operatorCount = "";
    state.operator = "";
    state.pending = "";
    state.pendingRegister = "";
}

function effectiveCount(state) {
    var outer = state.count === "" ? 1 : parseInt(state.count, 10);
    var inner = state.operatorCount === "" ? 1 : parseInt(state.operatorCount, 10);
    return Math.max(1, outer * inner);
}

// A command is one edit block, so that one u undoes the command rather than
// the several edits that carried it out. A block left open is not a lost
// command but a lost session: the document holds its change signal back, so
// TextEdit.text freezes and onTextChanged never runs again — no modified
// flag, no word count, no search refresh and no recovery draft, while the
// writer keeps typing into what looks like a working editor. Nothing leaves
// this function without closing the block, including a throw on its way out.
function withEditBlock(host, body) {
    host.beginChange();
    try {
        return body();
    } finally {
        host.endChange();
    }
}

function commitChange(state) {
    if (!state.replaying && state.mode !== "visual" && state.mode !== "vline")
        state.lastChange = {keys: state.keys.slice(), insert: null};
}

// ---------------------------------------------------------------- keys

function handleKey(state, host, key) {
    if (key === "")
        return false;

    if (state.mode === "insert") {
        if (key === "Escape" || key === "C-[" || key === "C-c") {
            leaveInsert(state, host);
            return true;
        }
        return false;
    }

    if (!state.replaying) {
        if (state.count === "" && state.operator === "" && state.pending === "")
            state.keys = [key];
        else
            state.keys.push(key);
    }

    var intended = -1;
    var handled = withEditBlock(host, function() {
        var done = dispatch(state, host, key);
        // Closing the edit block replays the change to the editor, which
        // remaps the caret from where it stood when the edit began and so
        // undoes where the command meant to leave it. Read where the command
        // wanted the caret while the block is still open.
        intended = host.cursor();
        return done;
    });
    // Put it back, but never while a visual selection is up, since moving the
    // caret would drop it.
    if (state.mode !== "visual" && state.mode !== "vline" && host.cursor() !== intended)
        host.setCursor(intended);
    return handled;
}

function dispatch(state, host, key) {
    if (state.pending !== "")
        return argument(state, host, key);

    if (key === "Escape" || key === "C-[" || key === "C-c") {
        if (state.count !== "" || state.operator !== "")
            resetPending(state);
        else if (state.mode === "visual" || state.mode === "vline")
            leaveVisual(state, host);
        else
            host.deselect();
        return true;
    }

    // 0 only counts as a digit once one is already typed; on its own it is
    // the motion to the start of the line.
    var digits = state.operator === "" ? state.count : state.operatorCount;
    if (/^[1-9]$/.test(key) || (key === "0" && digits !== "")) {
        if (state.operator === "")
            state.count += key;
        else
            state.operatorCount += key;
        return true;
    }

    if (key === "g" || key === "r" || key === "f" || key === "F" || key === "t"
            || key === "T" || key === "\"") {
        state.pending = key;
        return true;
    }

    // i and a name a text object while an operator waits or a visual
    // selection is open. Everywhere else they are the insert commands.
    if ((key === "i" || key === "a")
            && (state.operator !== "" || state.mode === "visual" || state.mode === "vline")) {
        state.pending = key;
        return true;
    }

    if (key === "d" || key === "c" || key === "y")
        return operator(state, host, key);

    var motion = evaluateMotion(state, host, key, effectiveCount(state));
    if (motion)
        return applyMotion(state, host, motion);

    if (state.operator !== "") {
        resetPending(state);
        return true;
    }

    return simpleCommand(state, host, key);
}

function argument(state, host, key) {
    var pending = state.pending;
    state.pending = "";

    if (pending === "g") {
        if (key === "g") {
            var target = state.count === "" && state.operatorCount === ""
                ? 0
                : lineNumberPosition(host.text(), effectiveCount(state));
            return applyMotion(state, host, {position: target, linewise: true, toFirstNonBlank: true});
        }
        // gj and gk are the logical-line motions, the mirror of vim, where
        // they are the display-line ones: here j and k already follow the
        // wrapped text, so g is what reaches the line the Markdown has.
        if (key === "j" || key === "k") {
            var delta = key === "j" ? effectiveCount(state) : -effectiveCount(state);
            return applyMotion(state, host,
                               {position: verticalMove(state, host.text(), caret(state, host), delta),
                                keepColumn: true, linewise: true});
        }
        if (key === "e" || key === "E") {
            var text = host.text();
            var back = caret(state, host);
            for (var n = 0; n < effectiveCount(state); n++)
                back = wordEndBackward(text, back, key === "E");
            if (state.operator !== "") {
                // ge is inclusive, and it runs backwards, so the operator
                // takes the caret's own character along with the span.
                applyOperator(state, host, state.operator, back,
                              stepForward(text, host.cursor(), 1), false);
                return true;
            }
            return applyMotion(state, host, {position: back});
        }
        resetPending(state);
        return true;
    }

    // " names the register the next yank, delete or paste uses. It outlives
    // this key, so it is set without resetting the rest of the pending state.
    if (pending === "\"") {
        if (/^[a-z+*]$/.test(key))
            state.pendingRegister = key;
        else
            resetPending(state);
        return true;
    }

    if (pending === "i" || pending === "a")
        return applyObject(state, host, key, pending === "a");

    if (pending === "r") {
        if (key.length === 1) {
            if (state.mode === "visual" || state.mode === "vline") {
                var range = selectionRange(state, host);
                var text = host.text();
                var from = range.start;
                var to = range.end;
                // A linewise range carries the anchors rather than the lines
                // they sit on, and V-LINE r covers those lines whole.
                if (range.linewise) {
                    from = lineStart(text, from);
                    to = lineEnd(text, to);
                }
                leaveVisual(state, host);
                replaceSelection(state, host, key, from, to);
            } else {
                replaceCharacters(state, host, key, effectiveCount(state));
            }
        }
        resetPending(state);
        return true;
    }

    if (key.length !== 1) {
        resetPending(state);
        return true;
    }

    state.lastFind = {command: pending, character: key};
    return findMotion(state, host, pending, key, effectiveCount(state));
}

function applyObject(state, host, key, around) {
    var text = host.text();
    var object = textObject(text, caret(state, host), around, key);
    if (!object) {
        resetPending(state);
        return true;
    }

    if (state.operator !== "") {
        // A linewise object already runs past its last line break, which
        // applyOperator would then widen by another line.
        var end = object.linewise ? Math.max(object.start, object.end - 1) : object.end;
        applyOperator(state, host, state.operator, object.start, end, object.linewise);
        return true;
    }

    // Without an operator the object becomes the selection, so a second one
    // can grow it or an operator can follow.
    state.anchor = object.start;
    // The object's end is past its last character, and the head sits on that
    // character rather than one code unit back from the end of it.
    state.head = clampNormal(text, Math.max(object.start, stepBackward(text, object.end, 1)));
    if (object.linewise)
        state.mode = "vline";
    showSelection(state, host);
    resetPending(state);
    return true;
}

function findMotion(state, host, command, character, count) {
    var position = findInLine(host.text(), caret(state, host), command, character, count);
    if (position < 0) {
        resetPending(state);
        return true;
    }
    var forward = command === "f" || command === "t";
    return applyMotion(state, host, {position: position, inclusive: forward});
}

function operator(state, host, key) {
    if (state.mode === "visual" || state.mode === "vline") {
        var range = selectionRange(state, host);
        state.lastVisual = visualLines(state, host);
        state.mode = "normal";
        state.anchor = -1;
        state.head = -1;
        host.deselect();
        applyOperator(state, host, key, range.start, range.end, range.linewise);
        return true;
    }

    if (state.operator === key) {
        // dd, cc and yy act on whole lines, count of them.
        var text = host.text();
        var start = host.cursor();
        var end = start;
        for (var line = 1; line < effectiveCount(state); line++) {
            var next = lineEnd(text, end);
            if (next >= text.length)
                break;
            end = next + 1;
        }
        applyOperator(state, host, key, start, end, true);
        return true;
    }

    if (state.operator !== "") {
        resetPending(state);
        return true;
    }

    // A selection dragged out with the mouse stands in for a visual range, so
    // d or y after one does what it looks like it should.
    var selection = host.selection();
    if (selection.end > selection.start) {
        host.deselect();
        applyOperator(state, host, key, selection.start, selection.end, false);
        return true;
    }

    state.operator = key;
    return true;
}

function applyMotion(state, host, motion) {
    var text = host.text();
    var origin = caret(state, host);
    if (state.operator !== "") {
        var from = host.cursor();
        var to = motion.position;
        var linewise = !!motion.linewise;
        if (motion.inclusive && to >= from) {
            to = stepForward(text, to, 1);
        } else if (!linewise && to > from && to === lineStart(text, to)
                   && to > lineEnd(text, from)) {
            // An exclusive motion that lands in column one stops at the end
            // of the line before it instead, and from at or before the first
            // word it turns linewise (:h exclusive). Word motions are exempt:
            // dw on the last word of a line reaches the line's end, no more.
            to--;
            linewise = !motion.word && from <= firstNonBlank(text, from);
        }
        applyOperator(state, host, state.operator,
                      Math.min(from, to), Math.max(from, to), linewise);
        return true;
    }

    var position = motion.toFirstNonBlank ? firstNonBlank(text, motion.position) : motion.position;
    moveCaret(state, host, host.settle(position, position >= origin ? 1 : -1));
    if (!motion.keepColumn) {
        state.column = -1;
        state.goalX = -1;
    }
    resetPending(state);
    return true;
}

function evaluateMotion(state, host, key, count) {
    var text = host.text();
    var position = caret(state, host);
    var i;

    switch (key) {
    case "h":
    case "Left":
    case "Backspace":
        return {position: Math.max(lineStart(text, position),
                                   stepBackward(text, position, count))};
    case "l":
    case "Right":
    case " ":
        return {position: Math.min(lineEnd(text, position),
                                   stepForward(text, position, count))};
    case "j":
    case "Down":
    case "k":
    case "Up":
        var down = key === "j" || key === "Down";
        // An operator over j or k takes whole lines, the way it does in vim,
        // where the display-line form is dgj. A bare j or k follows the
        // wrapped text instead, which is how a paragraph reads on screen.
        if (state.operator !== "") {
            var line = verticalMove(state, text, position, down ? count : -count);
            // At the top or bottom of the document there is no line to reach,
            // and a motion that cannot move fails its operator rather than
            // taking the line the caret is already on.
            if (lineStart(text, line) === lineStart(text, position))
                return null;
            return {position: line, keepColumn: true, linewise: true};
        }
        var moved = host.displayLine(position, down ? 1 : -1, count, state.goalX);
        state.goalX = moved.goalX;
        return {position: moved.position, keepColumn: true};
    case "Return":
        return {position: verticalMove(state, text, position, count),
                linewise: true, toFirstNonBlank: true};
    case "w":
    case "W":
        // cw is vim's odd one out: on a non-blank it changes to the end of
        // the word, leaving the space after it alone.
        if (state.operator === "c" && charClass(text.charAt(position), key === "W") !== 0) {
            for (i = 0; i < count; i++)
                position = i === 0
                    ? changeWordEnd(text, position, key === "W")
                    : wordEnd(text, position, key === "W");
            return {position: position, inclusive: true};
        }
        for (i = 0; i < count; i++)
            position = wordForward(text, position, key === "W");
        return {position: position, word: true};
    case "b":
    case "B":
        for (i = 0; i < count; i++)
            position = wordBackward(text, position, key === "B");
        return {position: position};
    case "e":
    case "E":
        for (i = 0; i < count; i++)
            position = wordEnd(text, position, key === "E");
        return {position: position, inclusive: true};
    case "0":
    case "Home":
        return {position: lineStart(text, position)};
    case "^":
        return {position: firstNonBlank(text, position)};
    case "$":
    case "End":
        for (i = 1; i < count; i++)
            position = Math.min(text.length, lineEnd(text, position) + 1);
        return {position: lineEnd(text, position)};
    case "G":
        return {position: state.count === "" && state.operatorCount === ""
                    ? lineStart(text, text.length)
                    : lineNumberPosition(text, count),
                linewise: true, toFirstNonBlank: true};
    case "{":
        for (i = 0; i < count; i++)
            position = paragraphBackward(text, position);
        return {position: position};
    case "}":
        for (i = 0; i < count; i++)
            position = paragraphForward(text, position);
        return {position: position};
    case ")":
        for (i = 0; i < count; i++)
            position = sentenceForward(text, position);
        return {position: position};
    case "(":
        for (i = 0; i < count; i++)
            position = sentenceBackward(text, position);
        return {position: position};
    case ";":
    case ",":
        if (!state.lastFind)
            return null;
        var command = state.lastFind.command;
        if (key === ",")
            command = {f: "F", F: "f", t: "T", T: "t"}[command];
        var found = findInLine(text, position, command, state.lastFind.character, count, true);
        if (found < 0)
            return null;
        return {position: found, inclusive: command === "f" || command === "t"};
    }
    return null;
}

// j and k walk logical lines, as in vim, and hold the column they started
// from so passing through a short line does not drag the caret left.
function verticalMove(state, text, position, delta) {
    // Characters across, not code units: a column counted in units and then
    // measured out on a line holding an astral character lands the caret
    // inside it.
    var column = state.column >= 0
        ? state.column
        : characterCount(text, lineStart(text, position), position);
    state.column = column;
    var start = lineStart(text, position);
    for (var i = 0; i < Math.abs(delta); i++) {
        if (delta > 0) {
            var end = lineEnd(text, start);
            if (end >= text.length)
                break;
            start = end + 1;
        } else {
            if (start === 0)
                break;
            start = lineStart(text, start - 1);
        }
    }
    return Math.min(stepForward(text, start, column), lineEnd(text, start));
}

// ---------------------------------------------------------------- registers

// Yanks and deletes land in the unnamed register, which stays inside the
// editor so an x never costs you what you copied from a browser. "a to "z
// keep text aside, and "+ and "* are the system clipboard and the primary
// selection, for when you do mean to carry text out of the window.
function isClipboardRegister(name) {
    return name === "+" || name === "*";
}

function readRegister(state, host, name) {
    if (isClipboardRegister(name)) {
        var text = EditorMutations.normalizePlainText(host.clipboard(name === "*"));
        // A clipboard carries no linewise flag, so a trailing newline stands
        // in for one, which is how vim's own "+ reads a yanked line too.
        return {text: text,
                linewise: text !== "" && text.charAt(text.length - 1) === "\n"};
    }
    if (name === "")
        return state.register;
    return state.registers[name] || {text: "", linewise: false};
}

function writeRegister(state, host, name, text, linewise) {
    if (isClipboardRegister(name)) {
        host.setClipboard(linewise && text.charAt(text.length - 1) !== "\n"
            ? text + "\n"
            : text, name === "*");
    } else if (name !== "") {
        state.registers[name] = {text: text, linewise: linewise};
    }
    // A named yank fills the unnamed register too, so a bare p still pastes
    // whatever was last taken.
    state.register = {text: text, linewise: linewise};
}

// ---------------------------------------------------------------- edits

function applyOperator(state, host, op, start, end, linewise) {
    var text = host.text();
    var cursor = host.cursor();
    start = clamp(text, start);
    end = Math.max(start, clamp(text, end));

    if (linewise) {
        start = lineStart(text, start);
        end = Math.min(text.length, lineEnd(text, end) + 1);
    }

    var slice = text.slice(start, end);
    if (linewise && !/\n$/.test(slice))
        slice += "\n";

    if (op === "y") {
        writeRegister(state, host, state.pendingRegister, slice, linewise);
        if (start < (linewise ? lineStart(text, cursor) : cursor))
            host.setCursor(linewise ? firstNonBlank(text, start) : start);
        resetPending(state);
        return;
    }

    writeRegister(state, host, state.pendingRegister, slice, linewise);

    if (op === "c" && linewise) {
        // cc empties the line but keeps it, and keeps its indentation so
        // rewriting a list item does not lose the bullet's nesting.
        var indent = indentOf(text, start);
        var contentEnd = lineEnd(text, end > start ? end - 1 : start);
        host.replace(start, contentEnd, indent);
        enterInsert(state, host, start + indent.length);
        return;
    }

    // Deleting through the end of the document takes the line break before it,
    // so the lines above do not gain a trailing empty line.
    if (linewise && end >= text.length && start > 0)
        start -= 1;

    host.replace(start, end, "");

    if (op === "c") {
        enterInsert(state, host, start);
        return;
    }

    var updated = host.text();
    host.setCursor(linewise
        ? firstNonBlank(updated, clamp(updated, start))
        : clampNormal(updated, start));
    commitChange(state);
    resetPending(state);
}

function paste(state, host, after, count) {
    var register = readRegister(state, host, state.pendingRegister);
    if (register.text === "")
        return;

    var text = host.text();
    var position = host.cursor();
    var payload = repeatString(register.text, count);

    if (register.linewise) {
        var at = after ? Math.min(text.length, lineEnd(text, position) + 1) : lineStart(text, position);
        var body = payload;
        // A last line without its own break needs one added ahead of the paste.
        if (at === text.length && text.length > 0 && text.charAt(text.length - 1) !== "\n") {
            body = "\n" + payload.replace(/\n$/, "");
            host.replace(at, at, body);
            host.setCursor(firstNonBlank(host.text(), at + 1));
        } else {
            host.replace(at, at, body);
            host.setCursor(firstNonBlank(host.text(), at));
        }
    } else {
        var target = after
            ? Math.min(lineEnd(text, position), stepForward(text, position, 1))
            : position;
        host.replace(target, target, payload);
        var pasted = host.text();
        host.setCursor(clampNormal(pasted, stepBackward(pasted, target + payload.length, 1)));
    }
    commitChange(state);
}

function deleteCharacters(state, host, forward, count) {
    var text = host.text();
    var position = host.cursor();
    var start = forward
        ? position
        : Math.max(lineStart(text, position), stepBackward(text, position, count));
    var end = forward
        ? Math.min(lineEnd(text, position), stepForward(text, position, count))
        : position;
    if (start === end)
        return;
    writeRegister(state, host, state.pendingRegister, text.slice(start, end), false);
    host.replace(start, end, "");
    host.setCursor(clampNormal(host.text(), start));
    commitChange(state);
}

function replaceCharacters(state, host, character, count) {
    var text = host.text();
    var position = host.cursor();
    var end = Math.min(lineEnd(text, position), stepForward(text, position, count));
    if (end === position)
        return;
    // One replacement character per character replaced, which is not the same
    // as one per code unit once an astral character is in the run.
    var replacement = repeatString(character, characterCount(text, position, end));
    host.replace(position, end, replacement);
    var updated = host.text();
    host.setCursor(clampNormal(updated, stepBackward(updated, position + replacement.length, 1)));
    commitChange(state);
}

// r over a visual selection replaces every character in it, one for one, and
// leaves the line breaks alone so the shape of the selection survives.
function replaceSelection(state, host, character, start, end) {
    var text = host.text();
    var replacement = "";
    for (var i = start; i < end; i = stepForward(text, i, 1))
        replacement += text.charAt(i) === "\n" ? "\n" : character;
    if (replacement === "")
        return;

    host.replace(start, end, replacement);
    host.setCursor(clampNormal(host.text(), start));
    commitChange(state);
}

function toggleCase(state, host, count) {
    var text = host.text();
    var position = host.cursor();
    var end = Math.min(lineEnd(text, position), stepForward(text, position, count));
    if (end === position)
        return;
    var slice = text.slice(position, end).replace(/./g, function(character) {
        var upper = character.toUpperCase();
        return character === upper ? character.toLowerCase() : upper;
    });
    host.replace(position, end, slice);
    // Where the run ends now, not where it ended before: toggling can change
    // the length of what it covers, and ß becoming SS pushes everything after
    // it along.
    host.setCursor(clampNormal(host.text(), position + slice.length));
    commitChange(state);
}

function joinLines(state, host, count) {
    var text = host.text();
    var position = host.cursor();
    var joined = 0;
    for (var i = 0; i < Math.max(1, count - 1); i++) {
        text = host.text();
        var end = lineEnd(text, position);
        if (end >= text.length)
            break;
        var next = end + 1;
        while (next < text.length && (text.charAt(next) === " " || text.charAt(next) === "\t"))
            next++;
        // A join adds the space between the two lines, unless one of them is
        // empty or the first already ends in one.
        var separator = end === lineStart(text, position)
            || next === lineEnd(text, next)
            || (end > 0 && isBlank(text.charAt(end - 1)))
            ? "" : " ";
        host.replace(end, next, separator);
        host.setCursor(end);
        position = end;
        joined++;
    }
    if (joined > 0)
        commitChange(state);
}

function openLine(state, host, below) {
    // The editor opens the line if it can, so a list item or a quote carries
    // its marker down the way it does when Return is pressed in insert mode.
    var opened = host.openLine(below);
    if (opened >= 0) {
        enterInsert(state, host, opened);
        return;
    }

    var text = host.text();
    var position = host.cursor();
    if (below) {
        var end = lineEnd(text, position);
        host.replace(end, end, "\n");
        enterInsert(state, host, Math.min(host.text().length, end + 1));
    } else {
        var start = lineStart(text, position);
        host.replace(start, start, "\n");
        enterInsert(state, host, start);
    }
}

function repeatChange(state, host) {
    var change = state.lastChange;
    if (!change)
        return;

    state.replaying = true;
    resetPending(state);
    for (var i = 0; i < change.keys.length; i++)
        handleKey(state, host, change.keys[i]);

    if (state.mode === "insert") {
        if (change.insert) {
            var position = host.cursor();
            host.replace(stepBackward(host.text(), position, change.insert.back),
                         position, change.insert.text);
        }
        state.mode = "normal";
        state.insertSession = null;
        stepBackOntoLastCharacter(host);
    }
    state.replaying = false;
    resetPending(state);
}

// ---------------------------------------------------------------- commands

function simpleCommand(state, host, key) {
    var text = host.text();
    var position = caret(state, host);
    var count = effectiveCount(state);
    var visual = state.mode === "visual" || state.mode === "vline";

    switch (key) {
    case "i":
        if (visual)
            return true;
        enterInsert(state, host, position);
        return true;
    case "I":
        if (visual)
            return true;
        enterInsert(state, host, firstNonBlank(text, position));
        return true;
    case "a":
        if (visual)
            return true;
        enterInsert(state, host,
                    Math.min(lineEnd(text, position), stepForward(text, position, 1)));
        return true;
    case "A":
        if (visual)
            return true;
        enterInsert(state, host, lineEnd(text, position));
        return true;
    case "o":
        if (visual) {
            var anchor = state.anchor;
            state.anchor = state.head;
            state.head = anchor;
            showSelection(state, host);
            return true;
        }
        openLine(state, host, true);
        return true;
    case "O":
        if (visual)
            return true;
        openLine(state, host, false);
        return true;
    case "v":
        enterVisual(state, host, "visual");
        return true;
    case "V":
        enterVisual(state, host, "vline");
        return true;
    case "x":
    case "Delete":
        if (visual)
            return operator(state, host, "d");
        deleteCharacters(state, host, true, count);
        resetPending(state);
        return true;
    case "X":
        if (visual)
            return operator(state, host, "d");
        deleteCharacters(state, host, false, count);
        resetPending(state);
        return true;
    case "s":
        if (visual)
            return operator(state, host, "c");
        applyOperator(state, host, "c", position,
                      Math.min(lineEnd(text, position), stepForward(text, position, count)),
                      false);
        return true;
    case "S":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "c");
        }
        state.operator = "c";
        return operator(state, host, "c");
    case "D":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "d");
        }
        applyOperator(state, host, "d", position, lineEnd(text, position), false);
        return true;
    case "C":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "c");
        }
        applyOperator(state, host, "c", position, lineEnd(text, position), false);
        return true;
    case "Y":
        if (visual) {
            state.mode = "vline";
            return operator(state, host, "y");
        }
        state.operator = "y";
        return operator(state, host, "y");
    case "p":
    case "P":
        if (visual) {
            var range = selectionRange(state, host);
            // Read before the delete, which writes the replaced text to the
            // unnamed register the way vim does.
            var incoming = readRegister(state, host, state.pendingRegister);
            var register = state.pendingRegister;
            state.mode = "normal";
            state.anchor = -1;
            state.head = -1;
            state.pendingRegister = "";
            host.deselect();

            // A URL pasted over a selection is a Markdown link, which is what
            // Ctrl+V does here too. p defers to the editor for that; P stays
            // the literal paste, and a count means the run was meant as text.
            // Both ends have to be charwise: a V-LINE range carries the raw
            // anchors rather than whole lines, and wrapping those would take
            // half the selection.
            if (key === "p" && count === 1 && !incoming.linewise && !range.linewise
                    && host.linkPaste(range.start, range.end, incoming.text, register)) {
                // The same two steps the plain path below takes on its way
                // out: what was pasted becomes the unnamed register, and the
                // command is the last change. Skipping them left . replaying
                // whatever came before — a dd here takes out a line.
                state.register = incoming;
                commitChange(state);
                resetPending(state);
                return true;
            }

            applyOperator(state, host, "d", range.start, range.end, range.linewise);
            state.register = incoming;
            paste(state, host, false, count);
            resetPending(state);
            return true;
        }
        paste(state, host, key === "p", count);
        resetPending(state);
        return true;
    case "u":
        host.undo();
        host.setCursor(clampNormal(host.text(), host.cursor()));
        resetPending(state);
        return true;
    case "C-r":
        host.redo();
        host.setCursor(clampNormal(host.text(), host.cursor()));
        resetPending(state);
        return true;
    case "J":
        if (visual) {
            var span = Math.max(2, countLines(text, Math.min(state.anchor, state.head),
                                              Math.max(state.anchor, state.head)));
            var top = lineStart(text, Math.min(state.anchor, state.head));
            state.mode = "normal";
            state.anchor = -1;
            state.head = -1;
            host.setCursor(top);
            joinLines(state, host, span);
            resetPending(state);
            return true;
        }
        joinLines(state, host, count);
        resetPending(state);
        return true;
    case "~":
        toggleCase(state, host, count);
        resetPending(state);
        return true;
    case ".":
        resetPending(state);
        repeatChange(state, host);
        return true;
    case ":":
        var prefill = "";
        if (visual) {
            state.lastVisual = visualLines(state, host);
            leaveVisual(state, host);
            prefill = "'<,'>";
        }
        resetPending(state);
        host.commandLine(prefill);
        return true;
    case "/":
        resetPending(state);
        host.search();
        return true;
    case "n":
        resetPending(state);
        host.searchNext(1);
        return true;
    case "N":
        resetPending(state);
        host.searchNext(-1);
        return true;
    case "C-d":
    case "PageDown":
        resetPending(state);
        host.page(1);
        return true;
    case "C-u":
    case "PageUp":
        resetPending(state);
        host.page(-1);
        return true;
    }

    // Normal mode swallows everything else, so stray letters never reach the
    // document; modified keys fall through to the window's shortcuts.
    resetPending(state);
    return key.indexOf("C-") !== 0;
}

// ---------------------------------------------------------------- ex

// The : commands worth having in a writing app: write, quit, open, jump to a
// line, substitute, delete lines. Main.qml owns the input field and calls
// runCommand() with whatever was typed.
var COMMANDS = {
    w: "write", wr: "write", wri: "write", writ: "write", write: "write",
    wa: "write", wall: "write",
    wq: "writequit", wqa: "writequit", wqall: "writequit", x: "writequit",
    xi: "writequit", xit: "writequit", xa: "writequit", exi: "writequit", exit: "writequit",
    q: "quit", qu: "quit", qui: "quit", quit: "quit",
    qa: "quit", qal: "quit", qall: "quit", quita: "quit", quitall: "quit",
    e: "edit", ed: "edit", edi: "edit", edit: "edit",
    noh: "nohlsearch", nohl: "nohlsearch", nohls: "nohlsearch",
    nohlsearch: "nohlsearch",
    d: "delete", de: "delete", del: "delete", dele: "delete", delet: "delete",
    "delete": "delete"
};

function runCommand(state, host, input) {
    var command = String(input === undefined || input === null ? "" : input)
        .replace(/^:+/, "").trim();
    if (command === "")
        return {ok: true, message: ""};

    var range = parseRange(state, host, command);
    var rest = range.rest;

    // A bare range is a jump: :42, :$, :'<,'>
    if (rest === "")
        return range.given ? gotoLine(host, range.end) : {ok: true, message: ""};

    var substituteMatch = rest.match(/^s(?:u|ub|ubs|ubst|ubsti|ubstit|ubstitu|ubstitut|ubstitute)?([^A-Za-z0-9 \t])([\s\S]*)$/);
    if (substituteMatch)
        return substitute(state, host, range, substituteMatch[1], substituteMatch[2]);

    var parts = rest.match(/^([A-Za-z]+)(!?)\s*([\s\S]*)$/);
    if (!parts)
        return {ok: false, message: "Not an editor command: " + rest};

    var name = COMMANDS[parts[1]];
    var force = parts[2] === "!";
    var argument = parts[3].trim();

    switch (name) {
    case "write":
        if (argument === "")
            host.save();
        else
            host.saveAs(argument);
        return {ok: true, message: ""};
    case "writequit":
        if (argument === "") {
            host.saveAndQuit();
        } else {
            host.saveAs(argument);
            host.quit(false);
        }
        return {ok: true, message: ""};
    case "quit":
        host.quit(force);
        return {ok: true, message: ""};
    case "edit":
        host.open(argument, force);
        return {ok: true, message: ""};
    case "nohlsearch":
        host.clearSearch();
        return {ok: true, message: ""};
    case "delete":
        return deleteRange(state, host, range);
    }

    return {ok: false, message: "Not an editor command: " + parts[1]};
}

function lineNumberAt(text, position) {
    var line = 1;
    var index = text.indexOf("\n");
    while (index >= 0 && index < position) {
        line++;
        index = text.indexOf("\n", index + 1);
    }
    return line;
}

function lastLineNumber(text) {
    return lineNumberAt(text, text.length);
}

function parseRange(state, host, command) {
    var text = host.text();
    var last = lastLineNumber(text);
    var current = lineNumberAt(text, host.cursor());
    var rest = command;
    var given = false;
    var start = current;
    var end = current;

    if (rest.charAt(0) === "%") {
        start = 1;
        end = last;
        given = true;
        rest = rest.slice(1);
    } else if (rest.indexOf("'<,'>") === 0) {
        var visual = state.lastVisual || {start: current, end: current};
        start = visual.start;
        end = visual.end;
        given = true;
        rest = rest.slice(5);
    } else {
        var first = parseAddress(rest, current, last);
        if (first) {
            start = first.line;
            end = first.line;
            given = true;
            rest = first.rest;
            if (rest.charAt(0) === ",") {
                var second = parseAddress(rest.slice(1), current, last);
                end = second ? second.line : current;
                rest = second ? second.rest : rest.slice(1);
            }
        }
    }

    if (start > end) {
        var swap = start;
        start = end;
        end = swap;
    }
    return {start: Math.max(1, Math.min(last, start)),
            end: Math.max(1, Math.min(last, end)),
            given: given,
            rest: rest.replace(/^\s+/, "")};
}

function parseAddress(input, current, last) {
    var match = input.match(/^(\d+|\.|\$)/);
    if (!match)
        return null;
    var line = match[1] === "." ? current
        : match[1] === "$" ? last
        : parseInt(match[1], 10);
    return {line: line, rest: input.slice(match[1].length)};
}

function gotoLine(host, line) {
    var text = host.text();
    host.setCursor(firstNonBlank(text, lineNumberPosition(text, line)));
    return {ok: true, message: ""};
}

function deleteRange(state, host, range) {
    var text = host.text();
    var start = lineNumberPosition(text, range.start);
    var end = lineNumberPosition(text, range.end);
    withEditBlock(host, function() {
        applyOperator(state, host, "d", start, end, true);
    });
    var lines = range.end - range.start + 1;
    return {ok: true, message: lines > 1 ? lines + " fewer lines" : ""};
}

// :s/pattern/replacement/flags, where the pattern is a JavaScript regular
// expression. & and \1 in the replacement stand for the match and its groups.
function substitute(state, host, range, separator, body) {
    var parts = splitOnSeparator(body, separator);
    var pattern = parts[0];
    var replacement = parts.length > 1 ? parts[1] : "";
    var flags = parts.length > 2 ? parts[2].trim() : "";

    if (pattern === "")
        pattern = state.lastSubstitute;
    if (pattern === "")
        return {ok: false, message: "No previous substitute"};

    var expression;
    try {
        expression = new RegExp(pattern, flags.indexOf("i") >= 0 ? "gi" : "g");
    } catch (error) {
        return {ok: false, message: "Invalid pattern: " + pattern};
    }
    state.lastSubstitute = pattern;

    var everyMatch = flags.indexOf("g") >= 0;
    var replaced = 0;
    var changedLines = 0;
    var landing = -1;
    var added = 0;

    withEditBlock(host, function() {
        // Bottom up, so replacing a line cannot shift the lines still to come.
        for (var line = range.end; line >= range.start; line--) {
            var text = host.text();
            var start = lineNumberPosition(text, line);
            var end = lineEnd(text, start);
            var source = text.slice(start, end);
            var here = 0;
            var updated = source.replace(expression, function(whole) {
                if (!everyMatch && here > 0)
                    return whole;
                here++;
                return expandReplacement(replacement, arguments);
            });
            if (here === 0)
                continue;
            host.replace(start, end, updated);
            replaced += here;
            changedLines++;
            // A replacement may carry \n, and a source line never does, so
            // this is what the edit added. Every edit still to come is above
            // this one and pushes the landing line down; an edit on the
            // landing line itself extends the change downwards. Vim leaves
            // the caret on the last line the substitution changed, so both
            // count the same way.
            added += updated.split("\n").length - 1;
            // Running bottom up, the first line reached with a match is the last
            // one in the file, which is where vim leaves the caret.
            if (landing < 0)
                landing = line;
        }
    });

    if (replaced === 0)
        return {ok: false, message: "Pattern not found: " + pattern};

    gotoLine(host, landing + added);
    return {ok: true,
            message: replaced > 1
                ? replaced + " substitutions on " + changedLines
                    + (changedLines > 1 ? " lines" : " line")
                : ""};
}

function splitOnSeparator(body, separator) {
    var parts = [];
    var current = "";
    for (var i = 0; i < body.length; i++) {
        var character = body.charAt(i);
        if (character === "\\" && i + 1 < body.length) {
            var next = body.charAt(i + 1);
            // An escaped separator is a literal one; anything else stays
            // escaped for the regular expression to read.
            current += next === separator ? next : character + next;
            i++;
            continue;
        }
        if (character === separator) {
            parts.push(current);
            current = "";
            continue;
        }
        current += character;
    }
    parts.push(current);
    return parts;
}

function expandReplacement(specification, match) {
    var result = "";
    for (var i = 0; i < specification.length; i++) {
        var character = specification.charAt(i);
        if (character === "&") {
            result += match[0];
            continue;
        }
        if (character !== "\\" || i + 1 >= specification.length) {
            result += character;
            continue;
        }
        var next = specification.charAt(++i);
        if (next >= "0" && next <= "9") {
            var group = parseInt(next, 10);
            result += match[group] === undefined ? "" : match[group];
        } else if (next === "n") {
            result += "\n";
        } else if (next === "t") {
            result += "\t";
        } else {
            result += next;
        }
    }
    return result;
}

function countLines(text, start, end) {
    var lines = 1;
    var position = lineStart(text, start);
    while (position < lineStart(text, end)) {
        position = lineEnd(text, position) + 1;
        lines++;
    }
    return lines;
}
