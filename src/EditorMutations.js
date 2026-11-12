.pragma library

function normalizePlainText(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
}

function replaceRange(editor, rangeStart, rangeEnd, replacement,
                      selectionStartOffset, selectionEndOffset) {
    var start = Math.max(0, Math.min(editor.text.length, rangeStart));
    var end = Math.max(start, Math.min(editor.text.length, rangeEnd));
    var insertedText = normalizePlainText(replacement);

    // The editor makes the change as one edit, so a single undo takes it back.
    editor.replaceText(start, end, insertedText);

    // The caret lands after the inserted text unless the caller asks for a
    // selection or caret within it.
    var insertedEnd = start + insertedText.length;
    if (selectionStartOffset !== undefined && selectionEndOffset !== undefined) {
        var selectionStart = Math.max(start,
                                      Math.min(insertedEnd, start + selectionStartOffset));
        var selectionEnd = Math.max(start,
                                    Math.min(insertedEnd, start + selectionEndOffset));
        if (selectionStart === selectionEnd)
            editor.cursorPosition = selectionStart;
        else
            editor.select(selectionStart, selectionEnd);
    } else {
        editor.cursorPosition = insertedEnd;
    }

    return insertedText;
}

// Unwraps when the marker is already there, selected or just outside the
// selection, and wraps otherwise. A marker only counts as a whole run, so the
// "*" of "**bold**" is not taken for italic; "***" counts as both.
//
// With nothing selected it acts on the span around the caret: at the end of
// its text the caret steps past the closing marker, so typing carries on
// plain (and steps back in on a second press); anywhere else in it the span
// comes off.
function toggleWrap(editor, marker, span) {
    var start = editor.selectionStart;
    var end = editor.selectionEnd;
    if (start === end && span && span.start !== undefined) {
        if (start === span.end) {
            editor.cursorPosition = span.textEnd;
        } else if (start >= span.textEnd) {
            editor.cursorPosition = span.end;
        } else {
            var content = editor.rawText().slice(span.contentStart, span.contentEnd);
            var caret = Math.max(0, start - span.contentStart);
            replaceRange(editor, span.start, span.end, content, caret, caret);
        }
        return;
    }
    var text = editor.rawText();
    var size = marker.length;
    var mark = marker.charAt(0);
    var runFrom = function(position, step, limit) {
        var run = 0;
        while (position + run * step !== limit && text.charAt(position + run * step) === mark)
            ++run;
        return run;
    };
    var holds = function(run) {
        return run === size || (mark === "*" && run === 3);
    };
    var hasMarker = function(part) {
        var runs = part.match(new RegExp("\\" + mark + "+", "g")) || [];
        return runs.some(function(run) { return holds(run.length); });
    };
    var selected = text.slice(start, end);
    var inner = selected.slice(size, selected.length - size);

    // A marker in the middle means separate spans, as in "**a** and **b**":
    // unwrapping would break them, so wrap instead.
    if (holds(runFrom(start - 1, -1, -1)) && holds(runFrom(end, 1, text.length))
            && !hasMarker(selected)) {
        replaceRange(editor, start - size, end + size, selected, 0, selected.length);
    } else if (selected.length >= 2 * size && holds(runFrom(start, 1, end))
               && holds(runFrom(end - 1, -1, start - 1)) && !hasMarker(inner)
               && text.charAt(start - 1) !== mark && text.charAt(end) !== mark) {
        replaceRange(editor, start, end, inner, 0, inner.length);
    } else {
        replaceRange(editor, start, end, marker + selected + marker,
                     size, size + selected.length);
    }
}
