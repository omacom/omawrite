#pragma once

// Header-only so attachment support needs no changes to the application build list.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QImageReader>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>
#include <QVector>
#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

namespace Attachments {
struct Reference {
    qsizetype start;
    qsizetype length;
    QString path;
};

// Deliberately conservative portable paths: no URL escapes, Markdown delimiters,
// drive letters, hidden components, traversal, or platform-specific separators.
inline bool safeRelativePath(const QString &path) {
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9_./-]*$"));
    if (!allowed.match(path).hasMatch() || QDir::isAbsolutePath(path))
        return false;
    for (const QString &part : path.split('/')) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.startsWith('.'))
            return false;
    }
    return true;
}

// Existing documents may use spaces, Unicode, and escaped punctuation. These
// are safe to copy too, but new generated Markdown uses the stricter policy.
inline bool safeLocalPath(const QString &path) {
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\')
        || path.contains(':') || path.contains(QChar(0))) return false;
    for (const QString &part : path.split('/'))
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
    return true;
}

inline QString decodedPath(const QString &path) {
    QString unescaped;
    for (qsizetype i = 0; i < path.size(); ++i) {
        if (path[i] == '\\' && i + 1 < path.size()
            && path[i + 1].unicode() < 128 && path[i + 1].isPunct()) ++i;
        unescaped += path[i];
    }
    return QUrl::fromPercentEncoding(unescaped.toUtf8());
}

inline QString labelKey(QString label) {
    return label.simplified().toCaseFolded();
}

// Source offsets, not rendered HTML: replacements can be one QTextCursor undo
// block. These are candidates, not a complete Markdown parser: the backend
// confirms destinations against Qt's parser before editing. Supports inline
// images and full, collapsed, and shortcut reference images.
inline QVector<Reference> references(const QString &text) {
    QString source = text;
    auto blank = [&source](int start, int length) {
        for (int i = start; i < start + length; ++i)
            if (source[i] != '\n') source[i] = QLatin1Char(' ');
    };
    static const QRegularExpression fences(QStringLiteral("(?m)^ {0,3}(`{3,}|~{3,})[^\\n]*$"));
    auto lines = fences.globalMatch(text);
    int fenceStart = -1;
    QString fence;
    while (lines.hasNext()) {
        const auto match = lines.next();
        const QString marker = match.captured(1);
        if (fenceStart < 0) {
            fenceStart = match.capturedStart();
            fence = marker;
        } else if (marker[0] == fence[0] && marker.size() >= fence.size()
                   && text.mid(match.capturedEnd(1), match.capturedEnd() - match.capturedEnd(1)).trimmed().isEmpty()) {
            blank(fenceStart, match.capturedEnd() - fenceStart);
            fenceStart = -1;
        }
    }
    if (fenceStart >= 0) blank(fenceStart, source.size() - fenceStart);
    static const QRegularExpression code(QStringLiteral("(`+)([^`]|(?!\\1)`)*?\\1"));
    auto codes = code.globalMatch(source);
    QVector<QPair<int, int>> codeSpans;
    while (codes.hasNext()) {
        const auto match = codes.next();
        codeSpans.append({int(match.capturedStart()), int(match.capturedLength())});
    }
    for (const auto &span : codeSpans) blank(span.first, span.second);

    auto closingBracket = [&source](qsizetype start) -> qsizetype {
        int depth = 1;
        for (qsizetype i = start + 1; i < source.size(); ++i) {
            if (source[i] == '\\') { ++i; continue; }
            if (source[i] == '[') ++depth;
            if (source[i] == ']' && --depth == 0) return i;
        }
        return -1;
    };
    auto destinationAt = [&source](qsizetype start) -> Reference {
        while (start < source.size() && source[start].isSpace()) ++start;
        const bool angle = start < source.size() && source[start] == '<';
        if (angle) ++start;
        qsizetype end = start;
        int depth = 0;
        for (; end < source.size(); ++end) {
            const QChar ch = source[end];
            if (ch == '\\') { ++end; continue; }
            if (angle) { if (ch == '>' || ch == '\n') break; }
            else {
                if (ch.isSpace() || (ch == ')' && depth == 0)) break;
                if (ch == '(') ++depth;
                if (ch == ')') --depth;
            }
        }
        if (end >= source.size() || end == start || depth != 0
            || (angle && source[end] != '>')) return {-1, 0, QString()};
        return {start, end - start, source.mid(start, end - start)};
    };
    QVector<Reference> result;
    QMap<QString, bool> labels;
    for (qsizetype i = 0; i + 1 < source.size(); ++i) {
        if (source[i] == '\\') { ++i; continue; }
        if (source[i] != '!' || source[i + 1] != '[') continue;
        const qsizetype close = closingBracket(i + 1);
        if (close < 0) continue;
        const QString alt = source.mid(i + 2, close - i - 2);
        qsizetype next = close + 1;
        if (next < source.size() && source[next] == '(') {
            const Reference ref = destinationAt(next + 1);
            if (ref.start >= 0) result.append(ref);
        } else if (next < source.size() && source[next] == '[') {
            const qsizetype end = closingBracket(next);
            if (end >= 0) {
                const QString label = source.mid(next + 1, end - next - 1);
                labels.insert(labelKey(label.isEmpty() ? alt : label), true);
            }
        } else labels.insert(labelKey(alt), true);
        i = close;
    }
    static const QRegularExpression definition(QStringLiteral("(?m)^ {0,3}\\[([^\\]\\n]+)\\]:[ \\t]*"));
    auto matches = definition.globalMatch(source);
    QMap<QString, bool> seen;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString key = labelKey(match.captured(1));
        if (!labels.contains(key) || seen.contains(key)) continue;
        seen.insert(key, true);
        const Reference ref = destinationAt(match.capturedEnd());
        if (ref.start >= 0) result.append(ref);
    }
    return result;
}

// Let Qt's Markdown parser enumerate save dependencies as well. Unlike the
// source-offset scanner this covers nested containers and supported inline HTML.
// No layout/render is requested, and we never load a resource or fetch a URL.
inline QStringList imagePaths(const QString &text) {
    class ResourceFreeDocument : public QTextDocument {
        QVariant loadResource(int, const QUrl &) override { return QVariant(); }
    } parsed;
    parsed.setMarkdown(text);
    QStringList paths;
    for (QTextBlock block = parsed.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (format.isImageFormat()) paths.append(format.toImageFormat().name());
        }
    }
    return paths;
}

inline constexpr qint64 MaxImageFileBytes = 20 * 1024 * 1024;
inline constexpr qint64 MaxImagePixels = 24 * 1000 * 1000;

// Decode original pixels only from bounded local files. Preview scaling is a
// separate concern; clipboard copies must not inherit viewport dimensions.
inline QImage loadLocalImage(const QUrl &url) {
    if (!url.isValid() || url.hasQuery() || url.hasFragment()) return {};
    QString path;
    if (url.isLocalFile() && (url.host().isEmpty() || url.host() == QStringLiteral("localhost")))
        path = url.toLocalFile();
    else if (url.isRelative() && url.host().isEmpty()) path = url.path();
    else return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > MaxImageFileBytes) return {};
    QImageReader reader(&file);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || qint64(size.width()) * size.height() > MaxImagePixels) return {};
    const QImage image = reader.read();
    if (image.isNull() || qint64(image.width()) * image.height() > MaxImagePixels) return {};
    return image;
}

inline bool sameFileContents(const QString &a, const QString &b) {
    QFile left(a), right(b);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)
        || left.size() != right.size()) return false;
    while (!left.atEnd()) {
        const QByteArray bytes = left.read(65536);
        if (bytes.isEmpty() || bytes != right.read(bytes.size())) return false;
    }
    return left.error() == QFile::NoError && right.error() == QFile::NoError;
}

// Refuse symlink components, even ones currently pointing inside the document
// folder. Never overwrite an attachment; an identical existing copy is reusable.
inline bool copyInto(const QString &source, const QString &root, const QString &relative, QString *error) {
    auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    if (!safeLocalPath(relative)) return fail(QStringLiteral("Unsafe attachment path: %1").arg(relative));
    QString parent = QFileInfo(root).absoluteFilePath();
    if (!QFileInfo(parent).isDir()) return fail(QStringLiteral("Attachment destination folder is missing."));
    const QStringList parts = relative.split('/');
    for (int i = 0; i + 1 < parts.size(); ++i) {
        parent = QDir(parent).filePath(parts[i]);
        const QFileInfo info(parent);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return fail(QStringLiteral("Unsafe attachment folder: %1").arg(parent));
        if (!QDir().mkpath(parent)) return fail(QStringLiteral("Could not create attachment folder: %1").arg(parent));
    }
    const QString target = QDir(parent).filePath(parts.last());
    const QFileInfo targetInfo(target);
    if (targetInfo.isSymLink()) return fail(QStringLiteral("Attachment target is a symlink: %1").arg(relative));
    if (targetInfo.exists()) {
        if (targetInfo.isFile() && sameFileContents(source, target)) return true;
        return fail(QStringLiteral("Attachment already exists: %1").arg(relative));
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Could not read attachment: %1").arg(relative));
    QTemporaryFile output(QDir(parent).filePath(QStringLiteral(".omawrite-XXXXXX")));
    if (!output.open()) return fail(QStringLiteral("Could not stage attachment: %1").arg(relative));
    while (!input.atEnd()) {
        const QByteArray bytes = input.read(65536);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size())
            return fail(QStringLiteral("Could not copy attachment: %1").arg(relative));
    }
    if (input.error() != QFile::NoError || !output.flush())
        return fail(QStringLiteral("Could not flush attachment: %1").arg(relative));
#ifdef Q_OS_UNIX
    if (::fsync(output.handle()) != 0)
        return fail(QStringLiteral("Could not sync attachment: %1").arg(relative));
#elif defined(Q_OS_WIN)
    if (::_commit(output.handle()) != 0)
        return fail(QStringLiteral("Could not sync attachment: %1").arg(relative));
#endif
    output.close();
    // QFile::rename refuses to replace an existing destination.
    if (!output.rename(target)) return fail(QStringLiteral("Could not install attachment: %1").arg(relative));
    output.setAutoRemove(false);
    return true;
}
} // namespace Attachments
