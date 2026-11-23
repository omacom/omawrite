#pragma once

#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QSet>
#include <QUrl>
#include <QVector>
#include <algorithm>

// Source-based lookup shared by editing, reading, and hover. Never loads URLs.
namespace MarkdownReferences {
inline QString unescape(QString text) {
    static const QRegularExpression escape(QStringLiteral(R"(\\([!-/:-@\[-`{-~]))"));
    return text.replace(escape, QStringLiteral("\\1"));
}
inline QString key(const QString &label) { return unescape(label).simplified().toCaseFolded(); }
inline QString noteUrl(const QString &label) {
    return QStringLiteral("write-footnote:") + QString::fromLatin1(QUrl::toPercentEncoding(label));
}
inline QString noteKey(const QString &url) {
    return url.startsWith(QStringLiteral("write-footnote:"))
        ? QUrl::fromPercentEncoding(url.mid(15).toUtf8()) : QString();
}
struct Definition {
    QString text;
    int start = 0, end = 0;
    QString prefix;
    bool operator==(const Definition &other) const { return text == other.text; }
};
struct Occurrence {
    int start = 0, end = 0, contentStart = 0, contentLength = 0;
    QString label;
    bool footnote = false;
};

// Preserve source offsets while excluding literal code and HTML comments.
inline QString proseMask(const QString &text) {
    QString mask = text;
    auto blank = [&](int from, int end) {
        for (int i = from; i < end; ++i) if (mask[i] != '\n') mask[i] = QLatin1Char(' ');
    };
    static const QRegularExpression fence(QStringLiteral("^(?: {0,3}>[ \\t]?| {0,3}(?:[-+*]|[0-9]+[.)])[ \\t]+)* {0,3}(`{3,}|~{3,})(.*)$"));
    static const QRegularExpression quotes(QStringLiteral("^(?: {0,3}>[ \\t]?)*"));
    static const QRegularExpression listStart(QStringLiteral("^ {0,3}(?:[-+*]|[0-9]+[.)])[ \\t]+"));
    QChar marker;
    int fenceLength = 0, fenceQuotes = 0, listIndent = 0;
    for (int from = 0; from < text.size();) {
        int end = text.indexOf('\n', from);
        if (end < 0) end = text.size();
        const QString line = text.mid(from, end - from);
        const auto match = fence.match(line);
        const int quoteLength = quotes.match(line).capturedLength();
        const int quoteCount = line.left(quoteLength).count('>');
        const QString content = line.mid(quoteLength);
        if (!marker.isNull() && (quoteCount < fenceQuotes
            || (listIndent && !content.trimmed().isEmpty() && !content.startsWith(QString(listIndent, ' '))))) marker = {};
        if (!marker.isNull()) {
            blank(from, end);
            if (match.hasMatch() && match.captured(1).at(0) == marker
                && match.capturedLength(1) >= fenceLength && match.captured(2).trimmed().isEmpty()) marker = {};
        } else if (match.hasMatch() && !(match.captured(1).at(0) == '`' && match.captured(2).contains('`'))) {
            marker = match.captured(1).at(0); fenceLength = match.capturedLength(1);
            fenceQuotes = quoteCount;
            listIndent = listStart.match(content).capturedLength();
            blank(from, end);
        } else {
            if (content.startsWith(QStringLiteral("    ")) || content.startsWith('\t')) blank(from, end);
        }
        from = end + 1;
    }
    for (int at = 0; at < mask.size();) {
        if (mask[at] == '\\') { at += 2; continue; }
        if (QStringView(mask).mid(at, 4) == QStringLiteral("<!--")) {
            int end = mask.indexOf(QStringLiteral("-->"), at + 4);
            end = end < 0 ? mask.size() : end + 3;
            blank(at, end); at = end; continue;
        }
        if (mask[at] != '`') { ++at; continue; }
        int length = 1;
        while (at + length < mask.size() && mask[at + length] == '`') ++length;
        int close = at + length;
        while ((close = mask.indexOf(QString(length, '`'), close)) >= 0) {
            if ((close == 0 || mask[close - 1] != '`')
                && (close + length == mask.size() || mask[close + length] != '`')) break;
            while (close < mask.size() && mask[close] == '`') ++close;
        }
        if (close >= 0) { blank(at, close + length); at = close + length; }
        else at += length;
    }
    return mask;
}

struct Index {
    QHash<QString, Definition> links, notes;
    QVector<Occurrence> occurrences;
    QHash<QString, int> numbers;
    QSet<QString> reservedLinks, reservedNotes;

    explicit Index(const QString &text = {}) {
        if (!text.contains('[')) return;
        QString mask = proseMask(text);
        static const QRegularExpression reserved(QStringLiteral(R"(\[(\^?)(ref\d+|\d+)\])"), QRegularExpression::CaseInsensitiveOption);
        auto ids = reserved.globalMatch(mask);
        while (ids.hasNext()) {
            const auto id = ids.next();
            (id.captured(1).isEmpty() ? reservedLinks : reservedNotes).insert(key(id.captured(2)));
        }
        static const QRegularExpression definition(QStringLiteral(R"(^((?: {0,3}>[ \t]?)* {0,3})\[((?:\\.|[^\]\\\n])+)\]:[ \t]*(.*)$)"));
        static const QRegularExpression quotes(QStringLiteral("^(?: {0,3}>[ \\t]?)*"));
        static const QRegularExpression title(QStringLiteral(R"(^[ \t]*(?:\x22[^\x22\n]*\x22|'[^'\n]*'|\([^\n]*\))[ \t]*$)"));
        for (int from = 0; from < text.size();) {
            int end = text.indexOf('\n', from);
            if (end < 0) end = text.size();
            const auto match = definition.match(text.mid(from, end - from));
            if (match.hasMatch() && mask[from + match.capturedLength(1)] == '[') {
                const QString prefix = match.captured(1);
                QString label = match.captured(2), body = match.captured(3);
                auto nextLine = [&](int at, int &last) {
                    last = text.indexOf('\n', at);
                    if (last < 0) last = text.size();
                    QString line = text.mid(at, last - at);
                    if (prefix.contains('>')) line.remove(0, quotes.match(line).capturedLength());
                    return line;
                };
                const bool note = label.startsWith('^');
                if (note) {
                    label.remove(0, 1);
                    int next = end + 1;
                    while (next < text.size()) {
                        int last;
                        const QString line = nextLine(next, last);
                        if (line.trimmed().isEmpty()) {
                            // Blank paragraphs belong to the note only when followed by an indented continuation.
                            int after = last + 1, followingEnd;
                            QString following;
                            while (after < text.size()) {
                                following = nextLine(after, followingEnd);
                                if (!following.trimmed().isEmpty()) break;
                                after = followingEnd + 1;
                            }
                            if (after >= text.size() || !(following.startsWith(QStringLiteral("    ")) || following.startsWith('\t'))) break;
                            body += '\n'; end = last; next = last + 1; continue;
                        }
                        if (!line.startsWith(QStringLiteral("    ")) && !line.startsWith('\t')) break;
                        body += '\n' + line.mid(line.startsWith('\t') ? 1 : 4);
                        end = last; next = last + 1;
                    }
                    if (!key(label).isEmpty() && !notes.contains(key(label))) notes.insert(key(label), {body, from, end, prefix});
                } else {
                    if (body.isEmpty() && end < text.size()) {
                        int last;
                        const QString following = nextLine(end + 1, last);
                        if (!following.trimmed().isEmpty() && !following.startsWith(QStringLiteral("    ")) && !following.startsWith('\t')) {
                            body = following.trimmed(); end = last;
                        }
                    }
                    if (end < text.size()) {
                        int last;
                        const QString following = nextLine(end + 1, last);
                        if (title.match(following).hasMatch()) { body += ' ' + following.trimmed(); end = last; }
                    }
                    // CommonMark destinations, with an optional title.
                    static const QRegularExpression destination(QStringLiteral(R"(^(?:<((?:\\.|[^<>\n])*)>|((?:\\.|[^\s])+?))(?:[ \t]+(?:\x22[^\x22\n]*\x22|'[^'\n]*'|\([^\n]*\)))?[ \t]*$)"));
                    const auto url = destination.match(body);
                    if (url.hasMatch() && !links.contains(key(label)))
                        links.insert(key(label), {unescape(url.captured(1).isNull() ? url.captured(2) : url.captured(1)), from, end, prefix});
                }
                for (int i = from; i < end; ++i) if (mask[i] != '\n') mask[i] = QLatin1Char(' ');
            }
            from = end + 1;
        }
        occurrences = scan(mask, text);
        int number = 0;
        for (const auto &ref : occurrences)
            if (ref.footnote && !numbers.contains(ref.label)) numbers.insert(ref.label, ++number);
    }

    QVector<Occurrence> scan(const QString &text, const QString &source = {}) const {
        const QString &raw = source.isNull() ? text : source;
        QVector<Occurrence> result;
        QHash<int, int> pairs;
        QList<int> openings;
        for (int i = 0; i < text.size(); ++i) {
            if (text[i] == '\\') { ++i; continue; }
            if (text[i] == '\n') openings.clear();
            else if (text[i] == '[') openings.append(i);
            else if (text[i] == ']' && !openings.isEmpty()) pairs.insert(openings.takeLast(), i);
        }
        auto closing = [&](int at) { return pairs.value(at, -1); };
        static const QRegularExpression definitionPrefix(QStringLiteral("^(?: {0,3}>[ \\t]?)* {0,3}$"));
        for (int at = 0; at < text.size(); ++at) {
            if (text[at] == '\\') { ++at; continue; }
            if (text[at] != '[') continue;
            const int close = closing(at);
            if (close < 0) continue;
            const bool image = at > 0 && text[at - 1] == '!';
            const QString content = raw.mid(at + 1, close - at - 1);
            if (close + 1 < text.size() && text[close + 1] == ':'
                && definitionPrefix.match(text.mid(text.lastIndexOf('\n', at) + 1, at - text.lastIndexOf('\n', at) - 1)).hasMatch()) {
                at = close; continue;
            }
            if (close + 1 < text.size() && text[close + 1] == '(') {
                // Do not reinterpret inline link labels/destinations as refs.
                int depth = 1, end = close + 2;
                for (; end < text.size() && text[end] != '\n' && depth; ++end) {
                    if (text[end] == '\\') { ++end; continue; }
                    if (text[end] == '(') ++depth;
                    if (text[end] == ')') --depth;
                }
                at = depth ? close : end - 1; continue;
            }
            bool note = content.startsWith('^');
            QString label = key(note ? content.mid(1) : content);
            int end = close + 1;
            if (!note && end < text.size() && text[end] == '[') {
                const int last = closing(end);
                if (last < 0) { at = close; continue; }
                const QString explicitLabel = raw.mid(end + 1, last - end - 1);
                if (!explicitLabel.isEmpty()) label = key(explicitLabel);
                end = last + 1;
            }
            if (!image && ((note && notes.contains(label)) || (!note && links.contains(label))))
                result.append({at, end, at + 1 + int(note), close - at - 1 - int(note), label, note});
            at = end - 1;
        }
        return result;
    }
    QString url(const Occurrence &ref) const { return ref.footnote ? noteUrl(ref.label) : links.value(ref.label).text; }
    QString urlAt(int position) const {
        for (const auto &ref : occurrences)
            if (position >= (ref.footnote ? ref.start : ref.contentStart)
                && position < (ref.footnote ? ref.end : ref.contentStart + ref.contentLength)) return url(ref);
        return {};
    }
    QString nextLabel(bool footnote) const {
        const auto &definitions = footnote ? notes : links;
        for (int i = 1;; ++i) {
            const QString label = (footnote ? QString() : QStringLiteral("ref")) + QString::number(i);
            if (!definitions.contains(label) && !(footnote ? reservedNotes : reservedLinks).contains(label)) return label;
        }
    }
};

// Only resolved notes are transformed. Code and unknown notes stay literal;
// links, images and formatting in the body still go through Qt's safe importer.
inline QString readMarkdown(const QString &source, const Index *original = nullptr) {
    const Index index(source);
    // Table/diagram replacement may remove references from the imported text.
    // Keep document-wide source numbering identical to Edit mode.
    const auto &numbers = original ? original->numbers : index.numbers;
    struct Patch { int start, length; QString text; };
    QVector<Patch> patches;
    for (const auto &ref : index.occurrences) if (ref.footnote)
        patches.append({ref.start, ref.end - ref.start,
            QStringLiteral("<a href=\"%1\"><sup>%2</sup></a>").arg(noteUrl(ref.label)).arg(numbers.value(ref.label))});
    for (auto it = index.notes.cbegin(); it != index.notes.cend(); ++it) {
        if (!numbers.contains(it.key())) continue;
        patches.append({it->start, it->end - it->start,
            QStringLiteral("\n\n%1<a name=\"%2\"><b>%3.</b></a> ").arg(it->prefix, noteUrl(it.key())).arg(numbers.value(it.key()))
                + QString(it->text).replace(QStringLiteral("\n"), '\n' + it->prefix) + QStringLiteral("\n\n")});
    }
    std::sort(patches.begin(), patches.end(), [](const auto &a, const auto &b) { return a.start > b.start; });
    QString result = source;
    for (const auto &patch : patches) result.replace(patch.start, patch.length, patch.text);
    return result;
}
}
