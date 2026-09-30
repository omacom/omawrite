#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent) : QObject(parent) {
    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash in window 2 is still recovered even if window 1 exited normally.
    for (int pass = 0; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("recovery-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_recoveryPath = base + QStringLiteral(".json");
                m_recoveryLock = std::move(lock);
                break;
            }
        }
    }
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::writeRecovery);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                if (path != m_fileUrl.toLocalFile())
                    return;

                const bool deleted = !QFileInfo::exists(path);
                if (!deleted && m_hasKnownFileContents) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly)
                            && file.readAll() == m_lastKnownFileContents) {
                        // Atomic saves can replace the watched inode. Re-arm the
                        // watcher, but do not report our own save as an outside edit.
                        watchCurrentFile();
                        return;
                    }
                }

                emit externalChangeDetected(deleted, m_modified);
            });

    loadOmarchyTheme();
    watchOmarchyTheme();
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return QStringLiteral("Untitled.md");

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled.md") : name;
}

void Backend::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    loadOmarchyTheme();
    emit darkModeChanged();
}

void Backend::setTextScale(qreal textScale) {
    if (qFuzzyCompare(m_textScale, textScale))
        return;

    m_textScale = textScale;
    emit textScaleChanged();
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setDarkMode(m_darkMode);
    m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::open(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray contents = file.readAll();
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = true;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("Opened %1").arg(fileName()));
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveForClose() {
    if (!m_modified) {
        emit closeAfterSave();
        return;
    }

    m_closeAfterSave = true;
    save();
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::fileDialogCanceled() {
    m_closeAfterSave = false;
}

void Backend::discardRecovery() {
    clearRecovery();
}

void Backend::reloadFromDisk() {
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void Backend::keepExternalVersion() {
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        m_lastKnownFileContents = file.readAll();
        m_hasKnownFileContents = true;
    } else {
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
    }
    setModified(true);
    scheduleRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

void Backend::printDocument() {
    if (!m_document) {
        setStatus(QStringLiteral("There is no document to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print %1").arg(fileName()));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);

    if (dialog.exec() == QDialog::Accepted) {
        QTextDocument rendered;
        rendered.setDefaultFont(m_document->defaultFont());
        rendered.setMarkdown(currentDocumentText());
        rendered.print(&printer);
    }
}

void Backend::newWindow() {
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 QStringList());
    if (!started)
        setStatus(QStringLiteral("Could not open a new window."));
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    if (m_loading || m_formattingTypography)
        return false;

    const QString text = currentDocumentText();
    if (text == m_lastDocumentText)
        return false;
    m_lastDocumentText = text;

    if (m_document) {
        const int blockCount = m_document->blockCount();
        if (blockCount > m_formattedBlockCount)
            reapplyTypographyToChange();
        m_formattedBlockCount = blockCount;
    }

    scheduleWordCount();
    setModified(true);
    setStatus(QStringLiteral("Unsaved"));
    scheduleRecovery();
    return true;
}

QVariantList Backend::hiddenRangesAt(int position) const {
    QVariantList ranges;
    if (!m_document)
        return ranges;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return ranges;

    const int lineStart = block.position();
    QList<QPair<int, int>> spans;
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const MarkdownHighlighter::InlineMarkup &item : markup) {
        for (const MarkdownHighlighter::Span &marker : item.markers) {
            spans.append({lineStart + marker.start,
                          lineStart + marker.start + marker.length});
        }
    }
    std::sort(spans.begin(), spans.end());

    for (const auto &span : spans) {
        ranges.append(QVariantMap{{QStringLiteral("start"), span.first},
                                  {QStringLiteral("end"), span.second}});
    }
    return ranges;
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

void Backend::openExternalUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
            || scheme == QStringLiteral("mailto"))
        QDesktopServices::openUrl(url);
}

namespace {

// Marker block property for inserted fence-language labels (skipped by the
// styling walk; their formats are final at insertion time).
constexpr int PreviewLabelProp = QTextFormat::UserProperty + 1;

// Fixed Obsidian-style token palette (dark / light variants stay readable on
// both; the surrounding chrome still follows the Omarchy theme).
QColor previewTokenColor(const QString &role, bool dark) {
    if (role == QLatin1String("keyword"))
        return QColor(dark ? QStringLiteral("#c678dd") : QStringLiteral("#a626a4"));
    if (role == QLatin1String("type"))
        return QColor(dark ? QStringLiteral("#e5c07b") : QStringLiteral("#c18401"));
    if (role == QLatin1String("string"))
        return QColor(dark ? QStringLiteral("#98c379") : QStringLiteral("#50a14f"));
    if (role == QLatin1String("number"))
        return QColor(dark ? QStringLiteral("#d19a66") : QStringLiteral("#b76b01"));
    return QColor();
}

enum class CodeLang { Generic, CLike, Python, Shell };

CodeLang codeLangClass(const QString &lang) {
    static const QSet<QString> cLike{
        QStringLiteral("java"), QStringLiteral("javascript"), QStringLiteral("js"),
        QStringLiteral("typescript"), QStringLiteral("ts"), QStringLiteral("c"),
        QStringLiteral("cpp"), QStringLiteral("c++"), QStringLiteral("cc"),
        QStringLiteral("h"), QStringLiteral("hpp"), QStringLiteral("cs"),
        QStringLiteral("csharp"), QStringLiteral("kotlin"), QStringLiteral("kt"),
        QStringLiteral("swift"), QStringLiteral("scala"), QStringLiteral("php"),
        QStringLiteral("rust"), QStringLiteral("rs"), QStringLiteral("go"),
    };
    static const QSet<QString> pyLike{QStringLiteral("python"), QStringLiteral("py")};
    static const QSet<QString> shLike{QStringLiteral("bash"), QStringLiteral("sh"),
                                      QStringLiteral("shell"), QStringLiteral("zsh")};
    if (cLike.contains(lang))
        return CodeLang::CLike;
    if (pyLike.contains(lang))
        return CodeLang::Python;
    if (shLike.contains(lang))
        return CodeLang::Shell;
    return CodeLang::Generic;
}

const QSet<QString> &codeKeywordsFor(const QString &lang) {
    static const QSet<QString> java{
        QStringLiteral("abstract"), QStringLiteral("assert"), QStringLiteral("boolean"),
        QStringLiteral("break"), QStringLiteral("byte"), QStringLiteral("case"),
        QStringLiteral("catch"), QStringLiteral("char"), QStringLiteral("class"),
        QStringLiteral("const"), QStringLiteral("continue"), QStringLiteral("default"),
        QStringLiteral("do"), QStringLiteral("double"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("extends"), QStringLiteral("final"),
        QStringLiteral("finally"), QStringLiteral("float"), QStringLiteral("for"),
        QStringLiteral("if"), QStringLiteral("implements"), QStringLiteral("import"),
        QStringLiteral("instanceof"), QStringLiteral("int"), QStringLiteral("interface"),
        QStringLiteral("long"), QStringLiteral("native"), QStringLiteral("new"),
        QStringLiteral("package"), QStringLiteral("private"), QStringLiteral("protected"),
        QStringLiteral("public"), QStringLiteral("return"), QStringLiteral("short"),
        QStringLiteral("static"), QStringLiteral("super"), QStringLiteral("switch"),
        QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("throws"),
        QStringLiteral("try"), QStringLiteral("void"), QStringLiteral("volatile"),
        QStringLiteral("while"), QStringLiteral("var"), QStringLiteral("record"),
        QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null"),
    };
    static const QSet<QString> python{
        QStringLiteral("False"), QStringLiteral("None"), QStringLiteral("True"),
        QStringLiteral("and"), QStringLiteral("as"), QStringLiteral("assert"),
        QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("break"),
        QStringLiteral("class"), QStringLiteral("continue"), QStringLiteral("def"),
        QStringLiteral("del"), QStringLiteral("elif"), QStringLiteral("else"),
        QStringLiteral("except"), QStringLiteral("finally"), QStringLiteral("for"),
        QStringLiteral("from"), QStringLiteral("global"), QStringLiteral("if"),
        QStringLiteral("import"), QStringLiteral("in"), QStringLiteral("is"),
        QStringLiteral("lambda"), QStringLiteral("nonlocal"), QStringLiteral("not"),
        QStringLiteral("or"), QStringLiteral("pass"), QStringLiteral("raise"),
        QStringLiteral("return"), QStringLiteral("try"), QStringLiteral("while"),
        QStringLiteral("with"), QStringLiteral("yield"),
    };
    static const QSet<QString> javascript{
        QStringLiteral("abstract"), QStringLiteral("any"), QStringLiteral("as"),
        QStringLiteral("async"), QStringLiteral("await"), QStringLiteral("boolean"),
        QStringLiteral("break"), QStringLiteral("case"), QStringLiteral("catch"),
        QStringLiteral("class"), QStringLiteral("const"), QStringLiteral("continue"),
        QStringLiteral("debugger"), QStringLiteral("declare"), QStringLiteral("default"),
        QStringLiteral("delete"), QStringLiteral("do"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("export"), QStringLiteral("extends"),
        QStringLiteral("false"), QStringLiteral("finally"), QStringLiteral("for"),
        QStringLiteral("from"), QStringLiteral("function"), QStringLiteral("get"),
        QStringLiteral("if"), QStringLiteral("implements"), QStringLiteral("import"),
        QStringLiteral("in"), QStringLiteral("instanceof"), QStringLiteral("interface"),
        QStringLiteral("let"), QStringLiteral("new"), QStringLiteral("null"),
        QStringLiteral("number"), QStringLiteral("private"), QStringLiteral("protected"),
        QStringLiteral("public"), QStringLiteral("return"), QStringLiteral("set"),
        QStringLiteral("static"), QStringLiteral("string"), QStringLiteral("super"),
        QStringLiteral("switch"), QStringLiteral("this"), QStringLiteral("throw"),
        QStringLiteral("true"), QStringLiteral("try"), QStringLiteral("type"),
        QStringLiteral("typeof"), QStringLiteral("undefined"), QStringLiteral("var"),
        QStringLiteral("void"), QStringLiteral("while"), QStringLiteral("with"),
        QStringLiteral("yield"),
    };
    static const QSet<QString> cpp{
        QStringLiteral("alignas"), QStringLiteral("auto"), QStringLiteral("bool"),
        QStringLiteral("break"), QStringLiteral("case"), QStringLiteral("catch"),
        QStringLiteral("char"), QStringLiteral("class"), QStringLiteral("const"),
        QStringLiteral("consteval"), QStringLiteral("constexpr"), QStringLiteral("continue"),
        QStringLiteral("decltype"), QStringLiteral("default"), QStringLiteral("delete"),
        QStringLiteral("do"), QStringLiteral("double"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("explicit"), QStringLiteral("extern"),
        QStringLiteral("false"), QStringLiteral("float"), QStringLiteral("for"),
        QStringLiteral("friend"), QStringLiteral("goto"), QStringLiteral("if"),
        QStringLiteral("inline"), QStringLiteral("int"), QStringLiteral("long"),
        QStringLiteral("namespace"), QStringLiteral("new"), QStringLiteral("noexcept"),
        QStringLiteral("nullptr"), QStringLiteral("operator"), QStringLiteral("private"),
        QStringLiteral("protected"), QStringLiteral("public"), QStringLiteral("return"),
        QStringLiteral("short"), QStringLiteral("signed"), QStringLiteral("sizeof"),
        QStringLiteral("static"), QStringLiteral("struct"), QStringLiteral("switch"),
        QStringLiteral("template"), QStringLiteral("this"), QStringLiteral("throw"),
        QStringLiteral("true"), QStringLiteral("try"), QStringLiteral("typedef"),
        QStringLiteral("typename"), QStringLiteral("union"), QStringLiteral("unsigned"),
        QStringLiteral("using"), QStringLiteral("virtual"), QStringLiteral("void"),
        QStringLiteral("volatile"), QStringLiteral("while"),
    };
    static const QSet<QString> cLang{
        QStringLiteral("auto"), QStringLiteral("break"), QStringLiteral("case"),
        QStringLiteral("char"), QStringLiteral("const"), QStringLiteral("continue"),
        QStringLiteral("default"), QStringLiteral("do"), QStringLiteral("double"),
        QStringLiteral("else"), QStringLiteral("enum"), QStringLiteral("extern"),
        QStringLiteral("float"), QStringLiteral("for"), QStringLiteral("goto"),
        QStringLiteral("if"), QStringLiteral("inline"), QStringLiteral("int"),
        QStringLiteral("long"), QStringLiteral("register"), QStringLiteral("restrict"),
        QStringLiteral("return"), QStringLiteral("short"), QStringLiteral("signed"),
        QStringLiteral("sizeof"), QStringLiteral("static"), QStringLiteral("struct"),
        QStringLiteral("switch"), QStringLiteral("typedef"), QStringLiteral("union"),
        QStringLiteral("unsigned"), QStringLiteral("void"), QStringLiteral("volatile"),
        QStringLiteral("while"),
    };
    static const QSet<QString> rust{
        QStringLiteral("as"), QStringLiteral("break"), QStringLiteral("const"),
        QStringLiteral("continue"), QStringLiteral("crate"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("extern"), QStringLiteral("false"),
        QStringLiteral("fn"), QStringLiteral("for"), QStringLiteral("if"),
        QStringLiteral("impl"), QStringLiteral("in"), QStringLiteral("let"),
        QStringLiteral("loop"), QStringLiteral("match"), QStringLiteral("mod"),
        QStringLiteral("move"), QStringLiteral("mut"), QStringLiteral("pub"),
        QStringLiteral("ref"), QStringLiteral("return"), QStringLiteral("self"),
        QStringLiteral("Self"), QStringLiteral("static"), QStringLiteral("struct"),
        QStringLiteral("super"), QStringLiteral("trait"), QStringLiteral("true"),
        QStringLiteral("type"), QStringLiteral("unsafe"), QStringLiteral("use"),
        QStringLiteral("where"), QStringLiteral("while"), QStringLiteral("async"),
        QStringLiteral("await"), QStringLiteral("dyn"),
    };
    static const QSet<QString> go{
        QStringLiteral("break"), QStringLiteral("case"), QStringLiteral("chan"),
        QStringLiteral("const"), QStringLiteral("continue"), QStringLiteral("default"),
        QStringLiteral("defer"), QStringLiteral("else"), QStringLiteral("fallthrough"),
        QStringLiteral("for"), QStringLiteral("func"), QStringLiteral("go"),
        QStringLiteral("goto"), QStringLiteral("if"), QStringLiteral("import"),
        QStringLiteral("interface"), QStringLiteral("map"), QStringLiteral("package"),
        QStringLiteral("range"), QStringLiteral("return"), QStringLiteral("select"),
        QStringLiteral("struct"), QStringLiteral("switch"), QStringLiteral("type"),
        QStringLiteral("var"), QStringLiteral("true"), QStringLiteral("false"),
        QStringLiteral("nil"),
    };
    static const QSet<QString> shell{
        QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"),
        QStringLiteral("elif"), QStringLiteral("fi"), QStringLiteral("for"),
        QStringLiteral("while"), QStringLiteral("until"), QStringLiteral("do"),
        QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
        QStringLiteral("in"), QStringLiteral("function"), QStringLiteral("select"),
        QStringLiteral("return"), QStringLiteral("exit"), QStringLiteral("break"),
        QStringLiteral("continue"), QStringLiteral("export"), QStringLiteral("local"),
        QStringLiteral("readonly"), QStringLiteral("unset"), QStringLiteral("true"),
        QStringLiteral("false"),
    };
    static const QSet<QString> empty;
    if (lang == QLatin1String("python") || lang == QLatin1String("py"))
        return python;
    if (lang == QLatin1String("bash") || lang == QLatin1String("sh")
            || lang == QLatin1String("shell") || lang == QLatin1String("zsh"))
        return shell;
    if (lang == QLatin1String("c") || lang == QLatin1String("h"))
        return cLang;
    if (lang == QLatin1String("cpp") || lang == QLatin1String("c++")
            || lang == QLatin1String("cc") || lang == QLatin1String("hpp"))
        return cpp;
    if (lang == QLatin1String("javascript") || lang == QLatin1String("js")
            || lang == QLatin1String("typescript") || lang == QLatin1String("ts"))
        return javascript;
    if (lang == QLatin1String("rust") || lang == QLatin1String("rs"))
        return rust;
    if (lang == QLatin1String("go"))
        return go;
    // Java-like languages share the Java set; unknown languages get generic
    // treatment (comments/strings/numbers only).
    if (codeLangClass(lang) == CodeLang::CLike)
        return java;
    return empty;
}

enum class TokRole { Plain, Keyword, Type, String, Number, Comment };

struct CodeToken {
    int start;
    int len;
    TokRole role;
};

// Single linear scan per code line — no backtracking blowups. Block comments
// (/* */) and Python triple-quoted strings carry across lines via state.
QList<CodeToken> tokenizeCodeLine(const QString &line, CodeLang cls,
                                  const QSet<QString> &keywords, bool &inBlock,
                                  QChar &inTriple) {
    QList<CodeToken> toks;
    const int n = line.size();
    int i = 0;
    int plain = 0;
    auto push = [&](int s, int e, TokRole r) {
        if (e > s)
            toks.append({s, e - s, r});
    };
    auto flushPlain = [&](int to) {
        push(plain, to, TokRole::Plain);
        plain = to;
    };

    if (inBlock) {
        const int e = line.indexOf(QStringLiteral("*/"), i);
        if (e < 0) {
            push(0, n, TokRole::Comment);
            return toks;
        }
        push(0, e + 2, TokRole::Comment);
        i = e + 2;
        plain = i;
        inBlock = false;
    }
    if (!inTriple.isNull()) {
        const QString q(3, inTriple);
        const int e = line.indexOf(q, i);
        if (e < 0) {
            push(i, n, TokRole::String);
            return toks;
        }
        push(i, e + 3, TokRole::String);
        i = e + 3;
        plain = i;
        inTriple = QChar();
    }

    const bool cLike = cls == CodeLang::CLike;
    const bool py = cls == CodeLang::Python;
    const bool sh = cls == CodeLang::Shell;
    const bool hashComment = py || sh || cls == CodeLang::Generic;
    const bool slashComment = cLike || cls == CodeLang::Generic;
    const bool wantTypes = cLike || py;
    static const QRegularExpression numRe(
        QStringLiteral("^(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\\d[\\d_]*(?:\\.\\d+)?(?:[eE][+-]?\\d+)?[uUlLfF]*)"));

    while (i < n) {
        const QChar c = line[i];
        if (slashComment && c == QLatin1Char('/') && i + 1 < n && line[i + 1] == QLatin1Char('/')) {
            flushPlain(i);
            push(i, n, TokRole::Comment);
            return toks;
        }
        if (hashComment && c == QLatin1Char('#')) {
            flushPlain(i);
            push(i, n, TokRole::Comment);
            return toks;
        }
        if (cLike && c == QLatin1Char('/') && i + 1 < n && line[i + 1] == QLatin1Char('*')) {
            const int e = line.indexOf(QStringLiteral("*/"), i + 2);
            flushPlain(i);
            if (e < 0) {
                push(i, n, TokRole::Comment);
                inBlock = true;
                return toks;
            }
            push(i, e + 2, TokRole::Comment);
            i = e + 2;
            plain = i;
            continue;
        }
        if (py && (c == QLatin1Char('"') || c == QLatin1Char('\'')) && i + 2 < n
                && line[i + 1] == c && line[i + 2] == c) {
            const QString q(3, c);
            const int e = line.indexOf(q, i + 3);
            flushPlain(i);
            if (e < 0) {
                push(i, n, TokRole::String);
                inTriple = c;
                return toks;
            }
            push(i, e + 3, TokRole::String);
            i = e + 3;
            plain = i;
            continue;
        }
        if (c == QLatin1Char('"')) {
            int j = i + 1;
            while (j < n && line[j] != QLatin1Char('"')) {
                if (line[j] == QLatin1Char('\\'))
                    ++j;
                ++j;
            }
            flushPlain(i);
            push(i, j < n ? j + 1 : n, TokRole::String);
            i = j < n ? j + 1 : n;
            plain = i;
            continue;
        }
        if (c == QLatin1Char('\'')) {
            int j = i + 1;
            while (j < n && line[j] != QLatin1Char('\'')) {
                if (line[j] == QLatin1Char('\\'))
                    ++j;
                ++j;
            }
            // A lone apostrophe (don't) is plain text, except in shell where
            // it opens a (possibly multi-line) string.
            if (j < n || sh) {
                flushPlain(i);
                push(i, j < n ? j + 1 : n, TokRole::String);
                i = j < n ? j + 1 : n;
                plain = i;
                continue;
            }
            ++i;
            continue;
        }
        if ((cLike || py) && c == QLatin1Char('@') && i + 1 < n
                && (line[i + 1].isLetterOrNumber() || line[i + 1] == QLatin1Char('_'))) {
            int j = i + 1;
            while (j < n && (line[j].isLetterOrNumber() || line[j] == QLatin1Char('_')))
                ++j;
            flushPlain(i);
            push(i, j, TokRole::Keyword);
            i = j;
            plain = i;
            continue;
        }
        if (sh && c == QLatin1Char('$') && i + 1 < n
                && (line[i + 1].isLetter() || line[i + 1] == QLatin1Char('_')
                    || line[i + 1] == QLatin1Char('{'))) {
            int j = i + 1;
            if (line[j] == QLatin1Char('{')) {
                const int e = line.indexOf(QLatin1Char('}'), j);
                j = e < 0 ? n : e + 1;
            } else {
                while (j < n && (line[j].isLetterOrNumber() || line[j] == QLatin1Char('_')))
                    ++j;
            }
            flushPlain(i);
            push(i, j, TokRole::Type);
            i = j;
            plain = i;
            continue;
        }
        if (c.isDigit()) {
            const QRegularExpressionMatch m = numRe.match(line, i);
            if (m.hasMatch() && m.capturedStart() == i) {
                flushPlain(i);
                push(i, m.capturedEnd(), TokRole::Number);
                i = m.capturedEnd();
                plain = i;
                continue;
            }
            ++i;
            continue;
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < n && (line[j].isLetterOrNumber() || line[j] == QLatin1Char('_')))
                ++j;
            const QString w = line.mid(i, j - i);
            flushPlain(i);
            if (!keywords.isEmpty() && keywords.contains(w))
                push(i, j, TokRole::Keyword);
            else if (wantTypes && w.at(0).isUpper())
                push(i, j, TokRole::Type);
            else
                push(i, j, TokRole::Plain);
            i = j;
            plain = i;
            continue;
        }
        ++i;
    }
    flushPlain(n);
    return toks;
}

} // namespace

QString Backend::markdownPreview(const QString &markdown) {
    const QString themeKey = m_themeBackground + QLatin1Char('|')
        + m_themeForeground + QLatin1Char('|') + m_themeAccent + QLatin1Char('|')
        + m_themeSelection + QLatin1Char('|') + (m_darkMode ? QLatin1Char('D') : QLatin1Char('L'))
        + QLatin1Char('|') + QString::number(m_textScale, 'f', 3);
    if (markdown == m_previewInput && themeKey == m_previewThemeKey
            && !m_previewHtml.isNull())
        return m_previewHtml;

    if (markdown.trimmed().isEmpty()) {
        m_previewInput = markdown;
        m_previewThemeKey = themeKey;
        m_previewHtml = {};
        return m_previewHtml;
    }

    // Resolve relative image destinations against the open file's directory so
    // `![alt](img.png)` loads in the preview. Absolute URLs, absolute paths,
    // anchors, and data: URIs are left untouched.
    QString resolved = markdown;
    if (m_fileUrl.isLocalFile()) {
        const QDir baseDir(QFileInfo(m_fileUrl.toLocalFile()).absolutePath());
        static const QRegularExpression imageRe(
            QStringLiteral("!\\[([^\\]]*)\\]\\(([^)\\s]+)(\\s+\"[^\"]*\")?\\)"));
        auto it = imageRe.globalMatch(resolved);
        QString out;
        int last = 0;
        while (it.hasNext()) {
            const auto match = it.next();
            const QString dest = match.captured(2);
            const bool absolute = dest.startsWith(QStringLiteral("http://"))
                || dest.startsWith(QStringLiteral("https://"))
                || dest.startsWith(QStringLiteral("data:"))
                || dest.startsWith(QStringLiteral("#"))
                || dest.startsWith(QStringLiteral("/"))
                || dest.startsWith(QStringLiteral("file:"))
                || QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:")).match(dest).hasMatch();
            if (absolute)
                continue;
            const QString absPath = baseDir.absoluteFilePath(dest);
            out += resolved.mid(last, match.capturedStart(2) - last);
            out += QUrl::fromLocalFile(absPath).toString();
            last = match.capturedStart(2) + match.capturedLength(2);
        }
        if (!out.isEmpty()) {
            out += resolved.mid(last);
            resolved = out;
        }
    }

    const QString fg = m_themeForeground;
    const QString accent = m_themeAccent;
    const QString muted = m_darkMode ? QStringLiteral("#909191") : QStringLiteral("#aeb1b5");
    const QString codeBg = m_darkMode ? QStringLiteral("#1c1a1a") : QStringLiteral("#f8f8f8");
    const int base = qMax(1, qRound(20.0 * m_textScale));

    // Qt's Markdown importer silently drops prose lines starting with `<Tag`
    // (HTML blocks) — e.g. Java `<T> void sort(...)` notes — swallowing them
    // plus following non-blank lines. Escape those openers so they render
    // literally instead of vanishing. Verified kept as-is, hence skipped:
    // fenced code, 4-space/tab-indented code, autolinks, <!-- / <? markup.
    {
        static const QRegularExpression autoLink(
            QStringLiteral("^<[A-Za-z][A-Za-z0-9+.-]*:[^\\s<>]*>$|^<[^\\s<>@]*@[^\\s<>]*>$"));
        QStringList split = resolved.split(QLatin1Char('\n'));
        bool changed = false;
        bool inFence = false;
        QChar fenceMark;
        for (int li = 0; li < split.size(); ++li) {
            const QString ln = split.at(li);
            int indent = 0;
            while (indent < ln.size() && ln.at(indent) == QLatin1Char(' '))
                ++indent;
            if (indent >= ln.size() || indent >= 4 || ln.at(indent) == QLatin1Char('\t'))
                continue; // blank or indented code: never a fence, never guarded
            const int p = indent;
            // Fence tracking (``` / ~~~, info string allowed on openers).
            if (!inFence && p + 2 < ln.size()
                    && (ln.at(p) == QLatin1Char('`') || ln.at(p) == QLatin1Char('~'))
                    && ln.at(p + 1) == ln.at(p) && ln.at(p + 2) == ln.at(p)) {
                inFence = true;
                fenceMark = ln.at(p);
                continue;
            }
            if (inFence) {
                int q = p;
                int run = 0;
                while (q + run < ln.size() && ln.at(q + run) == fenceMark)
                    ++run;
                if (run >= 3) {
                    bool closer = true;
                    for (int r = q + run; r < ln.size(); ++r) {
                        if (!ln.at(r).isSpace()) {
                            closer = false;
                            break;
                        }
                    }
                    if (closer)
                        inFence = false;
                }
                continue;
            }
            // Optional blockquote/list prefixes before the opener.
            int s = p;
            for (int depth = 0; depth < 5; ++depth) {
                if (s < ln.size() && ln.at(s) == QLatin1Char('>')) {
                    ++s;
                    if (s < ln.size() && ln.at(s) == QLatin1Char(' '))
                        ++s;
                } else if (s + 1 < ln.size()
                           && (ln.at(s) == QLatin1Char('-') || ln.at(s) == QLatin1Char('+')
                               || ln.at(s) == QLatin1Char('*'))
                           && ln.at(s + 1) == QLatin1Char(' ')) {
                    s += 2;
                } else if (s < ln.size() && ln.at(s).isDigit()) {
                    int t = s;
                    while (t < ln.size() && ln.at(t).isDigit())
                        ++t;
                    if (t < ln.size()
                            && (ln.at(t) == QLatin1Char('.') || ln.at(t) == QLatin1Char(')'))
                            && t + 1 < ln.size() && ln.at(t + 1) == QLatin1Char(' ')) {
                        s = t + 2;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
            if (s < ln.size() && ln.at(s) == QLatin1Char('<') && s + 1 < ln.size()) {
                const QChar nx = ln.at(s + 1);
                if (nx.isLetter() || nx == QLatin1Char('/')) {
                    const int gt = ln.indexOf(QLatin1Char('>'), s);
                    const QString candidate = gt > s ? ln.mid(s, gt - s + 1) : QString();
                    if (!autoLink.match(candidate).hasMatch()) {
                        split[li] = ln.left(s) + QStringLiteral("&lt;") + ln.mid(s + 1);
                        changed = true;
                    }
                }
            }
        }
        if (changed)
            resolved = split.join(QLatin1Char('\n'));
    }

    QTextDocument doc;
    QFont bodyFont(QStringLiteral("Serif"));
    bodyFont.setPixelSize(base);
    doc.setDefaultFont(bodyFont);
    // NOTE: setDefaultStyleSheet() is intentionally NOT used: the Markdown
    // importer builds char formats directly and ignores it, and takes link
    // colors from the app palette. Theme the document directly by walking its
    // fragments once instead — O(n), and deliberately no per-language code
    // highlighting (one uniform code background).
    doc.setMarkdown(resolved, QTextDocument::MarkdownDialectGitHub);

    // Same palette as the editor (bg/fg/accent from colors.toml), but an
    // Obsidian-like reading rhythm: serif body, sans headings, mono code,
    // airy paragraphs, IDE-highlighted fenced code in bordered blocks.
    struct PreviewEdit {
        int pos;
        int len;
        QTextCharFormat format;
    };
    struct PreviewBlock {
        int pos;
        QTextBlockFormat format;
    };

    auto blockCodeLang = [](const QTextBlock &b) {
        return b.blockFormat().property(QTextFormat::BlockCodeLanguage)
            .toString().trimmed().toLower().section(QLatin1Char(' '), 0, 0);
    };
    auto blockIsCode = [](const QTextBlock &b) {
        if (!b.isValid() || b.text().isEmpty())
            return false;
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid() || f.length() <= 0)
                continue;
            const QStringList fams = f.charFormat().fontFamilies().toStringList();
            if (fams.isEmpty()
                    || fams.first().compare(QStringLiteral("monospace"),
                                            Qt::CaseInsensitive) != 0)
                return false;
        }
        return true;
    };

    // Group fenced code lines into regions (blank lines don't split one) so
    // each ```lang fence gets one language tag. Adjacent fences carry no
    // separator blocks, so a language change also starts a new region.
    struct CodeRegion {
        int firstPos;
        QString lang;
    };
    QList<CodeRegion> codeRegions;
    {
        bool inRegion = false;
        int regionFirst = -1;
        QString regionLang;
        for (QTextBlock block = doc.begin(); block != doc.end(); block = block.next()) {
            const bool code = blockIsCode(block);
            if (code) {
                const QString own = blockCodeLang(block);
                if (!inRegion) {
                    inRegion = true;
                    regionFirst = block.position();
                    regionLang = own;
                } else if (!own.isEmpty() && own != regionLang) {
                    if (!regionLang.isEmpty())
                        codeRegions.append({regionFirst, regionLang});
                    regionFirst = block.position();
                    regionLang = own;
                }
            } else if (inRegion && block.text().isEmpty()) {
                continue; // blank bridge line inside a fence
            } else if (inRegion) {
                inRegion = false;
                if (!regionLang.isEmpty())
                    codeRegions.append({regionFirst, regionLang});
            }
        }
        if (inRegion && !regionLang.isEmpty())
            codeRegions.append({regionFirst, regionLang});
    }

    // Insert a small muted language tag above each labeled region, back to
    // front so earlier positions stay valid. The tag block is marked and
    // skipped by the styling walk below (its formats are final here).
    {
        const QStringList sansFams{QStringLiteral("Adwaita Sans"),
                                   QStringLiteral("Noto Sans"),
                                   QStringLiteral("sans-serif")};
        const int labelPx = qMax(1, qRound(20.0 * m_textScale * 0.7));
        const int labelGap = qMax(0, qRound(2.0 * m_textScale));
        QTextCursor cur(&doc);
        for (int ri = codeRegions.size() - 1; ri >= 0; --ri) {
            const int at = codeRegions.at(ri).firstPos;
            const QString tag = codeRegions.at(ri).lang;
            cur.setPosition(at);
            cur.insertText(tag + QLatin1String("\n"));
            cur.setPosition(at);
            cur.setPosition(at + tag.size(), QTextCursor::KeepAnchor);
            QTextCharFormat lf;
            lf.setForeground(QColor(muted));
            lf.setFontFamilies(sansFams);
            QFont lfont;
            lfont.setPixelSize(labelPx);
            lf.setFont(lfont);
            cur.setCharFormat(lf);
            // Fresh format (SET, not merge): the split inherits the code
            // block's <pre> marker, which would otherwise swallow the label
            // into the bordered table run below.
            cur.setPosition(at + tag.size());
            QTextBlockFormat lbf;
            lbf.setProperty(PreviewLabelProp, true);
            lbf.setBottomMargin(labelGap);
            cur.setBlockFormat(lbf);
        }
    }

    QList<PreviewEdit> previewEdits;
    QList<PreviewBlock> previewBlocks;
    const int paraBottom = qMax(0, qRound(10.0 * m_textScale));
    const int codePad = qMax(0, qRound(12.0 * m_textScale));
    // Widest code line in px, for horizontal scrolling (tabs measured wide).
    QFont measureFont(QStringLiteral("iA Writer Mono S"));
    measureFont.setPixelSize(qMax(1, qRound(20.0 * m_textScale * 0.8)));
    const QFontMetricsF measure(measureFont);
    qreal maxCodePx = 0;
    // Tokenizer state, carried across lines within one code region.
    QString tokLang;
    CodeLang tokCls = CodeLang::Generic;
    static const QSet<QString> noKeywords;
    const QSet<QString> *tokKw = &noKeywords;
    bool tokBlock = false;
    QChar tokTriple;
    for (QTextBlock block = doc.begin(); block != doc.end(); block = block.next()) {
        if (block.blockFormat().property(PreviewLabelProp).isValid()) {
            tokLang.clear();
            tokCls = CodeLang::Generic;
            tokKw = &noKeywords;
            tokBlock = false;
            tokTriple = QChar();
            continue;
        }
        const int heading = block.blockFormat().headingLevel();
        // Blockquotes arrive as double-indented paragraphs (40px each side).
        const bool quote = block.blockFormat().leftMargin() >= 30
            && block.blockFormat().rightMargin() >= 30;
        const bool listItem = block.textList() != nullptr;
        const bool empty = block.text().isEmpty();

        bool allMono = !empty;
        const QString lineLang = blockCodeLang(block);
        qreal codeLineWidth = 0;
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || frag.length() <= 0)
                continue;
            QTextCharFormat fmt = frag.charFormat();
            bool change = false;
            if (fmt.isAnchor()) {
                fmt.setForeground(QColor(accent));
                fmt.setFontUnderline(true);
                change = true;
            }
            // fontFamily() asserts on formats without families; read the list.
            const QStringList families = fmt.fontFamilies().toStringList();
            const bool mono = !families.isEmpty()
                && families.first().compare(QStringLiteral("monospace"),
                                            Qt::CaseInsensitive) == 0;
            if (!mono)
                allMono = false;
            if (mono) {
                fmt.setFontFamilies(QStringList{QStringLiteral("iA Writer Mono S"),
                                                QStringLiteral("Adwaita Mono"),
                                                QStringLiteral("monospace")});
                // Clearly smaller than prose (16px vs 20px at scale 1).
                // NB: pixel size via QFont (serializes as px); setFontPointSize
                // would come back as larger pt units.
                QFont codeFont = fmt.font();
                codeFont.setPixelSize(qMax(1, qRound(20.0 * m_textScale * 0.8)));
                fmt.setFont(codeFont);
                fmt.setBackground(QColor(codeBg));
                change = true;
                codeLineWidth += measure.horizontalAdvance(
                    frag.text().replace(QLatin1Char('\t'), QStringLiteral("        ")));
                if (!fmt.isAnchor() && heading == 0) {
                    // IDE-style token colors. Base format above carries no
                    // foreground, so these merge cleanly over it. A language
                    // change means a new fence: refresh and reset state.
                    if (lineLang != tokLang) {
                        tokLang = lineLang;
                        tokCls = codeLangClass(lineLang);
                        tokKw = &codeKeywordsFor(lineLang);
                        tokBlock = false;
                        tokTriple = QChar();
                    }
                    const QList<CodeToken> toks = tokenizeCodeLine(
                        frag.text(), tokCls, *tokKw, tokBlock, tokTriple);
                    for (const CodeToken &tok : toks) {
                        if (tok.role == TokRole::Plain)
                            continue;
                        QTextCharFormat tf;
                        if (tok.role == TokRole::Comment) {
                            tf.setForeground(QColor(muted));
                            tf.setFontItalic(true);
                        } else if (tok.role == TokRole::Keyword) {
                            tf.setForeground(previewTokenColor(QStringLiteral("keyword"), m_darkMode));
                        } else if (tok.role == TokRole::Type) {
                            tf.setForeground(previewTokenColor(QStringLiteral("type"), m_darkMode));
                        } else if (tok.role == TokRole::String) {
                            tf.setForeground(previewTokenColor(QStringLiteral("string"), m_darkMode));
                        } else if (tok.role == TokRole::Number) {
                            tf.setForeground(previewTokenColor(QStringLiteral("number"), m_darkMode));
                        }
                        previewEdits.append(
                            {frag.position() + tok.start, tok.len, tf});
                    }
                }
            } else {
                if (heading >= 1 && heading <= 6) {
                    fmt.setFontFamilies(QStringList{QStringLiteral("Adwaita Sans"),
                                                    QStringLiteral("Noto Sans"),
                                                    QStringLiteral("sans-serif")});
                    fmt.setForeground(QColor(fg));
                    change = true;
                }
                if (quote && !fmt.isAnchor()) {
                    fmt.setForeground(QColor(muted));
                    fmt.setFontItalic(true);
                    change = true;
                }
            }
            if (change)
                previewEdits.append({frag.position(), frag.length(), fmt});
        }

        // Empty fenced lines carry no fragments; bridge them so the code
        // region stays visually continuous. Headings win over code (a heading
        // made only of `backticks` is still a heading).
        auto neighborIsCode = [](QTextBlock b) {
            if (!b.isValid() || b.text().isEmpty()
                    || b.blockFormat().headingLevel() > 0)
                return false;
            for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (!f.isValid() || f.length() <= 0)
                    continue;
                const QStringList fams = f.charFormat().fontFamilies().toStringList();
                if (fams.isEmpty()
                        || fams.first().compare(QStringLiteral("monospace"),
                                                Qt::CaseInsensitive) != 0)
                    return false;
            }
            return true;
        };
        const bool codeLine = heading == 0
            && (allMono
                || (empty && neighborIsCode(block.previous())
                    && neighborIsCode(block.next())));
        if (!codeLine && !empty) {
            tokLang.clear();
            tokCls = CodeLang::Generic;
            tokKw = &noKeywords;
            tokBlock = false;
            tokTriple = QChar();
        }
        if (codeLine)
            maxCodePx = qMax(maxCodePx, codeLineWidth);

        // Block-level rhythm (margins/line-height survive the HTML round-trip).
        QTextBlockFormat bf = block.blockFormat();
        bool blockChange = false;
        if (codeLine) {
            // Uniform full-bleed region: no gaps inside, padding at the edges.
            // NB: keep default line height here — a custom one leaves
            // unpainted leading gaps between the background bars.
            bf.setBackground(QColor(codeBg));
            bf.setTopMargin(neighborIsCode(block.previous()) ? 0 : codePad);
            bf.setBottomMargin(neighborIsCode(block.next()) ? 0 : codePad);
            blockChange = true;
        } else if (heading >= 1 && heading <= 6) {
            const int top = heading == 1 ? 26 : heading == 2 ? 22 : 18;
            bf.setTopMargin(qMax(0, qRound(top * m_textScale)));
            bf.setBottomMargin(qMax(0, qRound(8.0 * m_textScale)));
            bf.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
            blockChange = true;
        } else if (quote) {
            bf.setLineHeight(155, QTextBlockFormat::ProportionalHeight);
            blockChange = true;
        } else if (!empty && !listItem) {
            bf.setLineHeight(160, QTextBlockFormat::ProportionalHeight);
            bf.setBottomMargin(paraBottom);
            blockChange = true;
        }
        if (blockChange)
            previewBlocks.append({block.position(), bf});
    }
    if (!previewEdits.isEmpty() || !previewBlocks.isEmpty()) {
        QTextCursor cursor(&doc);
        cursor.beginEditBlock();
        // Fragments back-to-front so earlier positions stay valid; block
        // formats carry no text so order is irrelevant.
        for (int i = previewEdits.size() - 1; i >= 0; --i) {
            cursor.setPosition(previewEdits.at(i).pos);
            cursor.setPosition(previewEdits.at(i).pos + previewEdits.at(i).len,
                               QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(previewEdits.at(i).format);
        }
        for (const PreviewBlock &styled : std::as_const(previewBlocks)) {
            cursor.setPosition(styled.pos);
            cursor.mergeBlockFormat(styled.format);
        }
        cursor.endEditBlock();
    }

    QString html = doc.toHtml();
    // Self-contained body color (fragments without explicit color inherit it).
    html.replace(QStringLiteral("<body style=\""),
                 QStringLiteral("<body style=\" color:") + fg + QLatin1Char(';'));
    // Drop the qrichtext round-trip marker: in that mode the importer loses
    // <pre> non-wrapping, so long code lines wrap mid-statement. As generic
    // HTML, <pre> stays non-breakable and code scrolls horizontally instead.
    // (Inline margins/colors/spans are unaffected by the mode.)
    // NB: keep one <pre> per code line as emitted — joining them with raw
    // newlines smears the span background full-width on continuation lines.
    html.remove(QStringLiteral("<meta name=\"qrichtext\" content=\"1\" />"));
    // Bordered snippet container, Obsidian-style: wrap each maximal run of
    // consecutive <pre> lines in a single-cell table (border + inner padding
    // survive the import; the language tag stays outside, above it).
    // Manual scan, no regex backtracking hazards.
    {
        const QString tableOpen = QStringLiteral(
            "<table border=\"1\" cellpadding=\"10\" cellspacing=\"0\" bordercolor=\"")
            + muted + QStringLiteral("\"><tr><td>");
        const QString tableClose = QStringLiteral("</td></tr></table>");
        QString wrapped;
        bool found = false;
        int pos = 0;
        const int end = html.size();
        while (pos < end) {
            const int run = html.indexOf(QStringLiteral("<pre"), pos);
            if (run < 0) {
                wrapped += html.mid(pos);
                break;
            }
            int cur = run;
            int runEnd = run;
            while (cur < end) {
                const int close = html.indexOf(QStringLiteral("</pre>"), cur);
                if (close < 0) {
                    runEnd = end;
                    break;
                }
                runEnd = close + 6;
                int ns = runEnd;
                while (ns < end && html.at(ns).isSpace())
                    ++ns;
                if (html.mid(ns, 4) == QLatin1String("<pre")) {
                    cur = ns;
                    continue;
                }
                break;
            }
            wrapped += html.mid(pos, run - pos);
            wrapped += tableOpen + html.mid(run, runEnd - run) + tableClose;
            pos = runEnd;
            found = true;
        }
        if (found)
            html = wrapped;
    }

    m_previewInput = markdown;
    m_previewThemeKey = themeKey;
    m_previewHtml = html;
    const int contentPx = qCeil(maxCodePx);
    if (contentPx != m_previewContentWidth) {
        m_previewContentWidth = contentPx;
        emit previewContentWidthChanged();
    }
    return m_previewHtml;
}

namespace {

// A slice renders nothing when it holds no tags and only whitespace
// (inter-block gaps from the export); everything else is kept verbatim.
bool previewSliceAlive(const QString &html, int from, int to) {
    bool hasTag = false;
    for (int i = from; i < to; ++i) {
        const QChar c = html.at(i);
        if (c == QLatin1Char('<'))
            hasTag = true;
        else if (!c.isSpace())
            return true;
    }
    return hasTag;
}

} // namespace

QStringList Backend::splitPreviewHtml(const QString &html) const {
    QStringList chunks;
    if (html.isEmpty())
        return chunks;
    // <head> style + <body> attrs replay into every chunk so each
    // mini-document renders identically to its slice of the whole.
    QString head;
    {
        const int hs = html.indexOf(QStringLiteral("<head>"), 0, Qt::CaseInsensitive);
        const int he = html.indexOf(QStringLiteral("</head>"), 0, Qt::CaseInsensitive);
        if (hs >= 0 && he > hs)
            head = html.mid(hs + 6, he - hs - 6);
    }
    QString bodyAttrs;
    int bodyStart = -1;
    int bodyEnd = html.size();
    {
        const int bs = html.indexOf(QStringLiteral("<body"), 0, Qt::CaseInsensitive);
        if (bs >= 0) {
            const int be = html.indexOf(QLatin1Char('>'), bs);
            if (be > bs) {
                bodyAttrs = html.mid(bs + 5, be - bs - 5);
                bodyStart = be + 1;
            }
        }
        const int bend = html.indexOf(QStringLiteral("</body>"), bodyStart < 0 ? 0 : bodyStart,
                                      Qt::CaseInsensitive);
        if (bend >= 0)
            bodyEnd = bend;
    }
    if (bodyStart < 0 || bodyStart >= bodyEnd) {
        chunks.append(html);
        return chunks;
    }
    // Chunk boundaries fall on top-level block closes only; list items, table
    // cells/rows and inline spans never split (they nest inside).
    static const QSet<QString> boundary{
        QStringLiteral("p"), QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"), QStringLiteral("h5"),
        QStringLiteral("h6"), QStringLiteral("pre"), QStringLiteral("ul"),
        QStringLiteral("ol"), QStringLiteral("table"), QStringLiteral("blockquote"),
    };
    static const QSet<QString> voids{
        QStringLiteral("br"), QStringLiteral("hr"), QStringLiteral("img"),
        QStringLiteral("meta"), QStringLiteral("link"), QStringLiteral("input"),
    };
    const int end = bodyEnd;
    QStringList stack;
    int chunkStart = bodyStart;
    int pos = bodyStart;
    auto emitTo = [&](int sliceEnd) {
        if (previewSliceAlive(html, chunkStart, sliceEnd)) {
            chunks.append(QStringLiteral("<html><head>") + head
                          + QStringLiteral("</head><body") + bodyAttrs
                          + QLatin1Char('>') + html.mid(chunkStart, sliceEnd - chunkStart)
                          + QStringLiteral("</body></html>"));
        }
        chunkStart = sliceEnd;
    };
    while (pos < end) {
        const int lt = html.indexOf(QLatin1Char('<'), pos);
        if (lt < 0 || lt >= end)
            break;
        int p = lt + 1;
        bool closing = false;
        if (p < end && html.at(p) == QLatin1Char('/')) {
            closing = true;
            ++p;
        }
        int ns = p;
        while (ns < end && html.at(ns).isLetterOrNumber())
            ++ns;
        const QString name = html.mid(p, ns - p).toLower();
        const int gt = html.indexOf(QLatin1Char('>'), ns);
        if (gt < 0 || gt >= end || name.isEmpty())
            break; // malformed tail lands in the final chunk below
        const bool selfClose = html.at(gt - 1) == QLatin1Char('/');
        if (!closing && !selfClose && !voids.contains(name)) {
            stack.append(name);
        } else if (closing) {
            if (!stack.isEmpty() && stack.constLast() == name)
                stack.takeLast();
            if (stack.isEmpty() && boundary.contains(name))
                emitTo(gt + 1);
        }
        pos = gt + 1;
    }
    if (chunkStart < end)
        emitTo(end);
    if (chunks.isEmpty())
        chunks.append(html);
    return chunks;
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    m_loading = true;
    m_document->setPlainText(text);
    m_lastDocumentText = text;
    m_loading = false;

    applyDocumentTypography();
    m_wordCountTimer.stop();
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
    watchCurrentFile();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Only local files can be saved."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        return;
    }

    const bool shouldClose = m_closeAfterSave;
    m_closeAfterSave = false;
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = true;
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();

    if (shouldClose)
        emit closeAfterSave();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

QString Backend::recoveryPath() const {
    return m_recoveryPath;
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = recoveryPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("text"), currentDocumentText()}};
    file.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
    file.commit();
}

void Backend::restoreRecovery() {
    QFile file(recoveryPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text")))
        return;
    const QJsonObject recovery = json.object();
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly)) {
        m_lastKnownFileContents = diskFile.readAll();
        m_hasKnownFileContents = true;
    } else {
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
    }
    setFileUrl(recoveredUrl);
    setModified(true);
    setStatus(QStringLiteral("Recovered unsaved changes"));
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(recoveryPath());
}

void Backend::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        m_fileWatcher.addPath(m_fileUrl.toLocalFile());
}

void Backend::loadOmarchyTheme() {
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            QString value = line.mid(equals + 1).trimmed();
            if (value.size() >= 2
                    && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
                        || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
                value = value.mid(1, value.size() - 2);

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                m_themeBackground = value;
            else if (key == QStringLiteral("foreground"))
                m_themeForeground = value;
            else if (key == QStringLiteral("accent"))
                m_themeAccent = value;
            else if (key == QStringLiteral("selection"))
                m_themeSelection = value;
        }
    }

    bool themeModeKnown = false;
    bool themeIsDark = m_darkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
        themeModeKnown = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
        themeModeKnown = true;
    } else {
        const QColor background(m_themeBackground);
        if (background.isValid()) {
            const double luminance = 0.299 * background.redF()
                + 0.587 * background.greenF() + 0.114 * background.blueF();
            themeIsDark = luminance < 0.5;
            themeModeKnown = true;
        }
    }
    if (themeModeKnown && themeIsDark != m_darkMode) {
        m_darkMode = themeIsDark;
        emit darkModeChanged();
    }

    if (m_highlighter) {
        m_highlighter->setDarkMode(m_darkMode);
        m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);
    }

    emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(suggestedFileName(currentDocumentText())));
}

QString Backend::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

int Backend::countWords(const QString &text) {
    static const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    int count = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

void Backend::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void Backend::refreshWordCount() {
    setWordCount(countWords(currentDocumentText()));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // A full pass is only used for freshly loaded/attached documents, so it is
    // safe to drop undo history here (re-enabling clears the stack anyway).
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(blockFormat);
    m_formattingTypography = false;

    m_document->setUndoRedoEnabled(undoEnabled);

    m_formattedBlockCount = m_document->blockCount();
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    QTextBlockFormat blockFormat;
    blockFormat.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    m_formattingTypography = true;
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.mergeBlockFormat(blockFormat);
    cursor.endEditBlock();
    m_formattingTypography = false;
}
