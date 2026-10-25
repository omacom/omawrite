#include "backend.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QtConcurrent/QtConcurrentRun>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QScreen>
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
#include <QTextLayout>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>
#include <iterator>

#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;

const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString textZoomSetting = QStringLiteral("text/zoom");

// The user's own text size, multiplied onto the desktop's text scale.
// Browser-sized steps, with a wider range for small and large displays.
constexpr qreal textZoomSteps[] = {0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
constexpr int textZoomStepCount = int(std::size(textZoomSteps));

const QString editorFontSetting = QStringLiteral("editor/font");

// Noto ships a separate family per script, which buries every other font in
// the picker. Offer only the core Latin families; the rest still serve as
// fallbacks for whatever scripts a document contains.
const QStringList keptNotoFamilies = {QStringLiteral("Noto Sans"),
                                      QStringLiteral("Noto Sans Mono"),
                                      QStringLiteral("Noto Serif")};

// Fontconfig's generic aliases just point at another listed family.
const QStringList genericFontAliases = {QStringLiteral("Monospace"), QStringLiteral("Mono"),
                                        QStringLiteral("Sans Serif"), QStringLiteral("Sans"),
                                        QStringLiteral("Serif"), QStringLiteral("System-ui")};

// Symbol fonts often claim Latin coverage because they map their glyphs onto
// ASCII, so they can only be told apart by name. D050000L is URW's Dingbats.
const QRegularExpression symbolFontName(
    QStringLiteral("symbol|dingbat|emoji|icon|awesome|^D050000L$"),
    QRegularExpression::CaseInsensitiveOption);

namespace {

// A shade of `from` carried part of the way towards `to`.
QColor blend(const QColor &from, const QColor &to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

}

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

Backend::Backend(QObject *parent) : Backend(QString(), parent) {}

Backend::Backend(const QString &configPath, QObject *parent)
    : QObject(parent), m_explicitConfigPath(configPath) {
    // A settings file truncated by a crash reads back as an empty value, and
    // both that and a NaN would clamp to the smallest size rather than fall
    // back to the default.
    bool zoomOk = false;
    const qreal storedZoom = QSettings().value(textZoomSetting, 1.0).toDouble(&zoomOk);
    m_textZoom = zoomOk && storedZoom > 0 ? qBound(textZoomSteps[0], storedZoom,
                                                   textZoomSteps[textZoomStepCount - 1])
                                          : 1.0;

    const QString savedFont = QSettings().value(editorFontSetting).toString();
    m_editorFont = !savedFont.isEmpty() && QFontDatabase::hasFamily(savedFont)
        ? savedFont
        : defaultEditorFont();

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
    connect(&m_wordCountWatcher, &QFutureWatcher<int>::finished, this, [this] {
        if (!m_wordCountStale)
            setWordCount(m_wordCountWatcher.result());
        if (m_wordCountAgain) {
            m_wordCountAgain = false;
            refreshWordCount();
        }
    });
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::writeRecovery);
    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(500);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &Backend::autoSave);
    // Tools that replace a file by removing it and writing it again leave it
    // briefly missing, so let the burst settle before deciding what happened.
    m_fileCheckTimer.setSingleShot(true);
    m_fileCheckTimer.setInterval(100);
    connect(&m_fileCheckTimer, &QTimer::timeout, this, &Backend::checkWatchedFile);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, &m_fileCheckTimer,
            qOverload<>(&QTimer::start));
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged, &m_fileCheckTimer,
            qOverload<>(&QTimer::start));

    loadOmarchyTheme();
    watchOmarchyTheme();
    // Switching themes touches several watched paths in a row, and the theme
    // folder can be briefly missing, so reload once the burst has settled.
    m_themeReloadTimer.setSingleShot(true);
    m_themeReloadTimer.setInterval(100);
    connect(&m_themeReloadTimer, &QTimer::timeout, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, &m_themeReloadTimer,
            qOverload<>(&QTimer::start));
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

void Backend::setConfigPath(const QString &configPath) {
    if (m_explicitConfigPath == configPath)
        return;

    m_explicitConfigPath = configPath;
    m_lastConfigFont.clear();
    m_lastConfigZoom.clear();
    loadOmarchyTheme();
    watchOmarchyTheme();
}

QString Backend::defaultConfigPath() {
    const QByteArray xdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    const QString baseDir = !xdgConfigHome.isEmpty()
        ? QString::fromLocal8Bit(xdgConfigHome)
        : QDir::homePath() + QStringLiteral("/.config");
    return QDir(baseDir).filePath(QStringLiteral("omawrite/omawrite.toml"));
}

QString Backend::generateSystemConfigToml(bool systemDarkMode) {
    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");

    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    QString selectionBackground;
    QString selectionForeground;
    QString brightForeground;
    QString lighterBackground;

    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
                    || line.startsWith(QLatin1Char('[')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            const QString value = tomlValue(line.mid(equals + 1));

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                background = value;
            else if (key == QStringLiteral("foreground"))
                foreground = value;
            else if (key == QStringLiteral("accent"))
                accent = value;
            else if (key == QStringLiteral("selection"))
                selection = value;
            else if (key == QStringLiteral("selection_background"))
                selectionBackground = value;
            else if (key == QStringLiteral("selection_foreground"))
                selectionForeground = value;
            else if (key == QStringLiteral("bright_foreground"))
                brightForeground = value;
            else if (key == QStringLiteral("lighter_background"))
                lighterBackground = value;
        }
    }

    bool darkMode = systemDarkMode;
    if (themeMode == QStringLiteral("dark")) {
        darkMode = true;
    } else if (themeMode == QStringLiteral("light")) {
        darkMode = false;
    } else {
        const QColor parsedBackground(background);
        if (parsedBackground.isValid()) {
            const double luminance = 0.299 * parsedBackground.redF()
                + 0.587 * parsedBackground.greenF() + 0.114 * parsedBackground.blueF();
            darkMode = luminance < 0.5;
        }
    }

    QString resolvedBackground = darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    QString resolvedForeground = darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    QString resolvedAccent = darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    QString resolvedSelection = darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(resolvedBackground, background);
    applyColor(resolvedForeground, foreground);
    applyColor(resolvedAccent, accent);
    applyColor(resolvedSelection, selectionBackground);
    applyColor(resolvedSelection, selection);
    QString resolvedSelectionForeground = resolvedForeground;
    applyColor(resolvedSelectionForeground, brightForeground);
    applyColor(resolvedSelectionForeground, selectionForeground);

    const QColor page(resolvedBackground);
    const QColor lighter(lighterBackground);
    const QString resolvedCodeBackground = (lighter.isValid() && lighter != page
                                                ? lighter
                                                : blend(page, QColor(resolvedForeground), 0.06)).name();
    const QString resolvedCodeForeground = resolvedForeground;

    bool zoomOk = false;
    const qreal storedZoom = QSettings().value(textZoomSetting, 1.0).toDouble(&zoomOk);
    const qreal zoom = zoomOk && storedZoom > 0
        ? qBound(textZoomSteps[0], storedZoom, textZoomSteps[textZoomStepCount - 1])
        : 1.0;

    const QString savedFont = QSettings().value(editorFontSetting).toString();
    const QString font = !savedFont.isEmpty() ? savedFont : defaultEditorFont();

    QString escapedFont = font;
    escapedFont.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escapedFont.replace(QLatin1Char('"'), QStringLiteral("\\\""));

    return QStringLiteral(
               "mode = \"%1\"\n"
               "background = \"%2\"\n"
               "foreground = \"%3\"\n"
               "accent = \"%4\"\n"
               "selection = \"%5\"\n"
               "selection_foreground = \"%6\"\n"
               "code_background = \"%7\"\n"
               "code_foreground = \"%8\"\n"
               "code_line_numbers = false\n"
               "# code_line_numbers_font = \"\"\n"
               "# code_line_numbers_foreground = \"\"\n"
               "# code_line_numbers_background = \"\"\n"
               "# code_line_numbers_size = 14\n"
               "\n"
               "# background_image = \"\"\n"
               "# background_opacity = 0.85\n"
               "\n"
               "font = \"%9\"\n"
               "# code_font = \"\"\n"
               "zoom = %10\n")
        .arg(darkMode ? QStringLiteral("dark") : QStringLiteral("light"),
             resolvedBackground,
             resolvedForeground,
             resolvedAccent,
             resolvedSelection,
             resolvedSelectionForeground,
             resolvedCodeBackground,
             resolvedCodeForeground,
             escapedFont,
             QString::number(zoom, 'g', 4));
}

int Backend::generateDefaultConfigFile(bool systemDarkMode, QString *outputMessage) {
    const QFileInfo targetInfo(defaultConfigPath());
    const QString absolutePath = targetInfo.absoluteFilePath();

    const auto report = [outputMessage](int code, const QString &message) {
        if (outputMessage)
            *outputMessage = message;
        QTextStream(code == 0 ? stdout : stderr) << message << QLatin1Char('\n');
        return code;
    };

    if (targetInfo.exists()) {
        return report(1, QStringLiteral("omawrite: configuration file already exists at %1")
                             .arg(absolutePath));
    }

    if (!QDir().mkpath(targetInfo.dir().absolutePath())) {
        return report(1, QStringLiteral("omawrite: could not create directory for %1")
                             .arg(absolutePath));
    }

    QFile out(absolutePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text)) {
        return report(1, QStringLiteral("omawrite: could not write configuration file at %1")
                             .arg(absolutePath));
    }

    const QByteArray contents = generateSystemConfigToml(systemDarkMode).toUtf8();
    if (out.write(contents) != qint64(contents.size()) || !out.flush()) {
        out.close();
        out.remove();
        return report(1, QStringLiteral("omawrite: could not write configuration file at %1")
                             .arg(absolutePath));
    }

    out.close();
    return report(0, QStringLiteral("Generated configuration file at %1").arg(absolutePath));
}

QString Backend::activeConfigPath() const {
    if (!m_explicitConfigPath.isEmpty())
        return QFileInfo::exists(m_explicitConfigPath) ? m_explicitConfigPath : QString();

    const QString candidate = defaultConfigPath();
    return QFileInfo::exists(candidate) ? candidate : QString();
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
    m_systemDarkMode = darkMode;
    loadOmarchyTheme();
}

void Backend::setDesktopTextScale(qreal textScale) {
    if (qFuzzyCompare(m_desktopTextScale, textScale))
        return;

    m_desktopTextScale = textScale;
    emit textScaleChanged();
    emit codeLineNumbersChanged();
}

void Backend::increaseTextSize() {
    setTextZoom(steppedTextZoom(1));
}

void Backend::decreaseTextSize() {
    setTextZoom(steppedTextZoom(-1));
}

void Backend::resetTextSize() {
    setTextZoom(1.0);
}

// The neighbouring rung, starting from whichever rung the current zoom sits
// closest to; the ends of the ladder hold.
qreal Backend::steppedTextZoom(int direction) const {
    int closest = 0;
    for (int step = 1; step < textZoomStepCount; ++step) {
        if (qAbs(textZoomSteps[step] - m_textZoom) < qAbs(textZoomSteps[closest] - m_textZoom))
            closest = step;
    }

    return textZoomSteps[qBound(0, closest + direction, textZoomStepCount - 1)];
}

void Backend::setTextZoom(qreal zoom) {
    // Report the size even when the ladder has run out, so a keypress that
    // changes nothing still says why.
    setStatus(QStringLiteral("Text size %1%").arg(qRound(zoom * 100)));
    if (qFuzzyCompare(m_textZoom, zoom))
        return;

    m_textZoom = zoom;
    QSettings().setValue(textZoomSetting, zoom);
    emit textScaleChanged();
    emit codeLineNumbersChanged();
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
    m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground, m_themeAccent,
                            m_themeCodeBackground, m_themeCodeForeground, codeFont());
    m_highlighter->setCaret(m_caretPosition);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int charsRemoved, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
                m_lastChangeRemoved = charsRemoved;
            });

    connectDocumentLayout();
    applyDocumentTypography(false);
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
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray raw = file.readAll();
    // Text mode drops carriage returns, matching how the file is compared
    // and saved later.
    file.setTextModeEnabled(true);
    file.seek(0);
    const QByteArray contents = file.readAll();
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    if (savesUnchanged(raw)) {
        setStatus(QStringLiteral("Opened %1").arg(fileName()));
    } else {
        // Saving writes UTF-8 with plain line endings, which would change
        // bytes this file holds. Leave that to an explicit save.
        suspendAutoSave();
        setStatus(QStringLiteral("Opened %1. Auto-save is off to keep its encoding and line endings.")
                      .arg(fileName()));
    }
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::discardRecovery() {
    clearRecovery();
    // Discarded changes must never reach the file, even if the open or close
    // that follows fails and leaves them in the editor. Opening a file or an
    // explicit save lifts this.
    suspendAutoSave();
}

// A file named on the command line need not exist yet: `omawrite new.md`
// starts a blank document that the first save writes to that path. Opening
// from the file picker or reloading still requires the file to be there.
void Backend::openOrStartFile(const QUrl &requestedUrl) {
    // A relative path names a file in the directory Omawrite was started
    // from; a recovery draft must not resolve it against another one later.
    const QFileInfo info(requestedUrl.toLocalFile());
    const QUrl url = requestedUrl.isLocalFile()
        ? QUrl::fromLocalFile(info.absoluteFilePath()) : requestedUrl;
    if (m_modified) {
        // A recovered draft fills this window; opening over it would lose it.
        setStatus(QStringLiteral("Recovered unsaved changes; %1 was not opened.")
                      .arg(info.fileName()));
        return;
    }
    if (!url.isLocalFile() || info.exists() || !info.absoluteDir().exists()) {
        open(url);
        return;
    }

    loadDocumentText(QString());
    clearRecovery();
    // Nothing has been read, so no text counts as saved, and auto-save holds
    // off until an explicit save creates the file (see autoSave()).
    setKnownFileContents(QByteArray(), false);
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("New file %1").arg(fileName()));
}

void Backend::reloadFromDisk() {
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void Backend::keepExternalVersion() {
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray raw = file.readAll();
        file.setTextModeEnabled(true);
        file.seek(0);
        setKnownFileContents(file.readAll(), true);
        // Keeping our text over an edited file is a deliberate choice to
        // overwrite it, so auto-save may resume, unless saving would change
        // the file's encoding or line endings. A removed file stays removed
        // until the writer saves it again.
        m_autoSaveSuspended = !savesUnchanged(raw);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    setModified(true);
    scheduleRecovery();
    scheduleAutoSave();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

QFont Backend::printFont(const QFont &editorFont, qreal screenDpi) {
    // The editor sizes its font in pixels, which a printer takes as device dots.
    // Convert to points at the screen's DPI so the page matches the editor.
    QFont font = editorFont;
    if (font.pixelSize() <= 0)
        return font;

    const qreal dpi = screenDpi > 0.0 ? screenDpi : 96.0;
    font.setPointSizeF(font.pixelSize() * 72.0 / dpi);
    return font;
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
        const QScreen *screen = m_parentWindow ? m_parentWindow->screen()
                                               : QGuiApplication::primaryScreen();
        rendered.setDefaultFont(printFont(m_document->defaultFont(),
                                          screen ? screen->logicalDotsPerInchY() : 0.0));
        rendered.setMarkdown(currentDocumentText());
        rendered.print(&printer);
    }
}

void Backend::newWindow() {
    QStringList args;
    if (!m_explicitConfigPath.isEmpty())
        args << QStringLiteral("--config") << m_explicitConfigPath;
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 args);
    if (!started)
        setStatus(QStringLiteral("Could not open a new window."));
}

void Backend::copyAsHtml() {
    QTextDocument rendered;
    rendered.setMarkdown(currentDocumentText());

    // Drop Qt's fixed fonts and margins from the body and blocks, so the pasted
    // text takes on the destination's own typeface and paragraph spacing.
    QString html = rendered.toHtml();
    static const QRegularExpression blockStyle(
        QStringLiteral("<(body|p|h[1-6]|ul|ol|li)\\s+style=\"[^\"]*\""));
    html.replace(blockStyle, QStringLiteral("<\\1"));

    auto *mimeData = new QMimeData;
    mimeData->setHtml(html);
    mimeData->setText(rendered.toPlainText());
    QGuiApplication::clipboard()->setMimeData(mimeData);
    setStatus(QStringLiteral("Copied as HTML."));
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

    reapplyTypographyToChange();

    scheduleWordCount();

    const QString &baseline = m_lastKnownFileText;
    // An empty baseline means a pristine untitled document, but only when there
    // is no file behind it. Once there is one, an unknown baseline is unknown
    // rather than empty, and emptying the editor is a change like any other.
    const bool baselineKnown = m_hasKnownFileContents
        || !m_fileUrl.isValid() || m_fileUrl.isEmpty();

    if (baselineKnown && text == baseline) {
        setModified(false);
        clearRecovery();
        if (m_hasKnownFileContents)
            setStatus(QStringLiteral("Saved %1").arg(fileName()));
        else
            setStatus(QString());
    } else {
        setModified(true);
        setStatus(QStringLiteral("Unsaved"));
        scheduleRecovery();
        scheduleAutoSave();
    }

    return true;
}

void Backend::setCaretPosition(int position) {
    m_caretPosition = position;
    if (m_highlighter)
        m_highlighter->setCaret(position);
}

// The emphasis span of the marker's kind that the caret sits in or against,
// in document positions, so a toggle with nothing selected can act on it.
QVariantMap Backend::emphasisSpanAt(int position, const QString &marker) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid() || block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    using Kind = MarkdownHighlighter::InlineKind;
    const Kind kind = marker == QStringLiteral("**") ? Kind::Bold
        : marker == QStringLiteral("~~")              ? Kind::Strikethrough
                                                      : Kind::Italic;
    const int offset = position - block.position();
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const auto &item : markup) {
        const int start = item.markers[0].start;
        const int end = item.markers[1].start + item.markers[1].length;
        if (item.kind != kind || offset < start || offset > end)
            continue;
        // The visible text ends before the chain of nested emphasis closing
        // with it, as the italic "*" inside "***word***".
        const int contentEnd = item.content.start + item.content.length;
        int textEnd = contentEnd;
        for (bool moved = true; moved;) {
            moved = false;
            for (const auto &nested : markup) {
                const auto &closing = nested.markers[1];
                if (closing.start >= item.content.start && closing.length > 0
                        && closing.start + closing.length == textEnd) {
                    textEnd = closing.start;
                    moved = true;
                }
            }
        }
        const int base = block.position();
        return {{QStringLiteral("start"), base + start},
                {QStringLiteral("end"), base + end},
                {QStringLiteral("contentStart"), base + item.content.start},
                {QStringLiteral("contentEnd"), base + contentEnd},
                {QStringLiteral("textEnd"), base + textEnd}};
    }
    return {};
}

QString Backend::linkUrlAt(int position) const {
    if (!m_document)
        return {};

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return {};

    // Fenced code is literal, so nothing in it is a link.
    if (block.userState() >= MarkdownHighlighter::InsideFence
            || block.previous().userState() >= MarkdownHighlighter::InsideFence)
        return {};

    return MarkdownHighlighter::linkUrlAt(block.text(), position - block.position());
}

int Backend::cursorPositionStep(int position, int direction) const
{
    if (!m_document || direction == 0)
        return position;

    const int documentLength = m_document->characterCount() - 1;
    position = qBound(0, position, documentLength);

    if (direction > 0) {
        if (position >= documentLength)
            return documentLength;
    } else if (position <= 0) {
        return 0;
    }

    const QTextBlock block = m_document->findBlock(position);
    if (!block.isValid())
        return position;

    const int blockStart = block.position();
    const int blockTextLength = block.text().size();
    // A block's text excludes its paragraph separator, so this is the last
    // cursor position inside the block, just before the separator.
    const int blockEnd = blockStart + blockTextLength;

    if (direction > 0) {
        // The separator is the next valid cursor position past a block; the
        // layout only knows the block's own characters.
        if (position >= blockEnd)
            return position + 1;
    } else if (position <= blockStart) {
        return position - 1;
    }

    // A fresh layout over the block's own text is always in sync with the
    // document, unlike the document's display layout, whose text() cannot be
    // checked for staleness. Building lines is unnecessary for cursor
    // stepping and dominates the cost, so the layout is left unformatted.
    // QTextLayout steps by whole grapheme clusters, so surrogate pairs, skin
    // tone modifiers and ZWJ sequences are crossed in a single move.
    QTextLayout layout(block.text());
    const int local = position - blockStart;
    const int stepped = direction > 0
        ? layout.nextCursorPosition(local, QTextLayout::SkipCharacters)
        : layout.previousCursorPosition(local, QTextLayout::SkipCharacters);
    return blockStart + qBound(0, stepped, blockTextLength);
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

// Found by the highlighter's own matcher, so the offsets agree with what it
// paints, diacritics and all.
QVariantList Backend::searchMatches(const QString &query) const {
    QVariantList matches;
    if (query.isEmpty())
        return matches;
    for (const int start : MarkdownHighlighter::findMatches(currentDocumentText(), query))
        matches.append(start);
    return matches;
}

// One edit command, so a single undo takes the whole replacement back.
void Backend::replaceRange(int start, int end, const QString &text) {
    if (!m_document)
        return;
    const int last = m_document->characterCount() - 1;
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    cursor.setPosition(qBound(0, start, last));
    cursor.setPosition(qBound(0, end, last), QTextCursor::KeepAnchor);
    cursor.insertText(text);
    cursor.endEditBlock();
}

// A block's state says what follows its line, so a fence line itself is only
// code on its inner side: past an opening fence's backticks or tildes, or anywhere
// before the end of a closing one.
bool Backend::insideFence(int position) const {
    if (!m_document)
        return false;
    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return false;
    const bool opensOrContinues = block.userState() >= MarkdownHighlighter::InsideFence;
    const bool afterOpening = block.previous().userState() >= MarkdownHighlighter::InsideFence;
    const int offset = position - block.position();
    if (opensOrContinues && !afterOpening) {
        static const QRegularExpression fenceMark(QStringLiteral("[`~]"));
        return offset >= block.text().indexOf(fenceMark) + 3;
    }
    if (afterOpening && !opensOrContinues)
        return offset < block.text().length();
    return opensOrContinues;
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

QString Backend::defaultEditorFont() {
    return QStringLiteral("IBM Plex Mono");
}

void Backend::setEditorFont(const QString &family) {
    if (family.isEmpty() || family == m_editorFont)
        return;

    m_editorFont = family;
    QSettings().setValue(editorFontSetting, family);
    emit editorFontChanged();
    if (m_codeFont.isEmpty()) {
        emit codeFontChanged();
        if (m_highlighter)
            m_highlighter->setCodeFont(codeFont());
    }
    if (m_codeLineNumberFont.isEmpty())
        emit codeLineNumbersChanged();
}

void Backend::setCodeFont(const QString &family) {
    if (family == m_codeFont)
        return;

    m_codeFont = family;
    emit codeFontChanged();
    if (m_codeLineNumberFont.isEmpty())
        emit codeLineNumbersChanged();
    if (m_highlighter)
        m_highlighter->setCodeFont(codeFont());
    applyDocumentTypography(true);
    emit codeBlockLinesChanged();
    emit codeBlockPanelsChanged();
}

void Backend::setCodeLineNumberFont(const QString &family) {
    if (m_codeLineNumberFont == family)
        return;
    m_codeLineNumberFont = family;
    emit codeLineNumbersChanged();
}

void Backend::setCodeLineNumberForeground(const QString &color) {
    if (m_codeLineNumberForeground == color)
        return;
    m_codeLineNumberForeground = color;
    emit codeLineNumbersChanged();
}

void Backend::setCodeLineNumberBackground(const QString &color) {
    if (m_codeLineNumberBackground == color)
        return;
    m_codeLineNumberBackground = color;
    emit codeLineNumbersChanged();
}

void Backend::setCodeLineNumberSize(qreal size) {
    if (qFuzzyCompare(m_codeLineNumberSize, size))
        return;
    m_codeLineNumberSize = size;
    emit codeLineNumbersChanged();
}

QStringList Backend::availableFonts() const {
    // Leave out icon, emoji, and other symbol fonts, along with anything that
    // cannot set the Latin text the editor is written in.
    QStringList families;
    for (const QString &family : QFontDatabase::families(QFontDatabase::Latin)) {
        if (QFontDatabase::isPrivateFamily(family)
                || QFontDatabase::writingSystems(family).contains(QFontDatabase::Symbol))
            continue;
        families.append(family);
    }
    return selectableFontFamilies(families);
}

QStringList Backend::selectableFontFamilies(const QStringList &families) {
    QStringList selectable;
    static const QRegularExpression foundrySuffix(QStringLiteral("\\s*\\[[^\\]]*\\]$"));
    for (QString family : families) {
        // The same family from two foundries is listed as "Name [Foundry]".
        family.remove(foundrySuffix);
        if (family.startsWith(QStringLiteral("Noto ")) && !keptNotoFamilies.contains(family))
            continue;
        if (genericFontAliases.contains(family, Qt::CaseInsensitive)
                || family.contains(symbolFontName))
            continue;
        if (family == defaultEditorFont() || selectable.contains(family, Qt::CaseInsensitive))
            continue;
        selectable.append(family);
    }
    std::sort(selectable.begin(), selectable.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    selectable.prepend(defaultEditorFont());
    return selectable;
}

void Backend::documentFontChanged() {
    if (m_highlighter)
        m_highlighter->refreshFont();
    applyDocumentTypography(true);
    emit codeBlockLinesChanged();
    emit codeBlockPanelsChanged();
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

    applyDocumentTypography(false);
    m_wordCountTimer.stop();
    m_wordCountStale = true;
    m_wordCountAgain = false;
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
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
        setStatus(QStringLiteral("Only local files can be saved."));
        emit saveFailed();
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    // Without a temporary file beside it, as in a folder only the file itself
    // may be written in, write the file in place rather than fail.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    unwatchFiles();

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        emit saveFailed();
        return;
    }

    setKnownFileContents(contents, true);
    m_autoSaveSuspended = false;
    m_autoSaveTimer.stop();
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

void Backend::scheduleAutoSave() {
    // Only auto-save documents with a file name. Untitled drafts are left to
    // manual save so auto-save never silently opens a file picker or creates a
    // file the writer did not name; crash recovery still covers them. A named
    // file that does not exist yet waits for an explicit save (see autoSave()).
    if (m_fileUrl.isLocalFile() && !m_autoSaveSuspended && !m_autoSaveHeld)
        m_autoSaveTimer.start();
}

void Backend::holdAutoSave(bool held) {
    m_autoSaveHeld = held;
    if (held)
        m_autoSaveTimer.stop();
    else if (m_modified)
        scheduleAutoSave();
}

void Backend::autoSave() {
    if (!m_modified || !m_fileUrl.isLocalFile() || m_autoSaveSuspended || m_autoSaveHeld)
        return;

    // Never write over a version we have not seen. The watcher normally
    // catches outside edits first, but it can miss some (or fire late), so
    // check the file itself right before replacing it.
    const bool exists = QFileInfo::exists(m_fileUrl.toLocalFile());
    if (!m_hasKnownFileContents) {
        // A removed file, a new one not yet saved, or a recovered draft whose
        // file was gone waits for an explicit save rather than being created.
        suspendAutoSave();
        if (exists)
            emit externalChangeDetected(false, m_modified);
        return;
    }
    if (!diskMatchesKnownContents()) {
        reportExternalChange(!exists);
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::suspendAutoSave() {
    // Held until the writer reloads, keeps their version, or saves explicitly.
    m_autoSaveTimer.stop();
    m_autoSaveSuspended = true;
}

// Whether the file still holds what we last read or wrote, read as open()
// reads it so carriage returns compare equal.
bool Backend::diskMatchesKnownContents() const {
    if (!m_hasKnownFileContents)
        return false;
    QFile file(m_fileUrl.toLocalFile());
    return file.open(QIODevice::ReadOnly | QIODevice::Text)
        && file.readAll() == m_lastKnownFileContents;
}

void Backend::reportExternalChange(bool deleted) {
    // Whatever is on disk now, it is not what we last read, so the baseline is
    // unknown until the writer picks a version. A deletion counts: the old
    // text is not saved anywhere either.
    setKnownFileContents(QByteArray(), false);
    suspendAutoSave();
    emit externalChangeDetected(deleted, m_modified);
}

// Saving writes UTF-8 with plain line endings; anything else on disk would be
// rewritten by it.
bool Backend::savesUnchanged(const QByteArray &raw) {
    return !raw.contains('\r') && QString::fromUtf8(raw).toUtf8() == raw;
}

void Backend::setKnownFileContents(const QByteArray &contents, bool known) {
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = known;
    // Every caller reads in text mode or writes plain line endings, so there
    // are no carriage returns to strip.
    m_lastKnownFileText = known ? QString::fromUtf8(contents) : QString();
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = m_recoveryPath;
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
    QFile file(m_recoveryPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text"))) {
        // Unreadable now means unreadable every launch; do not keep claiming it.
        file.remove();
        return;
    }
    const QJsonObject recovery = json.object();
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setKnownFileContents(diskFile.readAll(), true);
    } else {
        setKnownFileContents(QByteArray(), false);
    }
    setFileUrl(recoveredUrl);
    // The file may have moved on since this draft was written, and the draft
    // alone cannot tell. Leave overwriting it to an explicit save.
    m_autoSaveSuspended = recoveredUrl.isLocalFile();
    watchCurrentFile();
    setModified(true);
    setStatus(QStringLiteral("Recovered unsaved changes"));
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(m_recoveryPath);
}

void Backend::unwatchFiles() {
    m_fileCheckTimer.stop();
    const QStringList watched = m_fileWatcher.files() + m_fileWatcher.directories();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
}

// A missing file cannot be watched, so watch its folder instead to notice it
// coming back.
void Backend::watchCurrentFile() {
    unwatchFiles();
    if (!m_fileUrl.isLocalFile())
        return;
    const QFileInfo info(m_fileUrl.toLocalFile());
    m_fileMissing = !info.exists();
    if (info.exists())
        m_fileWatcher.addPath(info.filePath());
    else if (info.dir().exists())
        m_fileWatcher.addPath(info.dir().path());
}

void Backend::checkWatchedFile() {
    // Atomic saves replace the watched inode, which drops it from the
    // watcher, so re-arm it either way. Our own save is not an outside edit.
    const bool wasMissing = m_fileMissing;
    watchCurrentFile();
    if (diskMatchesKnownContents())
        return;
    // A file that was already missing has been reported, or was never there.
    // One that turns up again is an outside edit like any other, even while
    // the writer is still looking at the removal.
    if (m_fileMissing && wasMissing)
        return;
    reportExternalChange(m_fileMissing);
}

// A quoted TOML value ends at its closing quote; whatever trails it is an inline comment.
QString Backend::tomlValue(const QString &text) {
    const QString value = text.trimmed();
    if (value.isEmpty())
        return value;

    const QChar quote = value.front();
    if (quote != QLatin1Char('"') && quote != QLatin1Char('\'')) {
        const int comment = value.indexOf(QLatin1Char('#'));
        if (comment > 0 && value.at(comment - 1).isSpace())
            return value.left(comment).trimmed();
        return value;
    }

    if (quote == QLatin1Char('\'')) {
        const int close = value.indexOf(quote, 1);
        return close < 0 ? value : value.mid(1, close - 1);
    }

    QString decoded;
    decoded.reserve(value.length());
    bool closed = false;

    for (int i = 1; i < value.length(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('"')) {
            closed = true;
            break;
        }
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= value.length()) {
                decoded.append(c);
                break;
            }
            const QChar esc = value.at(++i);
            switch (esc.unicode()) {
            case '"':
                decoded.append(QLatin1Char('"'));
                break;
            case '\\':
                decoded.append(QLatin1Char('\\'));
                break;
            case 'b':
                decoded.append(QChar(0x08));
                break;
            case 't':
                decoded.append(QLatin1Char('\t'));
                break;
            case 'n':
                decoded.append(QLatin1Char('\n'));
                break;
            case 'f':
                decoded.append(QChar(0x0C));
                break;
            case 'r':
                decoded.append(QLatin1Char('\r'));
                break;
            case 'u': {
                if (i + 4 < value.length()) {
                    bool ok = false;
                    const char32_t code = value.mid(i + 1, 4).toUInt(&ok, 16);
                    if (ok) {
                        decoded.append(QString::fromUcs4(&code, 1));
                        i += 4;
                        break;
                    }
                }
                decoded.append(QLatin1Char('\\'));
                decoded.append(esc);
                break;
            }
            case 'U': {
                if (i + 8 < value.length()) {
                    bool ok = false;
                    const char32_t code = value.mid(i + 1, 8).toUInt(&ok, 16);
                    if (ok) {
                        decoded.append(QString::fromUcs4(&code, 1));
                        i += 8;
                        break;
                    }
                }
                decoded.append(QLatin1Char('\\'));
                decoded.append(esc);
                break;
            }
            default:
                decoded.append(QLatin1Char('\\'));
                decoded.append(esc);
                break;
            }
        } else {
            decoded.append(c);
        }
    }

    return closed ? decoded : value;
}

void Backend::loadOmarchyTheme() {
    const QString customConfig = activeConfigPath();
    const bool usingCustomConfig = !customConfig.isEmpty();
    const QString colorsPath = usingCustomConfig
        ? customConfig
        : QDir::homePath() + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");

    QString themeMode;
    QString background;
    QString foreground;
    QString accent;
    QString selection;
    QString selectionBackground;
    QString selectionForeground;
    QString brightForeground;
    QString lighterBackground;
    QString codeBackground;
    QString codeForeground;
    QString codeLineNumbersValue;
    QString codeLineNumbersFontValue;
    QString codeLineNumbersForegroundValue;
    QString codeLineNumbersBackgroundValue;
    QString codeLineNumbersSizeValue;
    QString backgroundImagePath;
    QString backgroundOpacityValue;
    QString fontValue;
    QString codeFontValue;
    QString zoomValue;

    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
                    || line.startsWith(QLatin1Char('[')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            const QString value = tomlValue(line.mid(equals + 1));

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                background = value;
            else if (key == QStringLiteral("foreground"))
                foreground = value;
            else if (key == QStringLiteral("accent"))
                accent = value;
            else if (key == QStringLiteral("selection"))
                selection = value;
            else if (key == QStringLiteral("selection_background"))
                selectionBackground = value;
            else if (key == QStringLiteral("selection_foreground"))
                selectionForeground = value;
            else if (key == QStringLiteral("bright_foreground"))
                brightForeground = value;
            else if (key == QStringLiteral("lighter_background"))
                lighterBackground = value;
            else if (usingCustomConfig && key == QStringLiteral("code_background"))
                codeBackground = value;
            else if (key == QStringLiteral("code_foreground"))
                codeForeground = value;
            else if (usingCustomConfig && key == QStringLiteral("code_line_numbers"))
                codeLineNumbersValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("code_line_numbers_font")
                         || key == QStringLiteral("code_line_number_font")))
                codeLineNumbersFontValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("code_line_numbers_foreground")
                         || key == QStringLiteral("code_line_number_foreground")
                         || key == QStringLiteral("code_line_numbers_color")
                         || key == QStringLiteral("code_line_number_color")
                         || key == QStringLiteral("code_line_numbers_fg")
                         || key == QStringLiteral("code_line_number_fg")))
                codeLineNumbersForegroundValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("code_line_numbers_background")
                         || key == QStringLiteral("code_line_number_background")
                         || key == QStringLiteral("code_line_numbers_bg")
                         || key == QStringLiteral("code_line_number_bg")))
                codeLineNumbersBackgroundValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("code_line_numbers_size")
                         || key == QStringLiteral("code_line_number_size")
                         || key == QStringLiteral("code_line_numbers_font_size")
                         || key == QStringLiteral("code_line_number_font_size")))
                codeLineNumbersSizeValue = value;
            else if (usingCustomConfig && key == QStringLiteral("background_image"))
                backgroundImagePath = value;
            else if (usingCustomConfig && key == QStringLiteral("background_opacity"))
                backgroundOpacityValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("font") || key == QStringLiteral("editor_font")))
                fontValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("code_font") || key == QStringLiteral("editor_code_font")))
                codeFontValue = value;
            else if (usingCustomConfig
                     && (key == QStringLiteral("zoom") || key == QStringLiteral("text_scale")))
                zoomValue = value;
        }
    }

    const bool wasDark = m_darkMode;
    const QStringList previousColors{m_themeBackground, m_themeForeground, m_themeAccent,
                                     m_themeSelection, m_themeSelectionForeground,
                                     m_themeCodeBackground, m_themeCodeForeground};
    const bool previousCodeLineNumbers = m_codeLineNumbers;
    const QString previousCodeLineNumberFont = m_codeLineNumberFont;
    const QString previousCodeLineNumberForeground = m_codeLineNumberForeground;
    const QString previousCodeLineNumberBackground = m_codeLineNumberBackground;
    const qreal previousCodeLineNumberSize = m_codeLineNumberSize;
    const QUrl previousBackgroundImage = m_backgroundImage;
    const qreal previousBackgroundOpacity = m_backgroundOpacity;

    bool themeIsDark = m_systemDarkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
    } else {
        const QColor parsedBackground(background);
        if (parsedBackground.isValid()) {
            const double luminance = 0.299 * parsedBackground.redF()
                + 0.587 * parsedBackground.greenF() + 0.114 * parsedBackground.blueF();
            themeIsDark = luminance < 0.5;
        }
    }
    m_darkMode = themeIsDark;

    // The defaults follow the mode the theme resolved to, so a colour we cannot
    // read costs that one colour rather than the contrast of the whole editor.
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    // An unparseable colour reaches QML as black, so keep the default instead.
    // Later calls win, so each fallback is applied before what it falls back from.
    const auto applyColor = [](QString &target, const QString &value) {
        if (QColor(value).isValid())
            target = value;
    };
    applyColor(m_themeBackground, background);
    applyColor(m_themeForeground, foreground);
    applyColor(m_themeAccent, accent);
    applyColor(m_themeSelection, selectionBackground);
    applyColor(m_themeSelection, selection);
    m_themeSelectionForeground = m_themeForeground;
    applyColor(m_themeSelectionForeground, brightForeground);
    applyColor(m_themeSelectionForeground, selectionForeground);

    // Omarchy themes name a lighter background for panels like the one code
    // sits on, so use the shade the theme chose. Not every theme sets the key,
    // and a few set it to the page background, which would leave code with no
    // panel at all: mix a shade of the page towards the text for those. Mixing,
    // unlike lightening, still moves on a pure black background.
    const QColor page(m_themeBackground);
    const QColor chosenCodeColor(!codeBackground.isEmpty() ? codeBackground : lighterBackground);
    m_themeCodeBackground = (chosenCodeColor.isValid() && chosenCodeColor != page
                                 ? chosenCodeColor
                                 : blend(page, QColor(m_themeForeground), 0.06)).name();

    const QColor chosenCodeFg(codeForeground);
    m_themeCodeForeground = chosenCodeFg.isValid() ? chosenCodeFg.name() : m_themeForeground;

    QString resolvedLineNumberFont;
    if (usingCustomConfig && !codeLineNumbersFontValue.isEmpty())
        resolvedLineNumberFont = codeLineNumbersFontValue;
    m_codeLineNumberFont = resolvedLineNumberFont;

    QString resolvedLineNumberFg;
    if (usingCustomConfig && !codeLineNumbersForegroundValue.isEmpty()) {
        const QColor fgColor(codeLineNumbersForegroundValue);
        if (fgColor.isValid())
            resolvedLineNumberFg = fgColor.name();
    }
    m_codeLineNumberForeground = resolvedLineNumberFg;

    QString resolvedLineNumberBg;
    if (usingCustomConfig && !codeLineNumbersBackgroundValue.isEmpty()) {
        if (codeLineNumbersBackgroundValue.trimmed().toLower() == QStringLiteral("transparent")) {
            resolvedLineNumberBg = QStringLiteral("transparent");
        } else {
            const QColor bgColor(codeLineNumbersBackgroundValue);
            if (bgColor.isValid())
                resolvedLineNumberBg = bgColor.name();
        }
    }
    m_codeLineNumberBackground = resolvedLineNumberBg;

    qreal resolvedLineNumberSize = 0.0;
    if (usingCustomConfig && !codeLineNumbersSizeValue.isEmpty()) {
        QString s = codeLineNumbersSizeValue.trimmed();
        if (s.endsWith(QLatin1String("px"), Qt::CaseInsensitive))
            s.chop(2);
        else if (s.endsWith(QLatin1String("pt"), Qt::CaseInsensitive))
            s.chop(2);
        bool ok = false;
        const qreal sz = s.trimmed().toDouble(&ok);
        if (ok && sz > 0.0)
            resolvedLineNumberSize = sz;
    }
    m_codeLineNumberSize = resolvedLineNumberSize;

    bool resolvedCodeLineNumbers = false;
    if (usingCustomConfig && !codeLineNumbersValue.isEmpty()) {
        const QString lower = codeLineNumbersValue.trimmed().toLower();
        resolvedCodeLineNumbers = (lower == QStringLiteral("true") || lower == QStringLiteral("1")
                                   || lower == QStringLiteral("yes"));
    } else if (usingCustomConfig && (!resolvedLineNumberFont.isEmpty()
                                     || !resolvedLineNumberFg.isEmpty()
                                     || !resolvedLineNumberBg.isEmpty()
                                     || resolvedLineNumberSize > 0.0)) {
        resolvedCodeLineNumbers = true;
    }
    m_codeLineNumbers = resolvedCodeLineNumbers;

    QUrl resolvedImage;
    if (usingCustomConfig && !backgroundImagePath.isEmpty()) {
        QString expanded = backgroundImagePath;
        if (expanded == QStringLiteral("~"))
            expanded = QDir::homePath();
        else if (expanded.startsWith(QStringLiteral("~/")))
            expanded = QDir::homePath() + expanded.mid(1);
        else if (QDir::isRelativePath(expanded))
            expanded = QFileInfo(customConfig).dir().filePath(expanded);

        const QFileInfo imageInfo(expanded);
        if (imageInfo.exists() && imageInfo.isFile())
            resolvedImage = QUrl::fromLocalFile(imageInfo.absoluteFilePath());
    }
    m_backgroundImage = resolvedImage;

    qreal resolvedOpacity = 0.85;
    if (usingCustomConfig && !backgroundOpacityValue.isEmpty()) {
        bool ok = false;
        QString numText = backgroundOpacityValue.trimmed();
        const bool isPercent = numText.endsWith(QLatin1Char('%'));
        if (isPercent)
            numText.chop(1);
        qreal parsed = numText.trimmed().toDouble(&ok);
        if (ok) {
            if (isPercent)
                parsed /= 100.0;
            resolvedOpacity = qBound(0.0, parsed, 1.0);
        }
    }
    m_backgroundOpacity = resolvedOpacity;

    if (usingCustomConfig) {
        if (!fontValue.isEmpty() && fontValue != m_lastConfigFont) {
            m_lastConfigFont = fontValue;
            if (QFontDatabase::hasFamily(fontValue))
                setEditorFont(fontValue);
        } else if (fontValue.isEmpty()) {
            m_lastConfigFont.clear();
        }

        if (!codeFontValue.isEmpty() && codeFontValue != m_lastConfigCodeFont) {
            m_lastConfigCodeFont = codeFontValue;
            if (QFontDatabase::hasFamily(codeFontValue))
                setCodeFont(codeFontValue);
        } else if (codeFontValue.isEmpty()) {
            m_lastConfigCodeFont.clear();
            setCodeFont(QString());
        }

        if (!zoomValue.isEmpty() && zoomValue != m_lastConfigZoom) {
            m_lastConfigZoom = zoomValue;
            bool ok = false;
            QString numText = zoomValue.trimmed();
            const bool isPercent = numText.endsWith(QLatin1Char('%'));
            if (isPercent)
                numText.chop(1);
            qreal parsed = numText.trimmed().toDouble(&ok);
            if (ok) {
                if (isPercent || parsed > 10.0)
                    parsed /= 100.0;
                if (parsed > 0.0) {
                    const qreal clamped = qBound(textZoomSteps[0], parsed,
                                                 textZoomSteps[textZoomStepCount - 1]);
                    if (!qFuzzyCompare(m_textZoom, clamped)) {
                        m_textZoom = clamped;
                        QSettings().setValue(textZoomSetting, clamped);
                        emit textScaleChanged();
                    }
                }
            }
        } else if (zoomValue.isEmpty()) {
            m_lastConfigZoom.clear();
        }
    } else {
        m_lastConfigFont.clear();
        m_lastConfigCodeFont.clear();
        setCodeFont(QString());
        m_codeLineNumberFont.clear();
        m_codeLineNumberForeground.clear();
        m_codeLineNumberBackground.clear();
        m_codeLineNumberSize = 0.0;
        m_lastConfigZoom.clear();
    }

    if (m_highlighter) {
        m_highlighter->setTheme(m_darkMode, m_themeBackground, m_themeForeground,
                                m_themeAccent, m_themeCodeBackground, m_themeCodeForeground,
                                codeFont());
    }

    applyDocumentTypography(true);

    if (m_darkMode != wasDark)
        emit darkModeChanged();
    const QStringList colors{m_themeBackground, m_themeForeground, m_themeAccent,
                             m_themeSelection, m_themeSelectionForeground,
                             m_themeCodeBackground, m_themeCodeForeground};
    if (colors != previousColors)
        emit themeColorsChanged();
    const bool lineNumbersPropsChanged = (m_codeLineNumbers != previousCodeLineNumbers)
        || (m_codeLineNumberFont != previousCodeLineNumberFont)
        || (m_codeLineNumberForeground != previousCodeLineNumberForeground)
        || (m_codeLineNumberBackground != previousCodeLineNumberBackground)
        || !qFuzzyCompare(m_codeLineNumberSize, previousCodeLineNumberSize);
    if (lineNumbersPropsChanged)
        emit codeLineNumbersChanged();
    if (m_backgroundImage != previousBackgroundImage
            || !qFuzzyCompare(m_backgroundOpacity, previousBackgroundOpacity)) {
        emit backgroundImageChanged();
    }
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const auto watchPathAndParents = [this](const QString &filePath) {
        if (filePath.isEmpty())
            return;
        const QFileInfo info(filePath);
        if (info.exists())
            m_themeWatcher.addPath(info.absoluteFilePath());
        QDir dir = info.dir();
        for (int depth = 0; depth < 3; ++depth) {
            if (dir.exists()) {
                m_themeWatcher.addPath(dir.absolutePath());
                break;
            }
            if (!dir.cdUp())
                break;
        }
    };

    if (!m_explicitConfigPath.isEmpty()) {
        watchPathAndParents(m_explicitConfigPath);
        return;
    }

    watchPathAndParents(defaultConfigPath());

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
    // Only letters and numbers: PCRE2 reads \p{Han} as Script_Extensions, which
    // puts 。 、 「 and the CJK radicals in Han and would count each as a word.
    // One per thread, as loading a file counts here while a worker may too.
    thread_local const QRegularExpression cjkRe(
        QStringLiteral("(?=[\\p{L}\\p{N}])[\\p{Han}\\p{Hiragana}\\p{Katakana}]"));
    thread_local const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));

    // Count unspaced Han and kana characters individually, then replace them
    // so they remain boundaries between surrounding space-delimited words.
    int count = 0;

    QRegularExpressionMatchIterator cjkIt = cjkRe.globalMatch(text);
    while (cjkIt.hasNext()) {
        cjkIt.next();
        ++count;
    }

    QString nonCjkText = text;
    nonCjkText.replace(cjkRe, QStringLiteral(" "));

    QRegularExpressionMatchIterator it = wordRe.globalMatch(nonCjkText);
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
    // One count at a time; edits made while it runs get one more after it.
    if (m_wordCountWatcher.isRunning()) {
        m_wordCountAgain = true;
        return;
    }
    // editorTextChanged keeps m_lastDocumentText current, and handing the
    // worker that shared copy saves reading the whole document out again.
    m_wordCountStale = false;
    m_wordCountWatcher.setFuture(QtConcurrent::run(&Backend::countWords, m_lastDocumentText));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

static bool blockFormatMatches(const QTextBlockFormat &actual, const QTextBlockFormat &target) {
    if (actual.lineHeightType() != target.lineHeightType()
            || !qFuzzyCompare(actual.lineHeight(), target.lineHeight()))
        return false;
    if (actual.background() != target.background())
        return false;
    if (!qFuzzyCompare(actual.leftMargin(), target.leftMargin()))
        return false;
    return true;
}

static bool isCodeBlock(const QTextBlock &block) {
    if (!block.isValid())
        return false;
    if (block.userState() >= MarkdownHighlighter::InsideFence)
        return true;
    const QTextBlock prev = block.previous();
    return prev.isValid() && prev.userState() >= MarkdownHighlighter::InsideFence;
}

QTextBlockFormat Backend::proseBlockFormat() const {
    QTextBlockFormat format;
    format.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);
    format.setBackground(Qt::NoBrush);
    format.setLeftMargin(0.0);
    return format;
}

QTextBlockFormat Backend::codeBlockFormat() const {
    QTextBlockFormat format;
    format.setLineHeight(100.0, QTextBlockFormat::ProportionalHeight);
    format.setBackground(Qt::NoBrush);
    format.setLeftMargin(0.0);
    return format;
}

qreal Backend::codeLineNumberMargin() const {
    const QString family = codeLineNumberFont();
    QFont font = !family.isEmpty()
        ? QFont(family)
        : (m_document ? m_document->defaultFont() : QFont(m_editorFont));
    if (m_codeLineNumberSize > 0.0) {
        font.setPixelSize(qRound(m_codeLineNumberSize * m_desktopTextScale * m_textZoom));
    } else if (font.pixelSize() <= 0) {
        font.setPixelSize(qRound(20 * m_desktopTextScale * m_textZoom));
    }
    const QFontMetricsF fm(font);
    const qreal width = fm.horizontalAdvance(QLatin1String("9999"));
    return width > 0.0 ? width : 40.0;
}

QVariantList Backend::codeBlockPanels() const {
    QVariantList result;
    if (!m_document || !m_document->documentLayout() || !m_hasCodeBlocks)
        return result;

    m_document->documentLayout()->documentSize();

    bool inCode = false;
    qreal startY = 0;
    qreal endY = 0;

    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const int state = block.userState();
        if (state >= MarkdownHighlighter::InsideFence) {
            const QRectF rect = m_document->documentLayout()->blockBoundingRect(block);
            if (!inCode) {
                inCode = true;
                startY = rect.y();
            }
            endY = rect.bottom();
        } else if (inCode) {
            const QRectF rect = m_document->documentLayout()->blockBoundingRect(block);
            endY = rect.bottom();
            inCode = false;

            QVariantMap map;
            map.insert(QStringLiteral("y"), startY);
            map.insert(QStringLiteral("height"), endY - startY);
            result.append(map);
        }
    }

    if (inCode) {
        QVariantMap map;
        map.insert(QStringLiteral("y"), startY);
        map.insert(QStringLiteral("height"), endY - startY);
        result.append(map);
    }

    return result;
}

QVariantList Backend::codeBlockLines() const {
    QVariantList result;
    if (!m_document || !m_document->documentLayout() || !m_codeLineNumbers || !m_hasCodeBlocks)
        return result;

    m_document->documentLayout()->documentSize();

    bool inCode = false;
    int lineNumber = 0;

    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const int state = block.userState();
        if (state >= MarkdownHighlighter::InsideFence) {
            if (!inCode) {
                inCode = true;
                lineNumber = 0;
            } else {
                lineNumber++;
                const QRectF rect = m_document->documentLayout()->blockBoundingRect(block);
                QVariantMap map;
                map.insert(QStringLiteral("y"), rect.y());
                map.insert(QStringLiteral("height"), rect.height());
                map.insert(QStringLiteral("number"), lineNumber);
                result.append(map);
            }
        } else {
            inCode = false;
            lineNumber = 0;
        }
    }

    return result;
}

void Backend::refreshCodeBlockOverlays() {
    if (m_formattingTypography || m_loading || !m_hasCodeBlocks)
        return;

    if (m_codeLineNumbers)
        emit codeBlockLinesChanged();
    emit codeBlockPanelsChanged();
}

void Backend::connectDocumentLayout() {
    if (!m_document)
        return;

    connect(m_document, &QTextDocument::documentLayoutChanged,
            this, &Backend::connectDocumentLayout, Qt::UniqueConnection);

    if (auto *layout = m_document->documentLayout()) {
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged,
                this, &Backend::refreshCodeBlockOverlays, Qt::UniqueConnection);
    }
}

int Backend::countCodeBlocks() const {
    if (!m_document)
        return 0;
    int count = 0;
    bool inCode = false;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        if (block.userState() >= MarkdownHighlighter::InsideFence) {
            if (!inCode) {
                inCode = true;
                ++count;
            }
        } else if (inCode) {
            inCode = false;
        }
    }
    return count;
}

bool Backend::fenceBoundariesChanged() {
    if (m_highlighter && m_highlighter->fenceBoundariesChanged()) {
        m_highlighter->resetFenceBoundariesChanged();
        return true;
    }
    if (m_highlighter)
        m_highlighter->resetFenceBoundariesChanged();

    if (m_hasCodeBlocks && m_lastChangeRemoved >= 3) {
        if (countCodeBlocks() != m_codeBlockCount)
            return true;
    }

    return false;
}

void Backend::applyDocumentTypography(bool preserveUndo) {
    if (!m_document)
        return;

    connectDocumentLayout();

    const QTextBlockFormat proseFmt = proseBlockFormat();
    const QTextBlockFormat codeFmt = codeBlockFormat();

    bool needed = false;
    bool inCode = false;
    int codeBlockCount = 0;

    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        bool isCode = false;
        if (block.userState() >= MarkdownHighlighter::InsideFence) {
            if (!inCode)
                ++codeBlockCount;
            isCode = true;
            inCode = true;
        } else if (inCode) {
            isCode = true;
            inCode = false;
        }

        const QTextBlockFormat &target = isCode ? codeFmt : proseFmt;
        if (!blockFormatMatches(block.blockFormat(), target)) {
            needed = true;
        }
    }

    if (m_highlighter)
        m_highlighter->resetFenceBoundariesChanged();

    const bool wasHasCodeBlocks = m_hasCodeBlocks;
    m_codeBlockCount = codeBlockCount;
    m_hasCodeBlocks = (codeBlockCount > 0);
    const bool shouldEmitOverlays = (m_hasCodeBlocks || wasHasCodeBlocks);

    if (!needed || (preserveUndo && m_document->isRedoAvailable())) {
        if (shouldEmitOverlays) {
            if (m_codeLineNumbers)
                emit codeBlockLinesChanged();
            emit codeBlockPanelsChanged();
        }
        return;
    }

    // A full pass without preserveUndo is only used for freshly loaded/attached
    // documents, so it is safe to drop undo history there (re-enabling clears the
    // stack anyway). Live updates keep undo enabled so earlier edits can still be undone.
    const bool undoEnabled = m_document->isUndoRedoEnabled();
    if (!preserveUndo)
        m_document->setUndoRedoEnabled(false);

    m_formattingTypography = true;
    QTextCursor docCursor(m_document);
    if (preserveUndo)
        docCursor.joinPreviousEditBlock();

    inCode = false;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        bool isCode = false;
        if (block.userState() >= MarkdownHighlighter::InsideFence) {
            isCode = true;
            inCode = true;
        } else if (inCode) {
            isCode = true;
            inCode = false;
        }

        const QTextBlockFormat &target = isCode ? codeFmt : proseFmt;
        if (!blockFormatMatches(block.blockFormat(), target)) {
            QTextCursor cursor(block);
            cursor.setBlockFormat(target);
        }
    }

    if (preserveUndo)
        docCursor.endEditBlock();

    m_formattingTypography = false;

    if (!preserveUndo)
        m_document->setUndoRedoEnabled(undoEnabled);

    if (shouldEmitOverlays) {
        if (m_codeLineNumbers)
            emit codeBlockLinesChanged();
        emit codeBlockPanelsChanged();
    }
}

void Backend::reapplyTypographyToChange() {
    if (!m_document)
        return;

    if (fenceBoundariesChanged()) {
        applyDocumentTypography(true);
        return;
    }

    // Format only the block(s) touched by the last edit instead of the whole
    // document, and fold the change into the preceding edit command so a single
    // undo reverts both the text and its formatting.
    const int maxPos = m_document->characterCount() - 1;
    const int start = qBound(0, m_lastChangePos, maxPos);
    const int end = qBound(start, m_lastChangePos + m_lastChangeAdded, maxPos);

    const QTextBlock startBlock = m_document->findBlock(start);
    const QTextBlock endBlock = m_document->findBlock(end);

    const QTextBlockFormat proseFmt = proseBlockFormat();
    const QTextBlockFormat codeFmt = codeBlockFormat();

    bool needed = false;
    for (QTextBlock block = startBlock;
         block.isValid() && block.position() <= endBlock.position(); block = block.next()) {
        const QTextBlockFormat &target = isCodeBlock(block) ? codeFmt : proseFmt;
        if (!blockFormatMatches(block.blockFormat(), target)) {
            needed = true;
            break;
        }
    }

    if (needed) {
        m_formattingTypography = true;
        QTextCursor cursor(m_document);
        cursor.joinPreviousEditBlock();
        for (QTextBlock block = startBlock;
             block.isValid() && block.position() <= endBlock.position(); block = block.next()) {
            const QTextBlockFormat &target = isCodeBlock(block) ? codeFmt : proseFmt;
            if (!blockFormatMatches(block.blockFormat(), target)) {
                QTextCursor blockCursor(block);
                blockCursor.setBlockFormat(target);
            }
        }
        cursor.endEditBlock();
        m_formattingTypography = false;
    }

    const bool codeParagraphChanged = m_hasCodeBlocks && (isCodeBlock(startBlock) || isCodeBlock(endBlock));
    if (codeParagraphChanged) {
        if (m_codeLineNumbers)
            emit codeBlockLinesChanged();
        emit codeBlockPanelsChanged();
    }
}
