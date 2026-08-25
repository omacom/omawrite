.pragma library

function normalizePlainText(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
}

// Inside a QTextDocument edit block the editor's text property still holds
// the version from before the block opened, and clamping a range against it
// drags the edit back inside the old length. An editor that can tell the
// difference offers liveLength(); one that cannot is never in a block.
function documentLength(editor) {
    return editor.liveLength ? editor.liveLength() : editor.text.length;
}

function replaceRange(editor, rangeStart, rangeEnd, replacement,
                      selectionStartOffset, selectionEndOffset) {
    var limit = documentLength(editor);
    var start = Math.max(0, Math.min(limit, rangeStart));
    var end = Math.max(start, Math.min(limit, rangeEnd));
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
