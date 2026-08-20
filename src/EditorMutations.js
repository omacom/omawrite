.pragma library

function normalizePlainText(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
}

function toggleWrap(editor, before, after) {
    var start = Math.min(editor.selectionStart, editor.selectionEnd);
    var end = Math.max(editor.selectionStart, editor.selectionEnd);
    var selected = editor.text.slice(start, end);

    // "*" is also the first half of "**", so a marker is only ours to strip
    // when it is a whole run rather than part of a longer one.
    var openChar = before.charAt(0);
    var closeChar = after.charAt(after.length - 1);

    // Selection includes the markers: strip them.
    if (selected.length >= before.length + after.length
            && selected.startsWith(before) && selected.endsWith(after)
            && selected.charAt(before.length) !== openChar
            && selected.charAt(selected.length - after.length - 1) !== closeChar) {
        var inner = selected.slice(before.length, selected.length - after.length);
        replaceRange(editor, start, end, inner, 0, inner.length);
        return;
    }

    // Selection (or caret) sits directly inside the markers: strip them.
    if (start >= before.length
            && editor.text.slice(start - before.length, start) === before
            && editor.text.slice(end, end + after.length) === after
            && editor.text.charAt(start - before.length - 1) !== openChar
            && editor.text.charAt(end + after.length) !== closeChar) {
        replaceRange(editor, start - before.length, end + after.length, selected,
                     0, selected.length);
        return;
    }

    replaceRange(editor, start, end, before + selected + after,
                 before.length, before.length + selected.length);
}

function replaceRange(editor, rangeStart, rangeEnd, replacement,
                      selectionStartOffset, selectionEndOffset) {
    var start = Math.max(0, Math.min(editor.text.length, rangeStart));
    var end = Math.max(start, Math.min(editor.text.length, rangeEnd));
    var insertedText = normalizePlainText(replacement);

    if (start !== end)
        editor.remove(start, end);

    editor.cursorPosition = start;
    editor.insert(start, insertedText);

    // TextEdit.insert() already leaves the caret after the inserted text. Only
    // move it again when the caller deliberately requests a selection/caret
    // within the replacement.
    if (selectionStartOffset !== undefined && selectionEndOffset !== undefined) {
        var insertedEnd = editor.cursorPosition;
        var selectionStart = Math.max(start,
                                      Math.min(insertedEnd, start + selectionStartOffset));
        var selectionEnd = Math.max(start,
                                    Math.min(insertedEnd, start + selectionEndOffset));
        if (selectionStart === selectionEnd)
            editor.cursorPosition = selectionStart;
        else
            editor.select(selectionStart, selectionEnd);
    }

    return insertedText;
}
