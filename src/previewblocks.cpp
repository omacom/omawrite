#include "previewblocks.h"
#include "markdownhighlighter.h"
#include "attachmenthelper.h"

#include <QAbstractTextDocumentLayout>
#include <QFile>
#include <QFontMetricsF>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QTextDocument>
#include <QTextOption>
#include <cmath>
#include <algorithm>

namespace {

bool allowedIndent(const QString &text) {
    int spaces = 0;
    while (spaces < text.size() && text.at(spaces) == QLatin1Char(' '))
        ++spaces;
    return spaces <= 3 && (spaces == text.size() || text.at(spaces) != QLatin1Char('\t'));
}

// Split only on real pipes, not escaped pipes or pipes in matched code spans.
QStringList cells(const QString &line) {
    if (!allowedIndent(line))
        return {};
    // These start other Markdown blocks rather than table rows.
    static const QRegularExpression otherBlock(
        QStringLiteral("^(?:>|#{1,6}(?:\\s|$)|[-+*]\\s|[0-9]{1,9}[.)]\\s)"));
    if (otherBlock.match(line.trimmed()).hasMatch())
        return {};
    const auto spans = MarkdownHighlighter::codeSpans(line);
    QList<int> pipes;
    int span = 0;
    for (int i = 0; i < line.size(); ++i) {
        while (span < spans.size() && i >= spans.at(span).start + spans.at(span).length)
            ++span;
        if (span < spans.size() && i >= spans.at(span).start)
            continue;
        if (line.at(i) == QLatin1Char('\\')) {
            ++i;
        } else if (line.at(i) == QLatin1Char('|')) {
            pipes.append(i);
        }
    }
    if (pipes.isEmpty())
        return {};
    QStringList result;
    int start = 0;
    for (int pipe : pipes) {
        result.append(line.mid(start, pipe - start).trimmed());
        start = pipe + 1;
    }
    result.append(line.mid(start).trimmed());
    if (line.left(pipes.first()).trimmed().isEmpty())
        result.removeFirst();
    if (line.mid(pipes.last() + 1).trimmed().isEmpty() && !result.isEmpty())
        result.removeLast();
    return result;
}

bool delimiter(const QStringList &row) {
    static const QRegularExpression re(QStringLiteral("^:?-{3,}:?$"));
    if (row.isEmpty())
        return false;
    for (const QString &cell : row) {
        if (!re.match(cell).hasMatch())
            return false;
    }
    return true;
}

bool punctuation(QChar c) {
    const ushort u = c.unicode();
    return (u >= 33 && u <= 47) || (u >= 58 && u <= 64)
        || (u >= 91 && u <= 96) || (u >= 123 && u <= 126);
}

QString unescape(const QString &text) {
    QString result;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()
            && punctuation(text.at(i + 1)))
            ++i;
        result += text.at(i);
    }
    return result;
}

bool standaloneImage(const QString &line, QString *path) {
    if (!allowedIndent(line))
        return false;
    const QString text = line.trimmed();
    if (!text.startsWith(QStringLiteral("![")))
        return false;
    int i = 2;
    int brackets = 1;
    for (; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('\\')) {
            ++i;
        } else if (text.at(i) == QLatin1Char('[')) {
            ++brackets;
        } else if (text.at(i) == QLatin1Char(']') && --brackets == 0) {
            break;
        }
    }
    if (i + 1 >= text.size() || text.at(++i) != QLatin1Char('('))
        return false;
    ++i;
    while (i < text.size() && text.at(i).isSpace())
        ++i;
    const bool angled = i < text.size() && text.at(i) == QLatin1Char('<');
    if (angled)
        ++i;
    const int start = i;
    int depth = 0;
    for (; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\') && i + 1 < text.size()
            && punctuation(text.at(i + 1))) {
            ++i;
            continue;
        }
        if (angled) {
            if (c == QLatin1Char('>'))
                break;
            if (c == QLatin1Char('<'))
                return false;
        } else {
            if (c.isSpace() || (c == QLatin1Char(')') && depth == 0))
                break;
            if (c == QLatin1Char('('))
                ++depth;
            else if (c == QLatin1Char(')'))
                --depth;
        }
    }
    if (depth != 0 || i == start || i >= text.size())
        return false;
    const QString destination = text.mid(start, i - start);
    if (angled)
        ++i;
    const int beforeSpace = i;
    while (i < text.size() && text.at(i).isSpace())
        ++i;
    if (i < text.size() && text.at(i) != QLatin1Char(')')) {
        if (i == beforeSpace)
            return false;
        const QChar quote = text.at(i++);
        if (quote != QLatin1Char('"') && quote != QLatin1Char('\'') && quote != QLatin1Char('('))
            return false;
        const QChar closing = quote == QLatin1Char('(') ? QLatin1Char(')') : quote;
        while (i < text.size() && text.at(i) != closing) {
            if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()
                && punctuation(text.at(i + 1)))
                ++i;
            ++i;
        }
        if (i >= text.size())
            return false;
        ++i;
        while (i < text.size() && text.at(i).isSpace())
            ++i;
    }
    if (i != text.size() - 1 || text.at(i) != QLatin1Char(')'))
        return false;
    *path = unescape(destination);
    return true;
}

// Rendering a table must not implicitly load inline image resources.
class TableDocument : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

constexpr qint64 MaxPixels = Attachments::MaxImagePixels;
constexpr int MaxHeight = 1800;

} // namespace

namespace PreviewBlocks {

void Parser::reset(const QString &source) {
    m_lines.clear(); m_records.clear(); m_length = source.size(); m_parsedLines = 0;
    for (int start = 0; start < source.size();) {
        int end = source.indexOf(QLatin1Char('\n'), start);
        if (end < 0)
            end = int(source.size());
        int contentEnd = end;
        if (contentEnd > start && source.at(contentEnd - 1) == QLatin1Char('\r'))
            --contentEnd;
        m_lines.append({start, contentEnd, source.mid(start, contentEnd - start)});
        start = end + 1;
    }
    m_offsets.fill(0, m_lines.size() + 1); m_constructs.clear();
    State state;
    for (int line = 0; line < m_lines.size();) {
        auto record = scan(line, state, source);
        if (!record.blocks.isEmpty() || !record.fences.isEmpty()) m_constructs.append(m_records.size());
        m_records.append(record);
    }
    collect();
}
int Parser::offset(int line) const {
    int value = 0;
    for (int i = line + 1; i > 0; i -= i & -i) value += m_offsets[i];
    return value;
}
void Parser::shiftSuffix(int line, int delta) {
    for (int i = line + 1; i < m_offsets.size(); i += i & -i) m_offsets[i] += delta;
}
void Parser::collect() {
    m_blocks.clear(); m_fences.clear();
    for (int index : m_constructs) {
        const auto &record = m_records[index];
        m_blocks.append(record.blocks); m_fences.append(record.fences);
    }
}

void Parser::edit(const QString &source, int position, int removed, int added) {
    if (m_lines.isEmpty() || position < 0 || removed < 0 || added < 0
        || position + removed > m_length || source.size() != m_length - removed + added) {
        reset(source); return;
    }
    const auto lineAt = [this](int p) {
        int low = 0, high = m_lines.size();
        while (low < high) { const int mid = (low + high) / 2; if (lineStart(mid) <= p) low = mid + 1; else high = mid; }
        return qMax(0, low - 1);
    };
    const int first = lineAt(position), last = lineAt(position + removed);
    auto recordIt = std::upper_bound(m_records.cbegin(), m_records.cend(), first,
        [](int line, const Record &record) { return line < record.first; });
    int recordAt = qMax(0, int(recordIt - m_records.cbegin()) - 2);
    const int restart = m_records[recordAt].first;
    const State initial = m_records[recordAt].before;
    const int delta = added - removed;
    const int spanStart = lineStart(first);
    const int spanEnd = last + 1 < m_lines.size() ? lineStart(last + 1) : m_length;
    QList<Line> replacement;
    const int newEnd = spanEnd + delta;
    for (int start = spanStart; start < newEnd;) {
        int end = source.indexOf(QLatin1Char('\n'), start);
        if (end < 0 || end > newEnd) end = newEnd;
        int contentEnd = end;
        if (contentEnd > start && source[contentEnd - 1] == QLatin1Char('\r')) --contentEnd;
        replacement.append({start, contentEnd, source.mid(start, contentEnd - start)});
        start = end + 1;
    }
    const int lineDelta = int(replacement.size()) - (last - first + 1);
    if (lineDelta) {
        // Line-number changes rebase the index once; ordinary characters use
        // logarithmic suffix offsets and retain every unchanged row in place.
        for (int n = 0; n < m_lines.size(); ++n) { const int d = offset(n); m_lines[n].start += d; m_lines[n].end += d; }
        m_lines.remove(first, last - first + 1);
        for (int n = 0; n < replacement.size(); ++n) m_lines.insert(first + n, replacement[n]);
        m_offsets.fill(0, m_lines.size() + 1);
    } else {
        for (int n = 0; n < replacement.size(); ++n) {
            auto row = replacement[n]; const int d = offset(first + n); row.start -= d; row.end -= d; m_lines[first + n] = row;
        }
    }
    shiftSuffix(first + replacement.size(), delta);
    // A non-structural edit inside a fence cannot alter its boundaries or
    // the following parser context. Do not rescan thousands of code lines.
    int containing = recordAt;
    while (containing + 1 < m_records.size() && m_records[containing + 1].first <= first) ++containing;
    const auto &owner = m_records[containing];
    if (last == first && replacement.size() == 1 && !owner.fences.isEmpty()
        && first > owner.first && first < owner.last) {
        const QString opening = m_lines[owner.first].text;
        if (!MarkdownHighlighter::closesFence(m_lines[first].text,
                MarkdownHighlighter::openingFenceLength(opening), opening.trimmed().at(0))) {
            for (int index : m_constructs) {
                auto &record = m_records[index];
                for (auto &block : record.blocks) {
                    if (block.start > position) { block.start += delta; block.end += delta; }
                    else if (block.end >= position) {
                        block.end += delta; block.markdown = source.mid(block.start, block.end - block.start);
                    }
                }
                for (auto &fence : record.fences) {
                    if (fence.first > position) { fence.first += delta; fence.second += delta; }
                    else if (fence.second >= position) fence.second += delta;
                }
            }
            m_length = source.size(); m_parsedLines = 1; collect(); return;
        }
    }
    int resume = int(std::upper_bound(m_records.cbegin(), m_records.cend(), last,
        [](int line, const Record &record) { return line < record.first; }) - m_records.cbegin());
    QList<Record> changed;
    State state = initial; m_parsedLines = 0;
    bool converged = false;
    static const QRegularExpression precedingSensitiveRule(QStringLiteral("^ {0,3}(?:-[ \\t]*){3,}$"));
    for (int line = restart; line < m_lines.size();) {
        while (resume < m_records.size() && m_records[resume].first + lineDelta < line) ++resume;
        // Rule/Setext classification also reads the previous physical line,
        // which is not represented by State. Revisit this one cheap successor
        // while still converging before an unchanged, potentially huge table.
        if (line >= first + replacement.size() && resume < m_records.size()
            && (line > first + replacement.size() || !precedingSensitiveRule.match(m_lines[line].text).hasMatch())
            && m_records[resume].first + lineDelta == line && m_records[resume].before == state) { converged = true; break; }
        changed.append(scan(line, state, source));
    }
    if (!converged) resume = m_records.size();
    for (int index : m_constructs) if (index >= resume) {
        auto &record = m_records[index];
        for (auto &block : record.blocks) { block.start += delta; block.end += delta; }
        for (auto &fence : record.fences) { fence.first += delta; fence.second += delta; }
    }
    if (lineDelta) for (int n = resume; n < m_records.size(); ++n) { m_records[n].first += lineDelta; m_records[n].last += lineDelta; }
    const int replaced = resume - recordAt, recordDelta = int(changed.size()) - replaced;
    QList<int> constructs;
    for (int index : m_constructs) if (index < recordAt) constructs.append(index);
    for (int n = 0; n < changed.size(); ++n) if (!changed[n].blocks.isEmpty() || !changed[n].fences.isEmpty()) constructs.append(recordAt + n);
    for (int index : m_constructs) if (index >= resume) constructs.append(index + recordDelta);
    m_constructs = constructs;
    if (!recordDelta) for (int n = 0; n < changed.size(); ++n) m_records[recordAt + n] = changed[n];
    else {
        m_records.remove(recordAt, replaced);
        for (int n = 0; n < changed.size(); ++n) m_records.insert(recordAt + n, changed[n]);
    }
    m_length = source.size(); collect();
}

Parser::Record Parser::scan(int &i, State &state, const QString &source) {
    const auto &lines = m_lines;
    Record record;
    record.first = i; record.before = state;
    const auto finish = [&] {
        record.last = i - 1; record.after = state;
        m_parsedLines += i - record.first;
        return record;
    };
    auto append = [&](int first, int last, Kind kind, const QString &path = QString()) {
        const int start = lineStart(first);
        const int end = lineEnd(last);
        record.blocks.append({start, end, kind, source.mid(start, end - start), path});
    };
    bool &htmlComment = state.comment;
    QString &htmlTag = state.html;
    {
        const QString &text = lines.at(i).text;
        // Do not project Markdown-looking lines inside literal HTML/comments.
        static const QRegularExpression htmlStart(QStringLiteral("^ {0,3}<([A-Za-z][A-Za-z0-9-]*)(?:[ >\\t/])"));
        const auto tag = htmlStart.match(text);
        if (htmlComment) {
            htmlComment = !text.contains(QStringLiteral("-->"));
            ++i;
            return finish();
        }
        if (!htmlTag.isEmpty()) {
            const bool literal = htmlTag == "pre" || htmlTag == "script" || htmlTag == "style" || htmlTag == "textarea";
            if ((!literal && text.trimmed().isEmpty())
                || text.contains(QStringLiteral("</") + htmlTag + QLatin1Char('>'), Qt::CaseInsensitive))
                htmlTag.clear();
            ++i;
            return finish();
        }
        if (text.trimmed().startsWith(QStringLiteral("<!--"))) {
            htmlComment = !text.contains(QStringLiteral("-->"));
            ++i;
            return finish();
        }
        if (tag.hasMatch()) {
            htmlTag = tag.captured(1).toLower();
            static const QStringList voidTags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
            if (text.contains(QStringLiteral("</") + htmlTag + QLatin1Char('>'), Qt::CaseInsensitive)
                || text.trimmed().endsWith(QStringLiteral("/>")) || voidTags.contains(htmlTag))
                htmlTag.clear();
            ++i;
            return finish();
        }
        const int fenceLength = MarkdownHighlighter::openingFenceLength(text);
        if (fenceLength) {
            const QString trimmed = text.trimmed();
            const QChar marker = trimmed.at(0);
            const bool mermaid = trimmed.mid(fenceLength).trimmed() == QStringLiteral("mermaid");
            int last = i + 1;
            while (last < lines.size()
                   && !MarkdownHighlighter::closesFence(lines.at(last).text, fenceLength, marker))
                ++last;
            if (last == lines.size())
                --last;
            if (mermaid)
                append(i, last, Kind::Mermaid);
            record.fences.append({lineStart(i), lineEnd(last)});
            i = last + 1;
            return finish();
        }
        // A blank/start boundary avoids turning Setext heading underlines into
        // rules. Other ambiguous container syntax stays editable Markdown.
        static const QRegularExpression horizontalRule(QStringLiteral("^ {0,3}(?:-[ \\t]*){3,}$"));
        if (horizontalRule.match(text).hasMatch()
            && (i == 0 || lines.at(i - 1).text.trimmed().isEmpty()
                || horizontalRule.match(lines.at(i - 1).text).hasMatch())) {
            append(i, i, Kind::HorizontalRule);
            ++i;
            return finish();
        }
        QString path;
        if (standaloneImage(text, &path)) {
            append(i, i, Kind::Image, path);
            ++i;
            return finish();
        }
        const QStringList header = cells(text);
        if (!header.isEmpty() && i + 1 < lines.size()) {
            const QStringList rule = cells(lines.at(i + 1).text);
            if (rule.size() == header.size() && delimiter(rule)) {
                int last = i + 1;
                while (last + 1 < lines.size()
                       && !MarkdownHighlighter::openingFenceLength(lines.at(last + 1).text)
                       && cells(lines.at(last + 1).text).size() == header.size())
                    ++last;
                append(i, last, Kind::Table);
                i = last + 1;
                return finish();
            }
        }
        ++i;
    }
    return finish();
}

QList<Block> parse(const QString &source) {
    Parser parser;
    parser.reset(source);
    return parser.blocks();
}

QImage renderHorizontalRule(const QFont &font, const QColor &foreground, int maxWidth) {
    if (maxWidth <= 0)
        return {};
    QImage image(qMin(maxWidth, 8192), qMax(12, int(std::ceil(QFontMetricsF(font).height()))),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    QColor color = foreground;
    color.setAlphaF(color.alphaF() * 0.5);
    painter.setPen(color);
    painter.drawLine(0, image.height() / 2, image.width() - 1, image.height() / 2);
    return image;
}

QImage renderTable(const QString &markdown, const QFont &font,
                   const QColor &foreground, const QColor &background, int maxWidth) {
    if (maxWidth <= 0 || markdown.isEmpty())
        return {};
    TableDocument document;
    document.setDefaultFont(font);
    document.setDefaultStyleSheet(QStringLiteral("body, table, td, th, p { color: %1; }")
                                     .arg(foreground.name(QColor::HexRgb)));
    document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document.setDefaultTextOption(option);
    const int width = qMin(maxWidth, 8192);
    document.setTextWidth(width);
    const QSizeF size = document.documentLayout()->documentSize();
    if (!std::isfinite(size.height()) || !std::isfinite(size.width())
        || size.height() <= 0 || size.width() <= 0)
        return {};
    const qreal scale = qMin(qreal(1), width / size.width());
    const qreal scaledHeight = std::ceil(size.height() * scale);
    if (scaledHeight > MaxPixels / width)
        return {};
    const int height = qMax(1, int(scaledHeight));
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.fill(background);
    QPainter painter(&image);
    painter.scale(scale, scale);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(QPalette::Text, foreground);
    context.palette.setColor(QPalette::WindowText, foreground);
    context.palette.setColor(QPalette::Base, background);
    context.clip = QRectF(0, 0, width / scale, height / scale);
    document.documentLayout()->draw(&painter, context);
    return image;
}

QImage loadImage(const QUrl &url, int maxWidth) {
    if (maxWidth <= 0) return {};
    QImage image = Attachments::loadLocalImage(url);
    if (image.isNull()) return {};
    if (image.width() > maxWidth || image.height() > MaxHeight)
        image = image.scaled(QSize(maxWidth, MaxHeight), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace PreviewBlocks
