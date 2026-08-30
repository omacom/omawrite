#include "codeblockhighlighter.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextStream>
#include <QUrl>

PreviewPalette PreviewPalette::fallback(bool darkMode) {
    PreviewPalette palette;
    palette.darkMode = darkMode;
    if (darkMode) {
        palette.background = QStringLiteral("#1a1b26");
        palette.foreground = QStringLiteral("#a9b1d6");
        palette.accent = QStringLiteral("#7aa2f7");
        palette.selection = QStringLiteral("#292e42");
        palette.muted = QStringLiteral("#414868");
        palette.lighterBackground = QStringLiteral("#24283b");
        palette.darkForeground = QStringLiteral("#565f89");
        palette.lightForeground = QStringLiteral("#b4bee6");
        palette.red = QStringLiteral("#f7768e");
        palette.yellow = QStringLiteral("#e0af68");
        palette.orange = QStringLiteral("#eb927b");
        palette.green = QStringLiteral("#9ece6a");
        palette.cyan = QStringLiteral("#449dab");
        palette.blue = QStringLiteral("#7aa2f7");
        palette.magenta = QStringLiteral("#ad8ee6");
        palette.brightYellow = QStringLiteral("#ff9e64");
        palette.brightBlue = QStringLiteral("#7da6ff");
    } else {
        palette.background = QStringLiteral("#ffffff");
        palette.foreground = QStringLiteral("#222324");
        palette.accent = QStringLiteral("#2077b2");
        palette.selection = QStringLiteral("#2077b2");
        palette.muted = QStringLiteral("#aeb1b5");
        palette.lighterBackground = QStringLiteral("#f4f4f5");
        palette.darkForeground = QStringLiteral("#6b6e73");
        palette.lightForeground = QStringLiteral("#3b3d40");
        palette.red = QStringLiteral("#c0392b");
        palette.yellow = QStringLiteral("#b8860b");
        palette.orange = QStringLiteral("#d35400");
        palette.green = QStringLiteral("#2e7d32");
        palette.cyan = QStringLiteral("#1a7f8e");
        palette.blue = QStringLiteral("#2077b2");
        palette.magenta = QStringLiteral("#8e44ad");
        palette.brightYellow = QStringLiteral("#d35400");
        palette.brightBlue = QStringLiteral("#2077b2");
    }
    return palette;
}

QString PreviewPalette::hex(const QString &value, const QString &ifInvalid) const {
    const QColor color(value);
    return color.isValid() ? color.name() : ifInvalid;
}

bool CodeBlockHighlighter::isCodeBlock(const QTextBlock &block) {
    if (!block.isValid())
        return false;

    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(QTextFormat::BlockCodeLanguage)
        || format.hasProperty(QTextFormat::BlockCodeFence)
        || format.nonBreakableLines();
}

QString CodeBlockHighlighter::codeLanguage(const QTextBlock &block) {
    if (!block.isValid())
        return {};

    const QString info =
        block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage).trimmed();
    if (info.isEmpty())
        return {};

    int end = 0;
    while (end < info.size()) {
        const QChar character = info.at(end);
        if (character.isSpace() || character == QLatin1Char('{'))
            break;
        ++end;
    }
    return info.left(end).toLower();
}

bool CodeBlockHighlighter::highlightAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("highlight")).isEmpty();
}

static QString htmlBody(const QString &qtHtml) {
    static const QRegularExpression bodyRe(
        QStringLiteral("<body[^>]*>(.*)</body>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = bodyRe.match(qtHtml);
    return match.hasMatch() ? match.captured(1) : qtHtml;
}

struct CodeFence {
    QString language;
    QString text;
};

static QList<CodeFence> collectFences(const QTextDocument &document) {
    QList<CodeFence> fences;
    CodeFence current;
    bool inCode = false;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const bool code = CodeBlockHighlighter::isCodeBlock(block);
        if (code) {
            const QString language = CodeBlockHighlighter::codeLanguage(block);
            if (!inCode) {
                current = CodeFence{language, {}};
            } else if (!language.isEmpty() && language != current.language) {
                fences.append(current);
                current = CodeFence{language, {}};
            } else {
                current.text += QLatin1Char('\n');
            }
            current.text += block.text();
            inCode = true;
        } else if (inCode) {
            fences.append(current);
            inCode = false;
        }
    }
    if (inCode)
        fences.append(current);
    return fences;
}

static bool allowedUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    return scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("mailto")
        || scheme == QStringLiteral("file");
}

static QString sanitizeUrls(QString html, const QUrl &documentUrl) {
    static const QRegularExpression attrRe(
        QStringLiteral(R"(\b(href|src)\s*=\s*(['"])([^'"]*)\2)"),
        QRegularExpression::CaseInsensitiveOption);

    QString result;
    int last = 0;
    QRegularExpressionMatchIterator it = attrRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += html.mid(last, match.capturedStart() - last);

        QUrl url(match.captured(3));
        if (url.isRelative() && documentUrl.isLocalFile())
            url = documentUrl.resolved(url);

        if (allowedUrl(url)) {
            result += match.captured(1) + QLatin1Char('=') + match.captured(2)
                + url.toString(QUrl::FullyEncoded) + match.captured(2);
        } else {
            result += match.captured(1) + QStringLiteral("=\"\"");
        }
        last = match.capturedEnd();
    }
    result += html.mid(last);
    return result;
}

static QString syntaxName(const QString &language) {
    static const QHash<QString, QString> aliases = {
        {QStringLiteral("js"), QStringLiteral("js")},
        {QStringLiteral("javascript"), QStringLiteral("js")},
        {QStringLiteral("jsx"), QStringLiteral("js")},
        {QStringLiteral("node"), QStringLiteral("js")},
        {QStringLiteral("ts"), QStringLiteral("ts")},
        {QStringLiteral("typescript"), QStringLiteral("ts")},
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("python"), QStringLiteral("python")},
        {QStringLiteral("c++"), QStringLiteral("c++")},
        {QStringLiteral("cpp"), QStringLiteral("c++")},
        {QStringLiteral("cc"), QStringLiteral("c++")},
        {QStringLiteral("cxx"), QStringLiteral("c++")},
        {QStringLiteral("cs"), QStringLiteral("cs")},
        {QStringLiteral("csharp"), QStringLiteral("cs")},
        {QStringLiteral("c#"), QStringLiteral("cs")},
        {QStringLiteral("sh"), QStringLiteral("sh")},
        {QStringLiteral("bash"), QStringLiteral("bash")},
        {QStringLiteral("zsh"), QStringLiteral("zsh")},
        {QStringLiteral("shell"), QStringLiteral("sh")},
        {QStringLiteral("yml"), QStringLiteral("yaml")},
        {QStringLiteral("yaml"), QStringLiteral("yaml")},
        {QStringLiteral("rb"), QStringLiteral("ruby")},
        {QStringLiteral("ruby"), QStringLiteral("ruby")},
        {QStringLiteral("rs"), QStringLiteral("rust")},
        {QStringLiteral("rust"), QStringLiteral("rust")},
        {QStringLiteral("golang"), QStringLiteral("go")},
        {QStringLiteral("go"), QStringLiteral("go")},
        {QStringLiteral("kt"), QStringLiteral("kotlin")},
        {QStringLiteral("md"), QStringLiteral("markdown")},
        {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("xml"), QStringLiteral("xml")},
        {QStringLiteral("json"), QStringLiteral("json")},
        {QStringLiteral("css"), QStringLiteral("css")},
        {QStringLiteral("sql"), QStringLiteral("sql")},
        {QStringLiteral("toml"), QStringLiteral("toml")},
        {QStringLiteral("lua"), QStringLiteral("lua")},
        {QStringLiteral("php"), QStringLiteral("php")},
        {QStringLiteral("swift"), QStringLiteral("swift")},
        {QStringLiteral("java"), QStringLiteral("java")},
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("qml"), QStringLiteral("js")},
    };
    return aliases.value(language, language);
}

static QString unwrapHighlightPre(QString html) {
    static const QRegularExpression wrapped(
        QStringLiteral("^\\s*<pre[^>]*>(.*)</pre>\\s*$"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = wrapped.match(html);
    return match.hasMatch() ? match.captured(1) : html.trimmed();
}

static QString writeHighlightTheme(const PreviewPalette &palette) {
    const PreviewPalette fallback = PreviewPalette::fallback(palette.darkMode);
    const auto colour = [&](const QString &value, const QString &fallbackValue) {
        return palette.hex(value.isEmpty() ? fallbackValue : value, fallbackValue);
    };

    const QString path = QDir::tempPath() + QStringLiteral("/omawrite-highlight.theme");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return {};

    QTextStream out(&file);
    out << "Description=\"Omawrite Omarchy\"\n\n"
        << "Default        = { Colour=\""
        << colour(palette.foreground, fallback.foreground) << "\" }\n"
        << "Canvas         = { Colour=\""
        << (palette.darkMode ? QStringLiteral("#1c1a1a") : QStringLiteral("#f8f8f8"))
        << "\" }\n"
        << "Number         = { Colour=\""
        << colour(palette.brightYellow.isEmpty() ? palette.orange : palette.brightYellow,
                  fallback.brightYellow) << "\" }\n"
        << "Escape         = { Colour=\""
        << colour(palette.cyan, fallback.cyan) << "\" }\n"
        << "String         = { Colour=\""
        << colour(palette.green, fallback.green) << "\" }\n"
        << "StringPreProc  = { Colour=\""
        << colour(palette.green, fallback.green) << "\" }\n"
        << "BlockComment   = { Colour=\""
        << colour(palette.darkForeground.isEmpty() ? palette.muted : palette.darkForeground,
                  fallback.darkForeground) << "\" }\n"
        << "PreProcessor   = { Colour=\""
        << colour(palette.magenta, fallback.magenta) << "\" }\n"
        << "LineNum        = { Colour=\""
        << colour(palette.darkForeground, fallback.darkForeground) << "\" }\n"
        << "Operator       = { Colour=\""
        << colour(palette.lightForeground, fallback.lightForeground) << "\" }\n"
        << "LineComment = BlockComment\n"
        << "Interpolation  = Escape\n\n"
        << "Keywords = {\n"
        << "  { Colour=\"" << colour(palette.accent.isEmpty() ? palette.blue : palette.accent,
                                    fallback.accent) << "\" },\n"
        << "  { Colour=\"" << colour(palette.cyan, fallback.cyan) << "\" },\n"
        << "  { Colour=\"" << colour(palette.magenta, fallback.magenta) << "\" },\n"
        << "  { Colour=\"" << colour(palette.yellow, fallback.yellow) << "\" },\n"
        << "  { Colour=\"" << colour(palette.foreground, fallback.foreground) << "\" },\n"
        << "  { Colour=\"" << colour(palette.brightBlue, fallback.brightBlue) << "\" },\n"
        << "}\n";
    file.close();
    return path;
}

static QString runHighlight(const QString &source, const QString &syntax,
                            const PreviewPalette &palette) {
    const QString binary = QStandardPaths::findExecutable(QStringLiteral("highlight"));
    if (binary.isEmpty() || source.size() > 512 * 1024)
        return {};

    const auto start = [&](const QStringList &extra) {
        QProcess process;
        process.setWorkingDirectory(QDir::tempPath());
        QStringList arguments{
            QStringLiteral("-O"), QStringLiteral("html"),
            QStringLiteral("-f"),
            QStringLiteral("-S"), syntax,
            QStringLiteral("--inline-css"),
            QStringLiteral("--quiet"),
            QStringLiteral("--no-version-info"),
            QStringLiteral("--no-trailing-nl"),
            QStringLiteral("--encoding=utf-8"),
        };
        arguments.append(extra);
        process.start(binary, arguments);
        if (!process.waitForStarted(400))
            return QString();
        process.write(source.toUtf8());
        process.closeWriteChannel();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(400);
            return QString();
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return QString();
        return QString::fromUtf8(process.readAllStandardOutput());
    };

    QStringList themed;
    const QString themePath = writeHighlightTheme(palette);
    if (!themePath.isEmpty())
        themed << QStringLiteral("-s") << themePath;

    QString output = themed.isEmpty() ? QString() : start(themed);
    if (output.isEmpty())
        output = start({});
    if (output.isEmpty())
        return {};
    return unwrapHighlightPre(output);
}

static QString colorize(const QString &source, const QString &language,
                        const PreviewPalette &palette) {
    if (source.isEmpty())
        return {};
    if (language.isEmpty())
        return source.toHtmlEscaped();

    const QString colored = runHighlight(source, syntaxName(language), palette);
    return colored.isEmpty() ? source.toHtmlEscaped() : colored;
}

static int countPreTags(const QString &html) {
    int count = 0;
    int position = 0;
    while ((position = html.indexOf(QLatin1String("<pre"), position, Qt::CaseInsensitive)) >= 0) {
        ++count;
        position += 4;
    }
    return count;
}

static int fenceLineCount(const CodeFence &fence) {
    return fence.text.isEmpty() ? 1 : fence.text.count(QLatin1Char('\n')) + 1;
}

static QString emitFence(const CodeFence &fence, const PreviewPalette &palette) {
    QString html = QStringLiteral("<pre");
    if (!fence.language.isEmpty()) {
        html += QStringLiteral(" class=\"language-");
        html += fence.language;
        html += QLatin1Char('"');
    }
    html += QLatin1Char('>');
    html += colorize(fence.text, fence.language, palette);
    html += QStringLiteral("</pre>\n");
    return html;
}

static QString replacePreRuns(const QString &html, const QList<CodeFence> &fences,
                              const PreviewPalette &palette) {
    static const QRegularExpression runRe(
        QStringLiteral("(?:<pre[^>]*>.*?</pre>\\s*)+"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption);

    QString result;
    int last = 0;
    int fenceIndex = 0;
    QRegularExpressionMatchIterator it = runRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += html.mid(last, match.capturedStart() - last);
        int remaining = countPreTags(match.captured(0));
        if (remaining <= 0 || fenceIndex >= fences.size()) {
            result += match.captured(0);
        } else {
            while (remaining > 0 && fenceIndex < fences.size()) {
                const CodeFence &fence = fences.at(fenceIndex++);
                result += emitFence(fence, palette);
                remaining -= fenceLineCount(fence);
            }
        }
        last = match.capturedEnd();
    }
    result += html.mid(last);
    return result;
}

static QString wrapInlineCode(QString html) {
    static const QRegularExpression inlineCodeRe(
        QStringLiteral("<span style=\"[^\"]*font-family:\\s*'monospace';?[^\"]*\">(.*?)</span>"),
        QRegularExpression::CaseInsensitiveOption);
    html.replace(inlineCodeRe, QStringLiteral("<code>\\1</code>"));
    return html;
}

QString CodeBlockHighlighter::html(const QString &markdown, const QString &title,
                                   const PreviewPalette &palette,
                                   const QUrl &documentUrl) {
    QTextDocument document;
    document.setMarkdown(markdown, QTextDocument::MarkdownNoHTML);

    const PreviewPalette fallback = PreviewPalette::fallback(palette.darkMode);
    const QString pageBackground = palette.hex(palette.background, fallback.background);
    const QString pageForeground = palette.hex(palette.foreground, fallback.foreground);
    const QString pageAccent = palette.hex(palette.accent, fallback.accent);
    const QString codeBackground = palette.darkMode
        ? QStringLiteral("#1c1a1a") : QStringLiteral("#f8f8f8");
    const QString muted = palette.hex(
        palette.darkForeground.isEmpty() ? palette.muted : palette.darkForeground,
        fallback.darkForeground);
    const int fontPixelSize = palette.fontPixelSize > 0 ? palette.fontPixelSize : 20;

    QString body = htmlBody(document.toHtml());
    body = wrapInlineCode(body);
    body = replacePreRuns(body, collectFences(document), palette);
    static const QRegularExpression coloredLinkRe(
        QStringLiteral("<a href=\"([^\"]*)\"><span style=\"[^\"]*color:[^\"]*\">(.*?)</span></a>"),
        QRegularExpression::CaseInsensitiveOption);
    body.replace(coloredLinkRe, QStringLiteral("<a href=\"\\1\">\\2</a>"));
    static const QRegularExpression openBlockRe(
        QStringLiteral("<(p|h[1-6]|li|ul|ol)(?:\\s[^>]*)?>"),
        QRegularExpression::CaseInsensitiveOption);
    body.replace(openBlockRe, QStringLiteral("<\\1>"));
    static const QRegularExpression fontSizeRe(
        QStringLiteral("font-size:\\s*[^;\"']+;?\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    body.replace(fontSizeRe, QString());
    body = sanitizeUrls(body, documentUrl);

    return QStringLiteral(
               "<!DOCTYPE html>\n"
               "<html><head>\n"
               "<meta charset=\"utf-8\">\n"
               "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
               "<title>%1</title>\n"
               "<style>\n"
               "@font-face { font-family: \"iA Writer Mono S\"; font-weight: 400; font-style: normal;\n"
               "  src: url(\"iAWriterMonoS-Regular.ttf\") format(\"truetype\"); }\n"
               "@font-face { font-family: \"iA Writer Mono S\"; font-weight: 400; font-style: italic;\n"
               "  src: url(\"iAWriterMonoS-Italic.ttf\") format(\"truetype\"); }\n"
               "@font-face { font-family: \"iA Writer Mono S\"; font-weight: 700; font-style: normal;\n"
               "  src: url(\"iAWriterMonoS-Bold.ttf\") format(\"truetype\"); }\n"
               "@font-face { font-family: \"iA Writer Mono S\"; font-weight: 700; font-style: italic;\n"
               "  src: url(\"iAWriterMonoS-BoldItalic.ttf\") format(\"truetype\"); }\n"
               "html { background: %2; color: %3; }\n"
               "body { margin: 5vh auto 8rem; max-width: 65ch; padding: 0 24px;\n"
               "       font-family: \"iA Writer Mono S\", ui-monospace, monospace;\n"
               "       font-size: %6px; font-weight: 400; line-height: 1.4; }\n"
               "h1, h2, h3, h4, h5, h6 { font-size: 1em; font-weight: 700;\n"
               "       margin: 0 0 0.4em; color: inherit; }\n"
               "p, ul, ol { margin: 0 0 0.8em; }\n"
               "a { color: %4; text-decoration: underline; }\n"
               "blockquote { color: %7; font-style: italic; margin: 0 0 0.8em; }\n"
               "pre, code { background: %5; color: inherit; font-family: inherit; }\n"
               "code { padding: 0.12em 0.35em; }\n"
               "pre { padding: 0.6em 0.8em; overflow: auto; }\n"
               "pre code { background: transparent; padding: 0; }\n"
               "img { max-width: 100%; }\n"
               "</style>\n"
               "</head><body>\n"
               "%8\n"
               "</body></html>\n")
        .arg(title.toHtmlEscaped(), pageBackground, pageForeground, pageAccent,
             codeBackground, QString::number(fontPixelSize), muted, body);
}
