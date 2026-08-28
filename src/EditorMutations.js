.pragma library

function normalizePlainText(text) {
    return text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
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

// Markdown nests a list item under the item above it by indenting the item to
// where that item's own text starts: two columns for `- `, three for `1. `.
// Everything below works in those columns rather than in a fixed indent unit,
// so nesting lines up the way a Markdown reader resolves it.
//
// The editor asks for a "plan" — a range of the document, the text to put
// there, and where the caret lands — and applies it with replaceRange(). The
// same grammar lives again in markdownhighlighter.cpp, which colours these
// lines; the two cannot share code across the C++/QML boundary.

var TAB_WIDTH = 4;

var LIST_ITEM_RE = /^([ \t]*)([-+*]|\d+[.)])([ \t]+)(.*)$/;
var QUOTE_LINE_RE = /^([ \t]*)(>+)([ \t]+)(.*)$/;

function columnWidth(text) {
    var width = 0;
    for (var i = 0; i < text.length; i++) {
        width = text.charAt(i) === "\t"
            ? (Math.floor(width / TAB_WIDTH) + 1) * TAB_WIDTH
            : width + 1;
    }
    return width;
}

function indentOf(width) {
    return " ".repeat(Math.max(0, width));
}

function leadingWhitespace(line) {
    return line.match(/^[ \t]*/)[0];
}

function isBlankLine(line) {
    return line.trim().length === 0;
}

// Indented text under an item: a wrapped paragraph, a code block, anything
// that travels with the item it hangs under.
function isContinuationLine(line) {
    return !isBlankLine(line) && /^[ \t]/.test(line);
}

function markedLine(match, ordered) {
    var prefix = match[1] + match[2] + match[3];
    return {
        indent: columnWidth(match[1]),
        indentText: match[1],
        marker: match[2],
        spacing: match[3],
        content: match[4],
        prefixLength: prefix.length,
        ordered: ordered,
        number: ordered ? parseInt(match[2], 10) : 0,
        delimiter: match[2].slice(-1),
        // Where the line's own text starts, and so where its children nest.
        contentIndent: columnWidth(prefix)
    };
}

function listItem(line) {
    var match = line.match(LIST_ITEM_RE);
    return match ? markedLine(match, /\d/.test(match[2])) : null;
}

// A blockquote carries its marker onto the next line like a list item, but it
// never nests or renumbers.
function quoteLine(line) {
    var match = line.match(QUOTE_LINE_RE);
    return match ? markedLine(match, false) : null;
}

function markedPrefix(line) {
    return listItem(line) || quoteLine(line);
}

function prefixLength(line) {
    var item = listItem(line);
    return item ? item.prefixLength : leadingWhitespace(line).length;
}

function documentLines(text) {
    var lines = text.split("\n");
    var starts = [];
    var offset = 0;
    for (var i = 0; i < lines.length; i++) {
        starts.push(offset);
        offset += lines[i].length + 1;
    }
    return { lines: lines, starts: starts };
}

function lineIndexAt(doc, position) {
    var index = 0;
    while (index + 1 < doc.lines.length && doc.starts[index + 1] <= position)
        index++;
    return index;
}

// The one line around a position, without splitting the whole document.
function lineAt(text, position) {
    var start = position > 0 ? text.lastIndexOf("\n", position - 1) + 1 : 0;
    var end = text.indexOf("\n", position);
    return text.slice(start, end < 0 ? text.length : end);
}

function insideCodeFence(text, position) {
    var before = text.slice(0, position);
    return ((before.match(/^\s*```/gm) || []).length % 2) === 1;
}

// A line still belongs to the surrounding list when it is an item, text
// hanging under one, or a blank line with more of the list beyond it.
function belongsToList(doc, index, step) {
    var scan = index;
    while (scan >= 0 && scan < doc.lines.length && isBlankLine(doc.lines[scan]))
        scan += step;
    if (scan < 0 || scan >= doc.lines.length)
        return false;
    return listItem(doc.lines[scan]) !== null || isContinuationLine(doc.lines[scan]);
}

// The whole list around an edit, because renumbering reaches past the lines
// that moved.
function listRegion(doc, firstLine, lastLine) {
    var start = firstLine;
    var end = lastLine;
    while (start > 0 && belongsToList(doc, start - 1, -1))
        start--;
    while (end + 1 < doc.lines.length && belongsToList(doc, end + 1, 1))
        end++;
    return { start: start, end: end };
}

// The nearest item above a line that sits at maxIndent or shallower. Blank
// lines and text hanging under an item don't interrupt the search; a
// paragraph does, because the list starts after it.
function itemAbove(doc, line, maxIndent) {
    for (var i = line - 1; i >= 0; i--) {
        var candidate = doc.lines[i];
        if (isBlankLine(candidate))
            continue;
        var item = listItem(candidate);
        if (item === null) {
            if (isContinuationLine(candidate))
                continue;
            break;
        }
        if (item.indent <= maxIndent)
            return item;
    }
    return null;
}

// An item nests under the sibling above it, aligning with that sibling's text.
// The first item of a list has nothing to nest under, and neither does one
// that already is a first child.
function nestingIndent(doc, line, anchor) {
    var above = itemAbove(doc, line, anchor.indent);
    return above !== null && above.indent === anchor.indent ? above.contentIndent : -1;
}

// Outdenting lifts an item to sit beside the item it used to hang under.
function outdentIndent(doc, line, anchor) {
    if (anchor.indent === 0)
        return -1;
    var above = itemAbove(doc, line, anchor.indent - 1);
    return above !== null ? above.indent : 0;
}

// Children move with the item they hang under.
function subtreeEnd(doc, lastLine) {
    var item = listItem(doc.lines[lastLine]);
    if (item === null)
        return lastLine;
    var end = lastLine;
    while (end + 1 < doc.lines.length) {
        var line = doc.lines[end + 1];
        var next = listItem(line);
        if (next !== null) {
            if (next.indent <= item.indent)
                break;
        } else if (!isContinuationLine(line)
                   || columnWidth(leadingWhitespace(line)) <= item.indent) {
            break;
        }
        end++;
    }
    return end;
}

// Walk the items of a region, reporting each one with the level it sits in. A
// level is a run of siblings at one indent; a bullet and a number at the same
// indent are separate lists, and unindented prose ends the list outright.
function walkListLevels(lines, visit) {
    var levels = [];
    for (var i = 0; i < lines.length; i++) {
        var line = lines[i];
        if (isBlankLine(line))
            continue;
        var item = listItem(line);
        if (item === null) {
            if (!isContinuationLine(line))
                levels = [];
            continue;
        }
        while (levels.length > 0 && levels[levels.length - 1].indent > item.indent)
            levels.pop();
        var level = levels.length > 0 ? levels[levels.length - 1] : null;
        var starts = level === null || level.indent < item.indent
            || level.ordered !== item.ordered;
        if (starts) {
            if (level !== null && level.indent === item.indent)
                levels.pop();
            level = { indent: item.indent, ordered: item.ordered, next: 0 };
            levels.push(level);
        }
        visit(i, item, level, starts);
    }
}

function levelStartLines(lines) {
    var starts = {};
    walkListLevels(lines, function (index, item, level, isStart) {
        if (isStart)
            starts[index] = true;
    });
    return starts;
}

// Numbers only mean anything in sequence, so every ordered level is renumbered
// once the structure has changed. A level that already opened the same way
// before the edit keeps its first number — a list deliberately starting at 3
// stays there — while a level this edit created starts at 1.
function renumberOrderedItems(lines, keptStarts) {
    walkListLevels(lines, function (index, item, level, isStart) {
        if (!item.ordered)
            return;
        var number = isStart && keptStarts[index] ? item.number
                   : isStart ? 1
                   : level.next;
        level.next = number + 1;
        if (number !== item.number) {
            lines[index] = item.indentText + number + item.delimiter + item.spacing
                + item.content;
        }
    });
}

function offsetInLines(lines, lineIndex, column) {
    var offset = 0;
    for (var i = 0; i < lineIndex; i++)
        offset += lines[i].length + 1;
    return offset + column;
}

// Follow a caret across a rewritten line: text keeps its distance from the
// marker, and a caret inside the marker stays inside the new one.
function mapPosition(doc, region, newLines, position) {
    var line = lineIndexAt(doc, position);
    if (line > region.end)
        return offsetInLines(newLines, newLines.length - 1,
                             newLines[newLines.length - 1].length);
    var index = line - region.start;
    var column = position - doc.starts[line];
    var oldPrefix = prefixLength(doc.lines[line]);
    var newPrefix = prefixLength(newLines[index]);
    var mapped = column >= oldPrefix ? newPrefix + (column - oldPrefix)
                                     : Math.min(column, newPrefix);
    return offsetInLines(newLines, index, Math.min(mapped, newLines[index].length));
}

// Drop text into a range and leave the caret after it.
function textPlan(start, end, replacement) {
    return {
        start: start,
        end: end,
        replacement: replacement,
        selectionStartOffset: replacement.length,
        selectionEndOffset: replacement.length
    };
}

// Narrow a rewritten region down to the lines that actually differ, so an edit
// leaves the rest of the list — and the undo stack — alone. The caret offsets
// come in measured against the whole rewritten region.
function linesPlan(doc, region, newLines, caretStart, caretEnd) {
    var oldLines = doc.lines.slice(region.start, region.end + 1);
    var first = 0;
    while (first < oldLines.length && first < newLines.length
           && oldLines[first] === newLines[first])
        first++;
    if (first === oldLines.length && first === newLines.length)
        return null;

    var oldLast = oldLines.length - 1;
    var newLast = newLines.length - 1;
    while (oldLast >= first && newLast >= first && oldLines[oldLast] === newLines[newLast]) {
        oldLast--;
        newLast--;
    }

    var replacement = newLines.slice(first, newLast + 1).join("\n");
    var dropped = offsetInLines(newLines, first, 0);
    var start;
    var end;
    if (oldLast >= first) {
        start = doc.starts[region.start + first];
        end = doc.starts[region.start + oldLast] + oldLines[oldLast].length;
    } else if (first < oldLines.length) {
        // Lines inserted ahead of a line that stays put.
        start = doc.starts[region.start + first];
        end = start;
        replacement += "\n";
    } else {
        // Lines appended after the region, which has no newline to insert at.
        var tail = oldLines.length - 1;
        start = doc.starts[region.start + tail] + oldLines[tail].length;
        end = start;
        replacement = "\n" + replacement;
        dropped -= 1;
    }
    return {
        start: start,
        end: end,
        replacement: replacement,
        selectionStartOffset: caretStart - dropped,
        selectionEndOffset: caretEnd - dropped
    };
}

function indentPlan(doc, from, to, direction) {
    var firstLine = lineIndexAt(doc, from);
    var lastLine = lineIndexAt(doc, to);
    // A selection ending exactly at a line start stops on the line before it.
    if (lastLine > firstLine && doc.starts[lastLine] === to)
        lastLine--;

    // The shallowest item in the selection sets the level the block moves by;
    // the rest keep their distance from it.
    var anchorLine = -1;
    var anchor = null;
    for (var i = firstLine; i <= lastLine; i++) {
        var item = listItem(doc.lines[i]);
        if (item !== null && (anchor === null || item.indent < anchor.indent)) {
            anchor = item;
            anchorLine = i;
        }
    }
    if (anchor === null)
        return null;

    var target = direction > 0 ? nestingIndent(doc, anchorLine, anchor)
                               : outdentIndent(doc, anchorLine, anchor);
    if (target < 0)
        return null;
    var delta = target - anchor.indent;

    var blockEnd = subtreeEnd(doc, lastLine);
    var region = listRegion(doc, firstLine, blockEnd);
    var newLines = doc.lines.slice(region.start, region.end + 1);
    var keptStarts = levelStartLines(newLines);
    for (i = firstLine; i <= blockEnd; i++) {
        var line = newLines[i - region.start];
        if (isBlankLine(line))
            continue;
        var indent = leadingWhitespace(line);
        newLines[i - region.start] = indentOf(columnWidth(indent) + delta)
            + line.slice(indent.length);
    }
    renumberOrderedItems(newLines, keptStarts);

    return linesPlan(doc, region, newLines,
                     mapPosition(doc, region, newLines, from),
                     mapPosition(doc, region, newLines, to));
}

// Indent (direction 1) or outdent (direction -1) the list items the selection
// touches. Returns null when there is nothing to nest, leaving Tab to whatever
// it did before.
function listIndentPlan(text, selectionStart, selectionEnd, direction) {
    var from = Math.min(selectionStart, selectionEnd);
    var to = Math.max(selectionStart, selectionEnd);
    // Tab outside a list is the common case; answer it from the caret's own
    // line rather than by walking the document.
    if (from === to && !LIST_ITEM_RE.test(lineAt(text, from)))
        return null;
    if (insideCodeFence(text, from))
        return null;
    return indentPlan(documentLines(text), from, to, direction);
}

// A bare marker ends the block: an indented item steps out one level, anything
// else drops out of the list or quote altogether.
function endBlockPlan(doc, lineIndex, from, to) {
    var item = listItem(doc.lines[lineIndex]);
    if (item !== null && item.content.length === 0 && item.indent > 0) {
        var lift = indentPlan(doc, from, to, -1);
        if (lift)
            return lift;
    }
    return textPlan(doc.starts[lineIndex], to, "\n");
}

// Carry the marker onto the next line, splitting the item at the caret and
// renumbering the rest of the list around it.
function continueBlockPlan(doc, lineIndex, from, to) {
    var line = doc.lines[lineIndex];
    var lastLine = lineIndexAt(doc, to);
    var tail = doc.lines[lastLine].slice(to - doc.starts[lastLine]);
    var head = line.slice(0, from - doc.starts[lineIndex]);
    var item = listItem(line);
    if (item === null) {
        var quote = quoteLine(line);
        return textPlan(from, to, "\n" + quote.indentText + quote.marker + quote.spacing);
    }

    var marker = item.ordered ? (item.number + 1) + item.delimiter : item.marker;
    var region = listRegion(doc, lineIndex, lastLine);
    var newLines = doc.lines.slice(region.start, region.end + 1);
    var split = lineIndex - region.start;
    newLines.splice(split, lastLine - lineIndex + 1, head,
                    item.indentText + marker + item.spacing + tail);
    // A new item is always a sibling of the one it follows, so it never opens
    // a level and every level start survives the split.
    renumberOrderedItems(newLines, levelStartLines(newLines));

    // The caret goes where the text carried onto the new line begins, which
    // renumbering can shift but never moves off the end of the line.
    var caret = offsetInLines(newLines, split + 1,
                              newLines[split + 1].length - tail.length);
    return linesPlan(doc, region, newLines, caret, caret);
}

// Return inside a list or a blockquote. What the caret has behind it on the
// line decides which: a marker with text after it carries the marker onto the
// next line, a bare marker ends the block. Returns null where Markdown
// structure doesn't apply, leaving Return to the editor's own default.
function returnPlan(text, selectionStart, selectionEnd) {
    var from = Math.min(selectionStart, selectionEnd);
    var to = Math.max(selectionStart, selectionEnd);
    // A fence suspends the structure below it, so Return is a plain break.
    if (insideCodeFence(text, from))
        return textPlan(from, to, "\n");

    var doc = documentLines(text);
    var lineIndex = lineIndexAt(doc, from);
    var typed = markedPrefix(doc.lines[lineIndex].slice(0, from - doc.starts[lineIndex]));
    if (typed === null)
        return null;
    return typed.content.length === 0 ? endBlockPlan(doc, lineIndex, from, to)
                                      : continueBlockPlan(doc, lineIndex, from, to);
}
