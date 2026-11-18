#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QList>
#include <QString>
#include <QUrl>

namespace PreviewBlocks {

enum class Kind { Table, Image, Mermaid, HorizontalRule };

struct Block {
    int start = 0;
    int end = 0;
    Kind kind = Kind::Table;
    QString markdown;
    QString imagePath;
};

// Resumable scanner checkpoints live outside QTextBlockUserData (owned by the
// syntax highlighter). An edit restarts at its enclosing construct plus one
// predecessor, and stops once the unchanged suffix has the same HTML context.
class Parser {
public:
    void reset(const QString &source);
    void edit(const QString &source, int position, int removed, int added);
    const QList<Block> &blocks() const { return m_blocks; }
    const QList<QPair<int, int>> &fences() const { return m_fences; }
    int parsedLines() const { return m_parsedLines; }
private:
    struct Line { int start, end; QString text; };
    struct State {
        bool comment = false;
        QString html;
        bool operator==(const State &other) const { return comment == other.comment && html == other.html; }
    };
    struct Record {
        int first = 0, last = 0;
        State before, after;
        QList<Block> blocks;
        QList<QPair<int, int>> fences;
    };
    Record scan(int &line, State &state, const QString &source);
    void collect();
    int offset(int line) const;
    void shiftSuffix(int line, int delta);
    int lineStart(int line) const { return m_lines[line].start + offset(line); }
    int lineEnd(int line) const { return m_lines[line].end + offset(line); }
    QList<int> m_offsets, m_constructs;
    QList<Line> m_lines;
    QList<Record> m_records;
    QList<Block> m_blocks;
    QList<QPair<int, int>> m_fences;
    int m_length = 0, m_parsedLines = 0;
};

// UTF-16, half-open source ranges; the final line ending is not included.
// Tables require pipe-separated, equally sized rows and >=3 hyphens per
// delimiter cell (optional alignment colons). Images must occupy a whole line;
// destinations with spaces require <...>. Up to three leading spaces are allowed.
// Fenced content is ignored except exact, case-sensitive "mermaid" info strings;
// an unclosed Mermaid fence extends to EOF. Reference images are not recognized.
QList<Block> parse(const QString &source);
QImage renderHorizontalRule(const QFont &font, const QColor &foreground, int maxWidth);
QImage renderTable(const QString &markdown, const QFont &font,
                   const QColor &foreground, const QColor &background, int maxWidth);
// Local file URLs or relative paths only; no network or data URLs. Input is
// limited to 20 MiB and 24 MP before decode; output fits maxWidth x 1800 without
// upscaling. Invalid, oversized or unsupported images return a null image.
QImage loadImage(const QUrl &url, int maxWidth);

} // namespace PreviewBlocks
