#include "documenttab.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QWindow>

#include "backend.h"
#include "markdownhighlighter.h"

constexpr qreal typoraLineHeightPercent = 140;
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");

DocumentTab::DocumentTab(QObject *parent) : QObject(parent) {
    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash is still recovered even if another tab/window exited normally.
    for (int pass = 0; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("untitled-%1").arg(slot));
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
    connect(&m_wordCountTimer, &QTimer::timeout, this, &DocumentTab::refreshWordCount);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &DocumentTab::onAutosaveTimeout);
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
}

DocumentTab::~DocumentTab() = default;

void DocumentTab::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

void DocumentTab::applyTheme(bool darkMode, const QString &background, const QString &foreground,
                             const QString &accent) {
    m_themeDarkMode = darkMode;
    m_themeBackground = background;
    m_themeForeground = foreground;
    m_themeAccent = accent;
    if (m_highlighter) {
        m_highlighter->setDarkMode(m_themeDarkMode);
        m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);
    }
}

QString DocumentTab::fileName() const {
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

void DocumentTab::attachDocument(QObject *textDocument) {
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
    m_highlighter->setDarkMode(m_themeDarkMode);
    m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);

    connect(m_document, &QTextDocument::contentsChange, this,
            [this](int position, int, int charsAdded) {
                if (m_formattingTypography || m_loading)
                    return;
                m_lastChangePos = position;
                m_lastChangeAdded = charsAdded;
            });

    applyDocumentTypography();
    applyPendingRestore();
}

void DocumentTab::open(const QUrl &url) {
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

void DocumentTab::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void DocumentTab::saveForClose() {
    if (!m_modified) {
        emit closeAfterSave();
        return;
    }

    m_closeAfterSave = true;
    save();
}

void DocumentTab::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void DocumentTab::saveAs(const QUrl &url) {
    saveTo(url);
}

void DocumentTab::fileDialogCanceled() {
    m_closeAfterSave = false;
}

void DocumentTab::discardRecovery() {
    clearRecovery();
}

void DocumentTab::reloadFromDisk() {
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void DocumentTab::keepExternalVersion() {
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

void DocumentTab::printDocument() {
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

bool DocumentTab::editorTextChanged() {
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

QVariantList DocumentTab::hiddenRangesAt(int position) const {
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

void DocumentTab::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

void DocumentTab::loadDocumentText(const QString &text) {
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
    setWordCount(Backend::countWords(text));
}

void DocumentTab::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
    watchCurrentFile();
}

void DocumentTab::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void DocumentTab::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void DocumentTab::saveTo(const QUrl &url) {
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

void DocumentTab::scheduleRecovery() {
    m_recoveryTimer.start();
}

// Named documents autosave straight to their real file, the same path Ctrl+S
// uses; untitled documents have nowhere safe to write, so they only get an
// internal recovery snapshot until the user gives them a name.
void DocumentTab::onAutosaveTimeout() {
    if (!m_modified)
        return;

    if (!m_fileUrl.isEmpty() && m_fileUrl.isValid())
        saveTo(m_fileUrl);
    else
        writeRecovery();
}

QString DocumentTab::recoveryPath() const {
    return m_recoveryPath;
}

void DocumentTab::writeRecovery() {
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

void DocumentTab::primeUntitledRestore(const QString &text, int cursor, int selectionStart,
                                       int selectionEnd) {
    m_pendingRestoreKind = RestoreKind::Untitled;
    m_pendingText = text;
    m_pendingCursor = cursor;
    m_pendingSelectionStart = selectionStart;
    m_pendingSelectionEnd = selectionEnd;
}

void DocumentTab::primeNamedRestore(const QUrl &fileUrl, int cursor, int selectionStart,
                                    int selectionEnd) {
    m_pendingRestoreKind = RestoreKind::Named;
    m_pendingFileUrl = fileUrl;
    m_pendingCursor = cursor;
    m_pendingSelectionStart = selectionStart;
    m_pendingSelectionEnd = selectionEnd;
}

void DocumentTab::primeNamedRestoreWithPendingEdits(const QUrl &fileUrl, const QString &text) {
    m_pendingRestoreKind = RestoreKind::NamedWithPendingEdits;
    m_pendingFileUrl = fileUrl;
    m_pendingText = text;
    m_pendingCursor = -1;
    m_pendingSelectionStart = -1;
    m_pendingSelectionEnd = -1;
}

void DocumentTab::applyPendingRestore() {
    const RestoreKind kind = m_pendingRestoreKind;
    m_pendingRestoreKind = RestoreKind::None;

    switch (kind) {
    case RestoreKind::None:
        return;
    case RestoreKind::Untitled:
        loadDocumentText(m_pendingText);
        setModified(true);
        setStatus(QStringLiteral("Recovered unsaved changes"));
        break;
    case RestoreKind::Named:
        open(m_pendingFileUrl);
        break;
    case RestoreKind::NamedWithPendingEdits: {
        QFile diskFile(m_pendingFileUrl.toLocalFile());
        if (m_pendingFileUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly)) {
            m_lastKnownFileContents = diskFile.readAll();
            m_hasKnownFileContents = true;
        } else {
            m_lastKnownFileContents.clear();
            m_hasKnownFileContents = false;
        }
        loadDocumentText(m_pendingText);
        setFileUrl(m_pendingFileUrl);
        watchCurrentFile();
        setModified(true);
        setStatus(QStringLiteral("Recovered unsaved changes"));
        break;
    }
    }

    if (m_pendingCursor >= 0) {
        m_hasCursorToConsume = true;
        m_cursorPosition = m_pendingCursor;
        m_selectionStart = qMax(0, m_pendingSelectionStart);
        m_selectionEnd = qMax(0, m_pendingSelectionEnd);
    }
}

void DocumentTab::flushPendingAutosave() {
    if (m_recoveryTimer.isActive()) {
        m_recoveryTimer.stop();
        onAutosaveTimeout();
    }
}

QString DocumentTab::untitledSlotName() const {
    if (m_recoveryPath.isEmpty())
        return {};
    return QFileInfo(m_recoveryPath).completeBaseName();
}

void DocumentTab::updateCursorState(int cursor, int selectionStart, int selectionEnd) {
    m_cursorPosition = cursor;
    m_selectionStart = selectionStart;
    m_selectionEnd = selectionEnd;
}

QVariantMap DocumentTab::consumePendingCursorRestore() {
    if (!m_hasCursorToConsume)
        return QVariantMap{{QStringLiteral("valid"), false}};

    m_hasCursorToConsume = false;
    return QVariantMap{{QStringLiteral("valid"), true},
                       {QStringLiteral("cursor"), m_cursorPosition},
                       {QStringLiteral("selectionStart"), m_selectionStart},
                       {QStringLiteral("selectionEnd"), m_selectionEnd}};
}

void DocumentTab::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(recoveryPath());
}

void DocumentTab::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        m_fileWatcher.addPath(m_fileUrl.toLocalFile());
}

QUrl DocumentTab::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(Backend::suggestedFileName(currentDocumentText())));
}

QString DocumentTab::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

void DocumentTab::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void DocumentTab::refreshWordCount() {
    setWordCount(Backend::countWords(currentDocumentText()));
}

void DocumentTab::scheduleWordCount() {
    m_wordCountTimer.start();
}

void DocumentTab::applyDocumentTypography() {
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

void DocumentTab::reapplyTypographyToChange() {
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
