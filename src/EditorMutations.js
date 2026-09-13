.pragma library

function normalizePlainText(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
}

function previousWhitespaceDelimitedWordStart(text, position) {
    var start = Math.max(0, Math.min(text.length, position));

    while (start > 0 && /\s/.test(text.charAt(start - 1)))
        --start;
    while (start > 0 && !/\s/.test(text.charAt(start - 1)))
        --start;

    return start;
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
