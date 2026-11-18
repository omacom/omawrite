#include "spellingprose.h"

#include <QBitArray>
#include <QHash>
#include <QRegularExpression>
#include <QVector>

namespace {

bool escaped(const QString &text, int pos)
{
    int slashes = 0;
    while (pos > 0 && text.at(--pos) == QLatin1Char('\\'))
        ++slashes;
    return slashes % 2 != 0;
}

void exclude(QBitArray &hidden, int start, int end)
{
    for (int i = start; i < end; ++i)
        hidden.setBit(i);
}

bool horizontalSpace(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('\r');
}

// Classify indentation relative to quote/list containers. Lazy paragraph
// continuations remain prose. Tab expansion never changes source offsets.
class IndentedCode {
public:
    void blockEnded() { m_paragraph = false; }
    bool line(const QString &source, int start, int end) {
        QString text;
        int column = 0;
        for (int i = start; i < end; ++i) {
            const QChar c = source.at(i);
            if (c == QLatin1Char('\t')) {
                const int spaces = 4 - column % 4;
                text.append(QString(spaces, QLatin1Char(' ')));
                column += spaces;
            } else {
                text.append(c);
                ++column;
            }
        }
        int p = 0, depth = 0;
        for (;;) {
            int marker = p;
            while (marker < text.size() && marker - p < 3 && text.at(marker) == QLatin1Char(' '))
                ++marker;
            if (marker == text.size() || text.at(marker) != QLatin1Char('>'))
                break;
            ++depth;
            p = marker + 1;
            if (p < text.size() && text.at(p) == QLatin1Char(' '))
                ++p;
        }
        if (depth != m_quoteDepth) {
            m_quoteDepth = depth;
            m_lists.clear();
            m_paragraph = false;
        }
        const int prefix = p;
        while (p < text.size() && text.at(p) == QLatin1Char(' '))
            ++p;
        if (p == text.size()) {
            m_paragraph = false;
            return false;
        }
        const int indent = p - prefix;
        while (!m_lists.isEmpty() && indent < m_lists.last())
            m_lists.removeLast();
        const int base = m_lists.isEmpty() ? 0 : m_lists.last();
        if (indent - base >= 4 && !m_paragraph)
            return true;
        static const QRegularExpression thematic(QStringLiteral(
            R"(^(?:\*(?: *\*){2,}|-(?: *-){2,}|_(?: *_){2,}) *$)"));
        if (thematic.match(text.mid(p)).hasMatch()) {
            m_paragraph = false;
            return false;
        }
        static const QRegularExpression marker(QStringLiteral(R"(^(?:[-+*]|[0-9]{1,9}[.)])(?= ))"));
        const auto item = marker.match(text.mid(p));
        if (item.hasMatch()) {
            const int after = p + item.capturedLength();
            int content = after;
            while (content < text.size() && text.at(content) == QLatin1Char(' '))
                ++content;
            const int spaces = content - after;
            const bool code = spaces >= 5;
            m_lists.append(indent + item.capturedLength() + (code ? 1 : spaces));
            m_paragraph = !code;
            return code;
        }
        static const QRegularExpression heading(QStringLiteral(R"(^#{1,6}(?: |$))"));
        m_paragraph = !heading.match(text.mid(p)).hasMatch();
        return false;
    }
private:
    QList<int> m_lists;
    int m_quoteDepth = 0;
    bool m_paragraph = false;
};

// Peel nested blockquote/list containers, including a list's continuation indent.
// Accepting excessive indentation deliberately errs towards hiding code.
int contentStart(const QString &text, int start, int end)
{
    int p = start;
    while (p < end) {
        while (p < end && horizontalSpace(text.at(p)))
            ++p;
        if (p == end)
            break;
        if (text.at(p) == QLatin1Char('>')) {
            ++p;
            continue;
        }
        int markerEnd = p;
        const QChar c = text.at(p);
        if (c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('*')) {
            markerEnd = p + 1;
        } else {
            while (markerEnd < end && text.at(markerEnd) >= QLatin1Char('0')
                   && text.at(markerEnd) <= QLatin1Char('9'))
                ++markerEnd;
            if (markerEnd > p && markerEnd < end
                && (text.at(markerEnd) == QLatin1Char('.')
                    || text.at(markerEnd) == QLatin1Char(')')))
                ++markerEnd;
            else
                markerEnd = p;
        }
        if (markerEnd > p && markerEnd < end && horizontalSpace(text.at(markerEnd))) {
            p = markerEnd;
            continue;
        }
        break;
    }
    return p;
}

// Inside a fence, list markers are literal code, not new containers. Only
// remove the quote prefixes captured at the opener and continuation whitespace.
int fenceContentStart(const QString &text, int start, int end, int quoteDepth)
{
    int p = start;
    int quotes = 0;
    while (p < end) {
        while (p < end && horizontalSpace(text.at(p)))
            ++p;
        if (p < end && quotes < quoteDepth && text.at(p) == QLatin1Char('>')) {
            ++p;
            ++quotes;
        } else {
            break;
        }
    }
    return quotes == quoteDepth ? p : end;
}

QVector<int> bracketEnds(const QString &text, const QBitArray &hidden)
{
    QVector<int> ends(text.size(), -1);
    QVector<int> openings;
    for (int p = 0; p < text.size(); ++p) {
        if (hidden.testBit(p))
            continue;
        if (text.at(p) == QLatin1Char('\\')) {
            ++p;
            continue;
        }
        if (text.at(p) == QLatin1Char('[')) {
            openings.append(p);
        } else if (text.at(p) == QLatin1Char(']') && !openings.isEmpty()) {
            ends[openings.takeLast()] = p + 1;
        } else if (text.at(p) == QLatin1Char('\n') && p + 1 < text.size()
                   && text.at(p + 1) == QLatin1Char('\n')) {
            openings.clear();
        }
    }
    return ends;
}

// Includes the destination and optional title. Malformed destinations are hidden
// to the end of the paragraph rather than spell-checking fragments of a path.
int destinationEnd(const QString &text, int start)
{
    int depth = 1;
    QChar quote;
    bool angle = false;
    for (int p = start + 1; p < text.size(); ++p) {
        const QChar c = text.at(p);
        if (c == QLatin1Char('\\')) {
            ++p;
            continue;
        }
        if (c == QLatin1Char('\n')) {
            int next = p + 1;
            while (next < text.size() && horizontalSpace(text.at(next)))
                ++next;
            if (next == text.size() || text.at(next) == QLatin1Char('\n'))
                return p;
        }
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
        } else if (angle) {
            if (c == QLatin1Char('>'))
                angle = false;
        } else if ((c == QLatin1Char('"') || c == QLatin1Char('\''))
                   && text.at(p - 1).isSpace()) {
            quote = c;
        } else if (c == QLatin1Char('<')) {
            angle = true;
        } else if (c == QLatin1Char('(')) {
            ++depth;
        } else if (c == QLatin1Char(')') && --depth == 0) {
            return p + 1;
        }
    }
    return int(text.size());
}

uint scalarAt(const QString &text, int pos, int &width)
{
    const QChar c = text.at(pos);
    width = 1;
    if (c.isHighSurrogate() && pos + 1 < text.size() && text.at(pos + 1).isLowSurrogate()) {
        width = 2;
        return QChar::surrogateToUcs4(c, text.at(pos + 1));
    }
    return c.unicode();
}

bool mark(uint c)
{
    const auto category = QChar::category(c);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

} // namespace

QList<SpellingProse::Word> SpellingProse::words(const QString &source)
{
    const int size = int(source.size());
    QBitArray hidden(size);
    QChar fence;
    int fenceLength = 0;
    int fenceQuotes = 0;
    int fencePrefixWidth = 0;
    bool referenceContinuation = false;
    bool referenceNeedsDestination = false;
    IndentedCode indented;
    static const QRegularExpression reference(QStringLiteral(R"(^\[(?:\\.|[^\]\\])+\]:)"));

    // Block constructs first: no inline construct can escape a fenced block.
    for (int start = 0; start < size;) {
        int end = int(source.indexOf(QLatin1Char('\n'), start));
        if (end < 0)
            end = size;
        const int next = end < size ? end + 1 : end;
        const int p = fence.isNull() ? contentStart(source, start, end)
                                    : fenceContentStart(source, start, end, fenceQuotes);
        const bool blank = p == end;
        int prefixWidth = 0;
        for (int i = start; i < p; ++i)
            prefixWidth += source.at(i) == QLatin1Char('\t') ? 4 : 1;
        int runEnd = p;
        if (p < end && (source.at(p) == QLatin1Char('`') || source.at(p) == QLatin1Char('~'))) {
            while (runEnd < end && source.at(runEnd) == source.at(p))
                ++runEnd;
        }
        if (!fence.isNull()) {
            indented.blockEnded();
            exclude(hidden, start, next);
            if (p < end && source.at(p) == fence && runEnd - p >= fenceLength
                && prefixWidth <= fencePrefixWidth + 3
                && source.mid(runEnd, end - runEnd).trimmed().isEmpty())
                fence = QChar();
        } else if (indented.line(source, start, end)) {
            exclude(hidden, start, next);
            referenceContinuation = false;
            referenceNeedsDestination = false;
        } else if (runEnd - p >= 3
                   && (source.at(p) != QLatin1Char('`')
                       || !source.mid(runEnd, end - runEnd).contains(QLatin1Char('`')))) {
            fence = source.at(p);
            fenceLength = runEnd - p;
            fenceQuotes = source.mid(start, p - start).count(QLatin1Char('>'));
            fencePrefixWidth = prefixWidth;
            indented.blockEnded();
            exclude(hidden, start, next);
            referenceContinuation = false;
        } else {
            const auto definition = reference.match(source.mid(p, end - p));
            if (definition.hasMatch()) {
                exclude(hidden, start, next);
                referenceContinuation = true;
                referenceNeedsDestination = source.mid(p + definition.capturedLength(),
                                                       end - p - definition.capturedLength()).trimmed().isEmpty();
            } else if (referenceContinuation && !blank
                       && (referenceNeedsDestination
                           || (start < end && horizontalSpace(source.at(start)))
                           || (p < end && (source.at(p) == QLatin1Char('"')
                                          || source.at(p) == QLatin1Char('\'')
                                          || source.at(p) == QLatin1Char('('))))) {
                exclude(hidden, start, next);
                referenceNeedsDestination = false;
            } else {
                referenceContinuation = false;
                referenceNeedsDestination = false;
            }
        }
        start = next;
    }

    // Index exact backtick runs in each unmasked paragraph. Closing runs may be
    // preceded by a backslash (backslashes are literal inside a code span).
    struct Run { int start; int length; int paragraph; int next = -1; };
    QVector<Run> runs;
    int paragraph = 0;
    bool lineHasContent = false;
    for (int p = 0; p < size;) {
        if (hidden.testBit(p)) {
            ++paragraph;
            lineHasContent = false;
            ++p;
        } else if (source.at(p) == QLatin1Char('\n')) {
            if (!lineHasContent)
                ++paragraph;
            lineHasContent = false;
            ++p;
        } else if (source.at(p) == QLatin1Char('`')) {
            const int start = p++;
            while (p < size && !hidden.testBit(p) && source.at(p) == QLatin1Char('`'))
                ++p;
            runs.append({start, p - start, paragraph, -1});
            lineHasContent = true;
        } else {
            if (!source.at(p).isSpace())
                lineHasContent = true;
            ++p;
        }
    }
    QHash<int, int> nextRun;
    for (int r = int(runs.size()) - 1; r >= 0; --r) {
        Run &run = runs[r];
        if (r + 1 < runs.size() && run.paragraph != runs.at(r + 1).paragraph)
            nextRun.clear();
        // An escaped first tick leaves the rest of its run available to open
        // a span. Inside a span, however, the entire raw run must match.
        const int openingLength = run.length - (escaped(source, run.start) ? 1 : 0);
        if (openingLength > 0)
            run.next = nextRun.value(openingLength, -1);
        nextRun.insert(run.length, r);
    }
    for (int r = 0; r < runs.size(); ++r) {
        const Run &run = runs.at(r);
        if (run.next < 0)
            continue;
        const Run &close = runs.at(run.next);
        const int start = run.start + (escaped(source, run.start) ? 1 : 0);
        exclude(hidden, start, close.start + close.length);
        r = run.next;
    }

    // Tags, autolinks and entities. Attribute quotes may contain '>'. Comments
    // are hidden through their terminator, or EOF when unfinished.
    static const QRegularExpression autolink(QStringLiteral(
        R"(\A(?:[A-Za-z][A-Za-z0-9+.-]*:[^\s<>]*|[^\s<>@]+@[^\s<>@]+)>)"));
    static const QRegularExpression angleStart(QStringLiteral(
        R"(\A(?:/?[A-Za-z][A-Za-z0-9:-]*(?=[\s/>])|[!?]))"));
    for (int p = 0; p < size; ++p) {
        if (hidden.testBit(p) || source.at(p) != QLatin1Char('<') || escaped(source, p))
            continue;
        if (source.mid(p, 4) == QStringLiteral("<!--")) {
            const int close = int(source.indexOf(QStringLiteral("-->"), p + 4));
            const int end = close < 0 ? size : close + 3;
            exclude(hidden, p, end);
            p = end - 1;
            continue;
        }
        const auto link = autolink.matchView(QStringView(source).mid(p + 1));
        if (link.hasMatch()) {
            const int end = p + 1 + int(link.capturedLength());
            exclude(hidden, p, end);
            p = end - 1;
            continue;
        }
        if (!angleStart.matchView(QStringView(source).mid(p + 1)).hasMatch())
            continue;
        QChar quote;
        int end = p + 1;
        for (; end < size; ++end) {
            const QChar c = source.at(end);
            if (!quote.isNull()) {
                if (c == quote)
                    quote = QChar();
            } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
            } else if (c == QLatin1Char('>')) {
                ++end;
                break;
            }
        }
        exclude(hidden, p, end);
        p = end - 1;
    }
    static const QRegularExpression entity(QStringLiteral(R"(&(?:[A-Za-z][A-Za-z0-9]*|\#[0-9]+|\#[xX][0-9A-Fa-f]+);)"));
    auto entities = entity.globalMatch(source);
    while (entities.hasNext()) {
        const auto match = entities.next();
        if (!escaped(source, int(match.capturedStart())))
            exclude(hidden, int(match.capturedStart()), int(match.capturedEnd()));
    }

    // Match nested brackets once, including failed openings, in O(n) time and
    // space. Later exclusions hide whole labels or their suffixes, so cannot
    // change the pairs of any remaining visible opening.
    const QVector<int> labelEnds = bracketEnds(source, hidden);

    // Keep visible link labels, but never image alt text or reference IDs.
    for (int p = 0; p < size; ++p) {
        if (hidden.testBit(p))
            continue;
        const bool image = source.at(p) == QLatin1Char('!') && p + 1 < size
            && source.at(p + 1) == QLatin1Char('[');
        if ((!image && source.at(p) != QLatin1Char('[')) || escaped(source, p))
            continue;
        const int open = image ? p + 1 : p;
        const int labelEnd = labelEnds.at(open);
        if (labelEnd < 0)
            continue;
        int suffix = labelEnd;
        while (suffix < size && horizontalSpace(source.at(suffix)))
            ++suffix;
        int end = labelEnd;
        if (suffix < size && source.at(suffix) == QLatin1Char('(')) {
            end = destinationEnd(source, suffix);
            exclude(hidden, labelEnd, end);
        } else {
            // Reference labels may be separated by one line ending.
            if (suffix < size && source.at(suffix) == QLatin1Char('\n')) {
                ++suffix;
                while (suffix < size && horizontalSpace(source.at(suffix)))
                    ++suffix;
            }
            if (suffix < size && source.at(suffix) == QLatin1Char('[')) {
                const int referenceEnd = labelEnds.at(suffix);
                if (referenceEnd >= 0) {
                    end = referenceEnd;
                    exclude(hidden, labelEnd, end);
                }
            }
        }
        if (image)
            exclude(hidden, p, end);
    }

    static const QRegularExpression url(QStringLiteral(
        R"(\b(?:[a-z][a-z0-9+.-]*://|www\.|mailto:|data:|tel:|urn:)[^\s<>"`]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto urls = url.globalMatch(source);
    while (urls.hasNext()) {
        const auto match = urls.next();
        exclude(hidden, int(match.capturedStart()), int(match.capturedEnd()));
    }

    QList<Word> result;
    for (int p = 0; p < size;) {
        int width;
        uint c = scalarAt(source, p, width);
        if (hidden.testBit(p) || (!QChar::isLetter(c) && !QChar::isNumber(c))) {
            p += width;
            continue;
        }
        const int start = p;
        bool hasLetter = false;
        while (p < size && !hidden.testBit(p)) {
            c = scalarAt(source, p, width);
            if (QChar::isLetter(c) || QChar::isNumber(c) || mark(c)) {
                hasLetter = hasLetter || QChar::isLetter(c);
                p += width;
            } else if ((c == '\'' || c == 0x2019) && p + width < size
                       && !hidden.testBit(p + width)) {
                int nextWidth;
                const uint next = scalarAt(source, p + width, nextWidth);
                if (!QChar::isLetter(next))
                    break;
                p += width;
            } else {
                break;
            }
        }
        if (hasLetter)
            result.append({start, p - start, source.mid(start, p - start)});
    }
    return result;
}
