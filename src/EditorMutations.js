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

// With nothing selected, act on the span around the caret: at the end of
// its text the caret steps past the closing marker, so typing carries on
// plain (and steps back in on a second press); anywhere else in it the span
// comes off.
function toggleAtCaret(editor, span) {
    var start = editor.selectionStart;
    if (start !== editor.selectionEnd || !span || span.start === undefined) return false;
    if (start === span.end) {
        editor.cursorPosition = span.textEnd;
    } else if (start >= span.textEnd) {
        editor.cursorPosition = span.end;
    } else {
        var content = editor.rawText().slice(span.contentStart, span.contentEnd);
        var caret = Math.max(0, start - span.contentStart);
        replaceRange(editor, span.start, span.end, content, caret, caret);
    }
    return true;
}

// Unwraps when a whole marker is selected or just outside the selection,
// otherwise wraps. The '*' of '**bold**' is not italic; '***' counts as both.
function toggleWrap(editor, marker, span) {
    if (toggleAtCaret(editor, span)) return;
    var start = editor.selectionStart;
    var end = editor.selectionEnd;
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

// Underline is HTML in Markdown, with asymmetric opening/closing markers.
function toggleUnderline(editor, span) {
    if (toggleAtCaret(editor, span)) return;
    var start = editor.selectionStart, end = editor.selectionEnd;
    var text = editor.rawText(), body = text.slice(start, end);
    var lower = body.toLowerCase();
    var tags = /<\/?u>/i;
    if (lower.startsWith("<u>") && lower.endsWith("</u>")
        && unescapedAt(text, start) && unescapedAt(text, end - 4)
        && !tags.test(body.slice(3, -4))) {
        body = body.slice(3, -4);
        replaceRange(editor, start, end, body, 0, body.length);
    } else if (text.slice(start - 3, start).toLowerCase() === "<u>"
               && text.slice(end, end + 4).toLowerCase() === "</u>"
               && unescapedAt(text, start - 3) && unescapedAt(text, end) && !tags.test(body)) {
        replaceRange(editor, start - 3, end + 4, body, 0, body.length);
    } else {
        replaceRange(editor, start, end, "<u>" + body + "</u>", 3, 3 + body.length);
    }
}

// Code delimiters must be longer than literal backtick runs in the body.
function codeDelimiter(body, minimum) {
    var runs = body.match(/`+/g) || [];
    var count = minimum;
    runs.forEach(function(run) { count = Math.max(count, run.length + 1); });
    return "`".repeat(count);
}

function unescapedAt(text, position) {
    var count = 0;
    while (position > 0 && text.charAt(--position) === "\\") ++count;
    return count % 2 === 0;
}

function singleCodeSpan(text, opening, marker, body) {
    if (!body.length || body.startsWith("`") || body.endsWith("`") || !unescapedAt(text, opening)) return false;
    if (marker.length >= 3 && (opening === 0 || text.charAt(opening - 1) === "\n")
        && body.indexOf("\n") >= 0) return false; // A fenced block, not an inline span.
    return !(body.match(/`+/g) || []).some(function(run) { return run.length === marker.length; });
}

function singleCodeBlock(body, marker) {
    return !(new RegExp("^ {0,3}`{" + marker.length + ",}[ \\t]*$", "m")).test(body);
}

function toggleInlineCode(editor) {
    var start = editor.selectionStart, end = editor.selectionEnd;
    var text = editor.text, body = text.slice(start, end);
    var wrapped = body.match(/^(`+)([\s\S]*?)\1$/);
    var left = text.slice(0, start).match(/(`+)( ?)$/);
    var right = text.slice(end).match(/^( ?)(`+)/);
    if (wrapped && text.charAt(start - 1) !== "`" && text.charAt(end) !== "`"
        && singleCodeSpan(text, start, wrapped[1], wrapped[2])) {
        body = wrapped[2];
        if (body.startsWith(" ") && body.endsWith(" ") && body.trim().length > 0) body = body.slice(1, -1);
        replaceRange(editor, start, end, body, 0, body.length);
    } else if (left && right && left[1] === right[2] && left[2] === right[1]
        && ((body.length === 0 && unescapedAt(text, start - left[0].length))
            || singleCodeSpan(text, start - left[0].length, left[1], left[2] + body + right[1]))) {
        replaceRange(editor, start - left[0].length, end + right[0].length, body, 0, body.length);
    } else {
        var marker = codeDelimiter(body, 1);
        var pad = body.startsWith("`") || body.endsWith("`")
            || (body.startsWith(" ") && body.endsWith(" ") && body.trim().length > 0) ? " " : "";
        replaceRange(editor, start, end, marker + pad + body + pad + marker,
                     marker.length + pad.length, marker.length + pad.length + body.length);
    }
}

// Insert the marker and its definition atomically, so one Undo removes both.
function insertReference(editor, id, footnote, clipboardUrl, clipboardText,
                         linkPlaceholder, notePlaceholder, prependDefinition) {
    var source = editor.rawText();
    var start = editor.selectionStart, end = editor.selectionEnd;
    var selected = source.slice(start, end);
    var escapeLabel = function(text) { return text.replace(/\\/g, "\\\\").replace(/\[/g, "\\[").replace(/\]/g, "\\]"); };
    var marker, definition, selectFrom, selectTo;
    var placeholder = false;
    if (footnote) {
        var body = normalizePlainText(selected || clipboardText || notePlaceholder);
        placeholder = !selected && !clipboardText;
        marker = "[^" + id + "]";
        definition = "[^" + id + "]: " + body.replace(/\n/g, "\n    ");
        selectFrom = selectTo = marker.length;
    } else {
        var label = normalizePlainText(selected || (!clipboardUrl ? clipboardText : "")).replace(/\n/g, " ") || linkPlaceholder;
        placeholder = !selected && (clipboardUrl.length > 0 || !clipboardText);
        marker = "[" + escapeLabel(label) + "][" + id + "]";
        var destination = (clipboardUrl || "https://").replace(/</g, "%3C").replace(/>/g, "%3E");
        definition = "[" + id + "]: <" + destination + ">";
        selectFrom = placeholder ? 1 : marker.length;
        selectTo = placeholder ? 1 + escapeLabel(label).length : marker.length;
    }
    var tail = source.slice(end);
    var replacement, definitionStart;
    if (prependDefinition) {
        // An unfinished code fence at EOF would swallow an appended definition.
        var prefix = definition + "\n\n";
        replacement = prefix + source.slice(0, start) + marker + tail;
        definitionStart = 0;
        selectFrom += prefix.length + start; selectTo += prefix.length + start;
        start = 0;
    } else {
        var beforeDefinition = source.slice(0, start) + marker + tail;
        var separator = beforeDefinition.endsWith("\n\n") ? "" : beforeDefinition.endsWith("\n") ? "\n" : "\n\n";
        replacement = marker + tail + separator + definition + "\n";
        definitionStart = marker.length + tail.length + separator.length;
    }
    if (footnote && placeholder) {
        selectFrom = definitionStart + id.length + 5;
        selectTo = selectFrom + notePlaceholder.length;
    } else if (!footnote && !placeholder && !clipboardUrl) {
        selectFrom = definitionStart + id.length + 5;
        selectTo = selectFrom + 8; // Editable https:// destination, not angle brackets.
    }
    replaceRange(editor, start, source.length, replacement, selectFrom, selectTo);
}

function toggleCodeBlock(editor) {
    var start = editor.selectionStart, end = editor.selectionEnd;
    var text = editor.text, body = text.slice(start, end);
    var wrapped = body.match(/^(`{3,})[^`\n]*\n([\s\S]*?)\n\1\n?$/);
    var left = text.slice(0, start).match(/(^|\n)(`{3,})[^`\n]*\n$/);
    var right = text.slice(end).match(/^\n?(`{3,})(?=\n|$)/);
    if (wrapped && (start === 0 || text.charAt(start - 1) === "\n")
        && (end === text.length || text.charAt(end) === "\n") && singleCodeBlock(wrapped[2], wrapped[1])) {
        replaceRange(editor, start, end, wrapped[2], 0, wrapped[2].length);
    } else if (left && right && left[2] === right[1] && singleCodeBlock(body, left[2])) {
        var fenceStart = start - left[0].length + left[1].length;
        replaceRange(editor, fenceStart, end + right[0].length, body, 0, body.length);
    } else {
        var marker = codeDelimiter(body, 3);
        var prefix = start > 0 && text.charAt(start - 1) !== "\n" ? "\n" : "";
        var suffix = end < text.length && text.charAt(end) !== "\n" ? "\n" : "";
        var opening = prefix + marker + "\n";
        replaceRange(editor, start, end, opening + body + (body.endsWith("\n") ? "" : "\n") + marker + suffix,
                     opening.length, opening.length + body.length);
    }
}
