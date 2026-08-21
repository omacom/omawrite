#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
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
const QString browseDirectorySetting = QStringLiteral("file/browseDirectory");
const QString sidebarWidthSetting = QStringLiteral("window/sidebarWidth");

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
    m_persistTimer.setSingleShot(true);
    m_persistTimer.setInterval(750);
    connect(&m_persistTimer, &QTimer::timeout, this, &Backend::persistDocument);
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

                m_externalChangePending = true;
                emit externalChangeDetected(deleted, m_modified);
                // A replacement leaves the old inode behind, and the path with
                // it, so re-arm or a second change would never be noticed.
                watchCurrentFile();
            });

    connect(&m_folderWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this]() { emit folderChanged(); });
    const QString remembered = QSettings().value(browseDirectorySetting).toString();
    applyFolder(QDir(remembered).exists() ? remembered
                                          : defaultDirectory().absolutePath(),
                false);

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

void Backend::setFolder(const QUrl &url) {
    if (!url.isLocalFile())
        return;

    applyFolder(url.toLocalFile(), true);
}

void Backend::openParentFolder() {
    QDir directory(m_folderUrl.toLocalFile());
    if (!directory.cdUp())
        return;

    applyFolder(directory.absolutePath(), true);
}

QUrl Backend::createDocument(const QString &name) {
    const QDir directory(m_folderUrl.toLocalFile());
    const QString fileName = suggestedFileName(name);
    const QString path = directory.filePath(fileName);
    if (QFileInfo::exists(path)) {
        setStatus(QStringLiteral("%1 already exists.").arg(fileName));
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setStatus(QStringLiteral("Could not create %1.").arg(fileName));
        return {};
    }
    file.close();

    setStatus(QStringLiteral("Created %1").arg(fileName));
    // The folder watcher reports this too, but not before the new row is
    // wanted on screen.
    emit folderChanged();
    return QUrl::fromLocalFile(path);
}

QUrl Backend::createFolder(const QString &name) {
    QDir directory(m_folderUrl.toLocalFile());
    const QString folderName = sanitizedEntryName(name);
    if (directory.exists(folderName)) {
        setStatus(QStringLiteral("%1 already exists.").arg(folderName));
        return {};
    }

    if (!directory.mkdir(folderName)) {
        setStatus(QStringLiteral("Could not create %1.").arg(folderName));
        return {};
    }

    setStatus(QStringLiteral("Created %1").arg(folderName));
    emit folderChanged();
    return QUrl::fromLocalFile(directory.filePath(folderName));
}

int Backend::sidebarWidth() const {
    return QSettings().value(sidebarWidthSetting, 240).toInt();
}

void Backend::saveSidebarWidth(int width) {
    QSettings().setValue(sidebarWidthSetting, width);
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
    // Only now, with the new text in hand: whatever was contested belonged to
    // the document being replaced, and an open that failed replaces nothing.
    m_externalChangePending = false;
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

void Backend::saveNow() {
    m_persistTimer.stop();
    persistDocument();
}

// The file the document belongs in: its own if it has one, one named from its
// first line if it does not. False when the write did not land there.
bool Backend::saveToItsOwnFile() {
    if (!m_modified)
        return true;

    if (m_fileUrl.isLocalFile())
        return saveTo(m_fileUrl);

    const QString text = currentDocumentText();
    if (text.trimmed().isEmpty()) {
        clearRecovery();
        setModified(false);
        return true;
    }

    return saveTo(unusedDocumentUrl(suggestedFileName(text)));
}

// Switching documents asks the strict question: the work has to have reached
// the file it belongs in, because the writer is about to lose sight of it.
bool Backend::saveBeforeLeaving() {
    m_persistTimer.stop();

    if (saveToItsOwnFile())
        return true;

    // Worth a draft even though the switch is declined.
    writeRecovery();
    return false;
}

// Closing asks the weaker one: anywhere at all will do. It has to be the draft
// written for this attempt, though — an older one on disk proves only that
// something was saved once, not that it holds what is on screen now.
bool Backend::saveBeforeClosing() {
    m_persistTimer.stop();

    if (saveToItsOwnFile() || writeRecovery())
        return true;

    setStatus(QStringLiteral("Could not save %1 anywhere; close again to discard.")
                  .arg(fileName()));
    return false;
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
    if (!m_fileUrl.isLocalFile())
        return;

    open(m_fileUrl);

    // A reload that did not happen has answered nothing, and the prompt that
    // asked has already closed itself. Ask again rather than leave the guard
    // standing with nothing able to clear it.
    if (m_externalChangePending)
        emit externalChangeDetected(!QFileInfo::exists(m_fileUrl.toLocalFile()),
                                    m_modified);
}

void Backend::keepExternalVersion() {
    m_externalChangePending = false;
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        m_lastKnownFileContents = file.readAll();
        m_hasKnownFileContents = true;
    } else {
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
    }
    setModified(true);
    schedulePersist();
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
    schedulePersist();
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
    emit documentLoaded();
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
    watchCurrentFile();
    if (m_fileUrl.isLocalFile())
        applyFolder(QFileInfo(m_fileUrl.toLocalFile()).absolutePath(), true);
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

bool Backend::saveTo(const QUrl &url) {
    // The prompt is on screen asking which version to keep, so the file is not
    // ours to write until it is answered. Saving somewhere else is still fine.
    if (m_externalChangePending && url == m_fileUrl) {
        setStatus(QStringLiteral("%1 changed on disk; answer that first.")
                      .arg(fileName()));
        return false;
    }

    if (!url.isLocalFile()) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Only local files can be saved."));
        return false;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        return false;
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
        return false;
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

    return true;
}

void Backend::schedulePersist() {
    m_persistTimer.start();
}

// A named document is written to its file. Anything that stops that — an
// unwritable file, or an outside change the writer has not answered yet, which
// is not ours to overwrite — falls back to the recovery draft, so quitting
// after a failed save still comes back.
void Backend::persistDocument() {
    if (!m_modified)
        return;

    if (m_fileUrl.isLocalFile() && saveTo(m_fileUrl))
        return;

    writeRecovery();
}

QString Backend::recoveryPath() const {
    return m_recoveryPath;
}

bool Backend::writeRecovery() {
    if (!m_modified)
        return true;
    const QString path = recoveryPath();
    if (path.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("text"), currentDocumentText()}};
    file.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
    return file.commit();
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
    m_persistTimer.stop();
    QFile::remove(recoveryPath());
}

void Backend::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        m_fileWatcher.addPath(m_fileUrl.toLocalFile());
}

void Backend::applyFolder(const QString &path, bool remember) {
    const QDir directory = QDir(path).exists() ? QDir(path) : defaultDirectory();
    const QUrl folderUrl = QUrl::fromLocalFile(directory.absolutePath());
    if (m_folderUrl == folderUrl)
        return;

    m_folderUrl = folderUrl;
    watchCurrentFolder();
    if (remember)
        QSettings().setValue(browseDirectorySetting, directory.absolutePath());
    emit folderChanged();
}

void Backend::watchCurrentFolder() {
    const QStringList watched = m_folderWatcher.directories();
    if (!watched.isEmpty())
        m_folderWatcher.removePaths(watched);
    if (m_folderUrl.isLocalFile())
        m_folderWatcher.addPath(m_folderUrl.toLocalFile());
}

QString Backend::folderName() const {
    const QDir directory(m_folderUrl.toLocalFile());
    // The root directory has no name of its own; show its path instead.
    return directory.isRoot() ? directory.absolutePath() : directory.dirName();
}

bool Backend::folderHasParent() const {
    return !QDir(m_folderUrl.toLocalFile()).isRoot();
}

QVariantList Backend::folderEntries() const {
    static const QStringList markdownFilter{QStringLiteral("*.md"),
                                            QStringLiteral("*.markdown")};
    // AllDirs exempts folders from the name filter so an empty one can still
    // be walked into, while files stay narrowed to what Omawrite can open:
    // this is a view of a writing folder, not a file manager.
    QVariantList entries;
    const QFileInfoList infos = QDir(m_folderUrl.toLocalFile())
        .entryInfoList(markdownFilter, QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot,
                       QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    entries.reserve(infos.size());
    for (const QFileInfo &info : infos) {
        entries.append(QVariantMap{
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("url"), QUrl::fromLocalFile(info.absoluteFilePath())},
            {QStringLiteral("isDir"), info.isDir()}});
    }
    return entries;
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

// A save cannot report a clash the way creating a document does, so the name
// gives way instead.
QUrl Backend::unusedDocumentUrl(const QString &fileName) const {
    const QDir directory(m_folderUrl.toLocalFile());
    if (!QFileInfo::exists(directory.filePath(fileName)))
        return QUrl::fromLocalFile(directory.filePath(fileName));

    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QLatin1Char('.') + info.suffix();
    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QStringLiteral("%1 %2%3").arg(base).arg(n).arg(suffix);
        if (!QFileInfo::exists(directory.filePath(candidate)))
            return QUrl::fromLocalFile(directory.filePath(candidate));
    }
    return QUrl::fromLocalFile(directory.filePath(fileName));
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    return QUrl::fromLocalFile(
        defaultDirectory().filePath(suggestedFileName(currentDocumentText())));
}

QDir Backend::defaultDirectory() const {
    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    return savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
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

QString Backend::sanitizedEntryName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    return name;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = sanitizedEntryName(text);
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
