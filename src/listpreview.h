#pragma once
#include "markdownhighlighter.h"
#include <QStringList>
#include <QList>
#include <QTextDocument>
#include <QTextBlock>
#include <QUuid>
#include <algorithm>

// Conservative, view-only list paragraphs. Checkpoints belong to this parser,
// not QTextBlockUserData, which is reserved for the embedded code lexer.
namespace ListPreview {
struct Item { int block, prefix, depth; QString marker; bool quote = false, soft = false; };
struct Level {
    int indent, content, number; bool ordered; QChar delimiter;
    bool operator==(const Level &b) const {
        return indent == b.indent && content == b.content && number == b.number
            && ordered == b.ordered && delimiter == b.delimiter;
    }
};
struct State {
    QList<Level> levels;
    int fence = 0, fenceIndent = 0;
    QChar fenceMarker;
    bool comment = false, table = false, blank = true;
    QString html;
    int quoteDepth = 0, quoteFence = 0, quoteTicks = 0, quoteBrackets = 0, quoteDestination = 0;
    bool quoteLabel = false, quoteAngle = false;
    QChar quoteTitle;
    QChar quoteFenceMarker;
    QString quoteHtml;
    bool quoteComment = false, quoteTable = false, quoteList = false;
    bool operator==(const State &b) const {
        return levels == b.levels && fence == b.fence && fenceIndent == b.fenceIndent
            && fenceMarker == b.fenceMarker && comment == b.comment && table == b.table
            && blank == b.blank && html == b.html && quoteDepth == b.quoteDepth
            && quoteFence == b.quoteFence && quoteTicks == b.quoteTicks
            && quoteFenceMarker == b.quoteFenceMarker && quoteHtml == b.quoteHtml
            && quoteComment == b.quoteComment && quoteTable == b.quoteTable && quoteList == b.quoteList
            && quoteBrackets == b.quoteBrackets && quoteDestination == b.quoteDestination
            && quoteLabel == b.quoteLabel && quoteAngle == b.quoteAngle && quoteTitle == b.quoteTitle;
    }
};
// Explicit quote prefixes only; never reinterpret indented code or lazy lines.
inline QPair<int, int> quotePrefix(const QString &line, int maximum = 0) {
    int prefix = 0, depth = 0;
    for (;;) {
        if (maximum && depth >= maximum) break;
        int at = prefix, spaces = 0;
        while (at < line.size() && line[at] == ' ' && spaces < 3) { ++at; ++spaces; }
        if (at >= line.size() || line[at] != '>') break;
        ++depth; prefix = at + 1;
        if (prefix < line.size() && (line[prefix] == ' ' || line[prefix] == '\t')) ++prefix;
    }
    return {prefix, depth};
}
inline QList<Item> step(State &state, const QString &line, const QString &next, int n) {
    auto &levels = state.levels;
    int &fence = state.fence, &fenceIndent = state.fenceIndent;
    QChar &fenceMarker = state.fenceMarker;
    bool &comment = state.comment, &table = state.table, &blank = state.blank;
    QString &html = state.html;
    QList<Item> result;
    static const QRegularExpression itemRe(QStringLiteral("^( *)([-+*]|[0-9]{1,9}[.)])( +)(.*)$"));
    static const QRegularExpression cellRe(QStringLiteral("^ *:?-{3,}:? *$"));
    static const QRegularExpression htmlRe(QStringLiteral("^</?(address|article|aside|base|basefont|blockquote|body|caption|center|col|colgroup|dd|details|dialog|dir|div|dl|dt|fieldset|figcaption|figure|footer|form|frame|frameset|h[1-6]|head|header|hr|html|iframe|legend|li|link|main|menu|menuitem|nav|noframes|ol|optgroup|option|p|param|pre|script|search|section|source|style|summary|table|tbody|td|th|thead|title|tr|track|ul|textarea)(?:\\s|>|/)"), QRegularExpression::CaseInsensitiveOption);
    const auto delimiter = [&](const QString &text) {
        if (!text.contains(QLatin1Char('|'))) return false;
        auto cells = text.trimmed().split(QLatin1Char('|'));
        if (cells.first().isEmpty()) cells.removeFirst();
        if (!cells.isEmpty() && cells.last().isEmpty()) cells.removeLast();
        if (cells.isEmpty()) return false;
        for (const auto &cell : cells) if (!cellRe.match(cell).hasMatch()) return false;
        return true;
    };
    const QString trimmed = line.trimmed();
    const auto quote = quotePrefix(line, state.quoteFence || state.quoteTicks || state.quoteComment
                                   || !state.quoteHtml.isEmpty() ? state.quoteDepth : 0);
    if (quote.second != state.quoteDepth) {
        state.quoteDepth = quote.second; state.quoteFence = state.quoteTicks = state.quoteBrackets = state.quoteDestination = 0;
        state.quoteLabel = state.quoteAngle = false; state.quoteTitle = {};
        state.quoteHtml.clear(); state.quoteComment = state.quoteTable = state.quoteList = false;
    }
    int leading = 0;
    while (leading < line.size() && line[leading] == QLatin1Char(' ')) ++leading;
    if (fence) {
        if (MarkdownHighlighter::closesFence(line.mid(qMin(leading, fenceIndent)), fence, fenceMarker)) { fence = 0; blank = true; }
        return result;
    }
    if (comment) { if (line.contains(QStringLiteral("-->"))) comment = false; return result; }
    if (trimmed.startsWith(QStringLiteral("<!--"))) {
        comment = !trimmed.contains(QStringLiteral("-->")); levels.clear(); return result;
    }
    if (!html.isEmpty()) {
        const bool raw = html == "pre" || html == "script" || html == "style" || html == "textarea";
        if ((raw && trimmed.contains("</" + html, Qt::CaseInsensitive)) || (!raw && trimmed.isEmpty())) { html.clear(); blank = true; }
        return result;
    }
    const auto tag = htmlRe.match(trimmed);
    static const QRegularExpression completeTag(QStringLiteral("^</?[A-Za-z][A-Za-z0-9-]*(?:\\s+[^<>]*)?\\s*/?>\\s*$"));
    if (!tag.hasMatch() && blank && completeTag.match(trimmed).hasMatch()) { html = "block"; levels.clear(); return result; }
    if (tag.hasMatch()) {
        html = tag.captured(1).toLower();
        const bool raw = html == "pre" || html == "script" || html == "style" || html == "textarea";
        if (trimmed.startsWith("</")) html = "block";
        else if (raw && trimmed.contains("</" + html, Qt::CaseInsensitive)) { html.clear(); blank = true; }
        levels.clear(); return result;
    }
    if (table) { if (trimmed.isEmpty() || !line.contains('|')) table = false; else return result; }
    if (line.contains('|') && delimiter(next)) { table = true; levels.clear(); return result; }
    if (quote.second) {
        levels.clear(); blank = false;
        const QString body = line.mid(quote.first), content = body.trimmed();
        if (state.quoteFence) {
            if (MarkdownHighlighter::closesFence(body, state.quoteFence, state.quoteFenceMarker)) state.quoteFence = 0;
            return result;
        }
        if (state.quoteComment) { if (body.contains("-->")) state.quoteComment = false; return result; }
        if (!state.quoteHtml.isEmpty()) {
            const auto tag = state.quoteHtml;
            const bool raw = tag == "pre" || tag == "script" || tag == "style" || tag == "textarea";
            if ((raw && content.contains("</" + tag, Qt::CaseInsensitive)) || (!raw && content.isEmpty())) state.quoteHtml.clear();
            return result;
        }
        if (body.contains("<!--")) { state.quoteComment = !body.contains("-->"); return result; }
        static const QRegularExpression incompleteTag(QStringLiteral("<[/!]?[A-Za-z][A-Za-z0-9-]*(?:\\s[^>]*)?$"));
        if (!state.quoteTicks && !state.quoteDestination && incompleteTag.match(body).hasMatch()) {
            state.quoteHtml = QStringLiteral("block"); return result;
        }
        const auto htmlTag = htmlRe.match(content);
        if (htmlTag.hasMatch() || completeTag.match(content).hasMatch()) {
            state.quoteHtml = htmlTag.hasMatch() ? htmlTag.captured(1).toLower() : QStringLiteral("block");
            if (content.contains("</" + state.quoteHtml, Qt::CaseInsensitive)) state.quoteHtml.clear();
            return result;
        }
        const int opening = MarkdownHighlighter::openingFenceLength(body);
        if (opening) { state.quoteFence = opening; state.quoteFenceMarker = content.front(); state.quoteTicks = 0; return result; }
        if (state.quoteTable) { if (content.isEmpty() || !body.contains('|')) state.quoteTable = false; else return result; }
        const auto nextQuote = quotePrefix(next);
        if (body.contains('|') && nextQuote.second == quote.second && delimiter(next.mid(nextQuote.first))) { state.quoteTable = true; return result; }
        static const QRegularExpression listStart(QStringLiteral("^(?:[-+*](?:\\s|$)|[0-9]{1,9}[.)]\\s)"));
        if (listStart.match(content).hasMatch()) state.quoteList = true;
        if (state.quoteList) { if (content.isEmpty()) state.quoteList = false; else return result; }
        static const QRegularExpression structural(QStringLiteral("^(?:#{1,6}(?:\\s|$)|\\[.*\\]:|=+\\s*$)"));
        if (body.startsWith("    ") || body.startsWith('\t') || structural.match(content).hasMatch()
            || MarkdownHighlighter::isThematicBreak(body)) { state.quoteTicks = 0; return result; }
        const bool beforeInline = state.quoteTicks || state.quoteBrackets || state.quoteDestination || state.quoteLabel;
        for (int i = 0; i < body.size(); ++i) {
            const auto c = body[i];
            if (c == '\\' && !state.quoteTicks) { state.quoteLabel = false; ++i; continue; }
            if (c == '`' && !state.quoteDestination) {
                int end = i + 1; while (end < body.size() && body[end] == '`') ++end;
                if (!state.quoteTicks) state.quoteTicks = end - i;
                else if (state.quoteTicks == end - i) state.quoteTicks = 0;
                i = end - 1; continue;
            }
            if (state.quoteTicks) continue;
            if (state.quoteDestination) {
                if (!state.quoteTitle.isNull()) { if (c == state.quoteTitle) state.quoteTitle = {}; continue; }
                if (state.quoteAngle) { if (c == '>') state.quoteAngle = false; continue; }
                if (c == '<') state.quoteAngle = true;
                else if ((c == '\'' || c == '"') && (!i || body[i - 1].isSpace())) state.quoteTitle = c;
                else if (c == '(') ++state.quoteDestination;
                else if (c == ')') --state.quoteDestination;
                continue;
            }
            if (state.quoteLabel && !c.isSpace()) {
                state.quoteLabel = false;
                if (c == '(') { state.quoteDestination = 1; continue; }
            }
            if (c == '[') ++state.quoteBrackets;
            else if (c == ']' && state.quoteBrackets && !--state.quoteBrackets) state.quoteLabel = true;
        }
        // Explicit hard-break spaces cannot continue a pending link label.
        if (body.endsWith("  ")) state.quoteLabel = false;
        if (content.isEmpty()) {
            state.quoteTicks = state.quoteBrackets = state.quoteDestination = 0;
            state.quoteLabel = state.quoteAngle = false; state.quoteTitle = {};
        }
        result.append({n, quote.first, quote.second, {}, true,
                       !content.isEmpty() && !beforeInline && !state.quoteTicks && !state.quoteBrackets
                       && !state.quoteDestination && !state.quoteLabel});
        return result;
    }
    if (trimmed.isEmpty()) { blank = true; return result; }
    int enclosing = int(levels.size()) - 1;
    while (enclosing >= 0 && leading < levels[enclosing].content) --enclosing;
    const int container = enclosing >= 0 ? levels[enclosing].content : 0;
    const auto candidate = line.mid(qMin(leading, container));
    fence = MarkdownHighlighter::openingFenceLength(candidate);
    if (fence) {
        while (levels.size() > enclosing + 1) levels.removeLast();
        fenceIndent = container; fenceMarker = candidate.trimmed().at(0); return result;
    }
    if (MarkdownHighlighter::isThematicBreak(line)) { levels.clear(); blank = true; return result; }
    if (line.contains('\t') || trimmed.startsWith('>')) { levels.clear(); blank = false; return result; }
    const auto match = itemRe.match(line);
    if (match.hasMatch()) {
        if (leading >= 4 && (levels.isEmpty() || leading >= levels.last().content + 4)) return result;
        while (levels.size() > 1 && leading < levels.last().indent) levels.removeLast();
        const bool ordered = match.captured(2).at(0).isDigit();
        const int number = ordered ? match.captured(2).chopped(1).toInt() : 0;
        const QChar delimiter = ordered ? match.captured(2).back() : QChar();
        if (levels.isEmpty() && !blank && ((ordered && number != 1) || match.captured(4).trimmed().isEmpty())) return result;
        if (levels.isEmpty() || leading >= levels.last().content)
            levels.append({levels.isEmpty() ? 0 : leading, int(match.capturedEnd(3)), number, ordered, delimiter});
        else if (levels.last().ordered != ordered || levels.last().delimiter != delimiter)
            levels.last() = {levels.last().indent, int(match.capturedEnd(3)), number, ordered, delimiter};
        else if (ordered) ++levels.last().number;
        levels.last().content = match.capturedEnd(3);
        const QString marker = ordered ? QString::number(levels.last().number) + delimiter
            : QString(QChar(levels.size() % 3 == 2 ? 0x25e6 : levels.size() % 3 == 0 ? 0x25aa : 0x2022));
        result.append({n, int(match.capturedEnd(3)), int(levels.size()), marker});
        blank = false;
    } else {
        if (blank) while (levels.size() > 1 && leading < levels.last().content) levels.removeLast();
        if (!levels.isEmpty() && !trimmed.startsWith('#')
            && leading < levels.last().content + 4 && (!blank || leading >= levels.last().content)) {
            result.append({n, leading, int(levels.size()), {}});
            blank = false;
        } else { levels.clear(); blank = trimmed.startsWith('#'); }
    }
    return result;
}
inline QList<Item> parse(const QString &text) {
    State state;
    QList<Item> items;
    const auto lines = text.split(QLatin1Char('\n'));
    for (int n = 0; n < lines.size(); ++n) items.append(step(state, lines[n], lines.value(n + 1), n));
    return items;
}
// Read/print-only preparation after source-offset object replacements. Never
// promote soft breaks: only explicit Markdown hard breaks need Qt's repair.
inline QString readHardBreaks(const QString &source, QStringList *markers = nullptr) {
    if (!markers) return source;
    markers->clear();
    auto lines = source.split('\n');
    // A disposable outer quote lets the same conservative prose classifier
    // handle ordinary paragraphs and actual quotes, including nested fences,
    // HTML, tables and multiline inline constructs. Prefixes gain two chars.
    QString framed = source;
    framed.replace(QLatin1Char('\n'), QStringLiteral("\n> "));
    framed.prepend(QStringLiteral("> "));
    const auto items = parse(framed);
    QString name;
    do { name = "PWHARDBREAK" + QUuid::createUuid().toString(QUuid::Id128); } while (source.contains(name));
    for (int i = 0; i + 1 < items.size(); ++i) {
        const auto &a = items[i]; const auto &b = items[i + 1];
        if (!a.quote || !a.soft || !b.quote || b.block != a.block + 1 || a.depth != b.depth) continue;
        // Only the next line's prefix is changed. Inline constructs may open
        // later on that line; requiring b.soft would miss reference links and
        // multiline code spans even though the sentinel is outside them.
        if (lines[b.block].mid(b.prefix - 2).trimmed().isEmpty()) continue;
        const auto &line = lines[a.block];
        int slashes = 0; for (int j = line.size() - 1; j >= 0 && line[j] == '\\'; --j) ++slashes;
        if (!line.endsWith("  ") && !(slashes % 2)) continue;
        const QString marker = name + QString::number(markers->size()) + "END";
        markers->append(marker);
        int prefix = b.prefix - 2;
        // Keep continuation indentation before the sentinel, where Markdown
        // strips it, rather than turning it into visible spaces after <br>.
        while (prefix < lines[b.block].size()
               && (lines[b.block][prefix] == ' ' || lines[b.block][prefix] == '\t')) ++prefix;
        lines[b.block].insert(prefix, marker + ' ');
    }
    return lines.join('\n');
}
// Qt's Markdown hard breaks can import as separate paragraphs. Exact disposable
// sentinels let us restore native line separators without guessing nth blocks,
// losing inline formatting, or adding a blank paragraph gap per source line.
inline bool restoreReadHardBreaks(QTextDocument &document, const QStringList &markers) {
    QList<QPair<int, int>> ranges;
    for (const auto &marker : markers) {
        const auto found = document.find(marker, 0, QTextDocument::FindCaseSensitively);
        if (found.isNull() || !document.find(marker, found.selectionEnd(), QTextDocument::FindCaseSensitively).isNull()) return false;
        const int first = found.selectionStart() - 1, last = found.selectionEnd();
        if (first < 0 || document.characterAt(last) != ' ') return false;
        const auto before = document.characterAt(first);
        if (before != ' ' && before != QChar::ParagraphSeparator && before != QChar::LineSeparator) return false;
        ranges.append({first, last + 1});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &range : ranges) {
        QTextCursor cursor(&document); cursor.setPosition(range.first);
        cursor.setPosition(range.second, QTextCursor::KeepAnchor); cursor.insertText(QString(QChar::LineSeparator));
    }
    return true;
}
class Parser {
    struct Row { QString text, next; State before, after; QList<Item> items; bool valid = false; };
    QList<Row> m_rows;
    int m_invalidLast = -1;
public:
    QList<Item> items;
    int changedFirst = 0, changedLast = -1, parsedLines = 0;
    void reset(const QTextDocument *document) {
        m_rows.clear(); items.clear(); m_rows.resize(document->blockCount()); m_invalidLast = document->blockCount() - 1; update(document, 0);
    }
    void splice(int first, int removedLines, int addedLines) {
        if (first < 0 || first + removedLines > m_rows.size()) { m_rows.clear(); m_invalidLast = -1; return; }
        m_invalidLast = qMax(first + addedLines - 1, m_invalidLast < first ? m_invalidLast : m_invalidLast + addedLines - removedLines);
        if (removedLines == addedLines) {
            for (int n = 0; n < addedLines; ++n) m_rows[first + n].valid = false;
        } else {
            m_rows.remove(first, removedLines);
            for (int n = 0; n < addedLines; ++n) m_rows.insert(first + n, Row());
            items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &item) {
                return item.block >= first && item.block < first + removedLines;
            }), items.end());
            for (auto &item : items) if (item.block >= first + removedLines) item.block += addedLines - removedLines;
        }
    }
    void update(const QTextDocument *document, int dirtyFirst) {
        if (m_rows.size() != document->blockCount()) { reset(document); return; }
        changedFirst = qMax(0, dirtyFirst - 1); changedLast = changedFirst - 1; parsedLines = 0;
        const int dirtyLast = qMax(dirtyFirst, m_invalidLast);
        State state = changedFirst ? m_rows[changedFirst - 1].after : State();
        for (QTextBlock block = document->findBlockByNumber(changedFirst); block.isValid(); block = block.next()) {
            const int n = block.blockNumber();
            auto &row = m_rows[n];
            const QString text = block.text(), next = block.next().text();
            if (n > dirtyLast && row.valid && row.text == text && row.next == next && row.before == state) break;
            row.text = text; row.next = next; row.before = state;
            row.items = step(state, text, next, 0); row.after = state; row.valid = true;
            ++parsedLines; changedLast = n;
        }
        m_invalidLast = -1;
        auto a = std::lower_bound(items.cbegin(), items.cend(), changedFirst, [](const Item &item, int n) { return item.block < n; });
        auto b = std::upper_bound(items.cbegin(), items.cend(), changedLast, [](int n, const Item &item) { return n < item.block; });
        const int at = a - items.cbegin(), count = b - a;
        QList<Item> changed;
        for (int n = changedFirst; n <= changedLast; ++n)
            for (Item item : m_rows[n].items) { item.block = n; changed.append(item); }
        if (count == changed.size()) for (int n = 0; n < changed.size(); ++n) items[at + n] = changed[n];
        else { items.remove(at, count); for (int n = 0; n < changed.size(); ++n) items.insert(at + n, changed[n]); }
    }
};
}
