#include "readcodehighlighting.h"
#include "codehighlighter.h"
#include "markdownhighlighter.h"
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace {
struct Fence {
    QString info;
    QStringList lines;
    bool closed = false;
};
int quotePrefix(QString &line, int maximum = 100) {
    static const QRegularExpression quote(QStringLiteral("^ {0,3}> ?"));
    int count = 0;
    while (count < maximum) {
        const auto match = quote.match(line);
        if (!match.hasMatch()) break;
        line.remove(0, match.capturedLength());
        ++count;
    }
    return count;
}
QString unindent(QString line, int indent) {
    int spaces = 0;
    while (spaces < line.size() && spaces < indent && line.at(spaces) == QLatin1Char(' ')) ++spaces;
    return line.mid(spaces);
}
QString languageToken(const QString &info) {
    return info.simplified().section(QLatin1Char(' '), 0, 0).toLower();
}
QList<Fence> fences(const QString &markdown) {
    QList<Fence> result;
    Fence current;
    int length = 0, indent = 0, quotes = 0;
    int listContentIndent = 0, listQuoteDepth = 0;
    QChar marker;
    static const QRegularExpression list(QStringLiteral("^\\s*(?:[-+*]|[0-9]+[.)])\\s+"));
    for (QString line : markdown.split(QLatin1Char('\n'))) {
        if (length) {
            quotePrefix(line, quotes);
            line = unindent(line, indent);
            if (MarkdownHighlighter::closesFence(line, length, marker)) {
                current.closed = true;
                result.append(current);
                current = {};
                length = 0;
            } else {
                current.lines.append(line);
            }
            continue;
        }
        const int depth = quotePrefix(line);
        if (depth != listQuoteDepth) listContentIndent = 0;
        listQuoteDepth = depth;
        const auto item = list.match(line);
        int listIndent = 0;
        if (item.hasMatch()) {
            listContentIndent = listIndent = item.capturedLength();
        } else if (!line.trimmed().isEmpty()) {
            int leading = 0;
            while (leading < line.size() && line.at(leading) == QLatin1Char(' ')) ++leading;
            if (leading >= listContentIndent) listIndent = listContentIndent;
            else listContentIndent = 0;
        }
        line.remove(0, listIndent);
        const QString trimmed = line.trimmed();
        length = MarkdownHighlighter::openingFenceLength(line);
        if (!length) continue;
        quotes = depth;
        int leading = 0;
        while (leading < line.size() && line.at(leading) == QLatin1Char(' ')) ++leading;
        indent = listIndent + leading;
        marker = trimmed.at(0);
        current.info = trimmed.mid(length).trimmed();
    }
    if (length) result.append(current);
    return result;
}
}

QList<QPair<int, int>> highlightReadCode(QTextDocument &document, const QString &markdown, bool dark) {
    QList<QTextBlock> imported;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        if (block.blockFormat().hasProperty(QTextFormat::BlockCodeFence)) imported.append(block);
    QList<QPair<int, int>> fallback;
    for (const auto &block : imported) {
        const int number = block.blockNumber();
        if (!fallback.isEmpty() && fallback.last().second + 1 == number)
            fallback.last().second = number;
        else
            fallback.append({number, number});
    }
    auto sourceFences = fences(markdown);
    int sourceLines = 0;
    for (const auto &fence : sourceFences) sourceLines += fence.lines.size();
    if (!sourceFences.isEmpty() && !sourceFences.last().closed
        && sourceLines == imported.size() + 1 && !sourceFences.last().lines.isEmpty()
        && sourceFences.last().lines.last().isEmpty())
        sourceFences.last().lines.removeLast(); // Final newline is not a code line.
    // Validate the complete sequence first. Never let an unmatched source
    // fragment claim a prefix of some later, unrelated imported fence.
    QStringList expectedLines;
    QStringList expectedLanguages;
    for (const auto &fence : sourceFences) {
        for (const auto &line : fence.lines) {
            expectedLines.append(line);
            expectedLanguages.append(languageToken(fence.info));
        }
    }
    if (expectedLines.size() != imported.size()) return fallback;
    for (int i = 0; i < imported.size(); ++i)
        if (imported.at(i).text() != expectedLines.at(i)
            || languageToken(imported.at(i).blockFormat().stringProperty(QTextFormat::BlockCodeLanguage)) != expectedLanguages.at(i))
            return fallback; // Native code geometry remains safe even without source metadata.
    CodeHighlighter highlighter(dark);
    QList<QPair<int, int>> groups;
    int next = 0;
    for (const Fence &fence : sourceFences) {
        // Qt's properties cannot distinguish adjacent same-language fences.
        const auto definition = CodeHighlighter::resolveLanguage(fence.info);
        const int count = fence.lines.size();
        const int found = next;
        next += count;
        if (count) groups.append({imported.at(found).blockNumber(), imported.at(next - 1).blockNumber()});
        if (!definition.isValid()) continue;
        KSyntaxHighlighting::State state;
        for (int i = 0; i < count; ++i) {
            const auto block = imported.at(found + i);
            const auto line = highlighter.tokenize(block.text(), definition, state);
            state = line.state;
            for (const auto &token : line.tokens) {
                QTextCursor cursor(&document);
                cursor.setPosition(block.position() + token.offset);
                cursor.setPosition(block.position() + token.offset + token.length, QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(token.format);
            }
        }
    }
    return groups;
}
