#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class MarkdownHighlighter;
class QTextDocument;
class QWindow;
class QLockFile;

// Owns everything that is per-document: file I/O, modified/word-count/status
// state, crash recovery, external-change detection, and typography. Backend
// owns a list of these and forwards its scalar Q_PROPERTYs to the active one.
class DocumentTab : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)

public:
    explicit DocumentTab(QObject *parent = nullptr);
    ~DocumentTab() override;

    void setParentWindow(QWindow *window);
    void applyTheme(bool darkMode, const QString &background, const QString &foreground,
                    const QString &accent);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;
    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    // A tab is "named" once it has a real file path -- that is what makes it
    // eligible for autosave-to-disk instead of only an internal snapshot.
    bool isNamed() const { return !m_fileUrl.isEmpty() && m_fileUrl.isValid(); }
    // Base name (e.g. "untitled-3") of this tab's own recovery/untitled
    // snapshot slot, for the session manifest to reference.
    QString untitledSlotName() const;
    int cursorPosition() const { return m_cursorPosition; }
    int selectionStart() const { return m_selectionStart; }
    int selectionEnd() const { return m_selectionEnd; }

    // Called by Backend, from parsed session/legacy-recovery data, before the
    // QML editor attaches its QTextDocument. Applied once, inside attachDocument().
    void primeUntitledRestore(const QString &text, int cursor, int selectionStart,
                              int selectionEnd);
    void primeNamedRestore(const QUrl &fileUrl, int cursor, int selectionStart,
                           int selectionEnd);
    void primeNamedRestoreWithPendingEdits(const QUrl &fileUrl, const QString &text);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void open(const QUrl &url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveForClose();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE void saveAs(const QUrl &url);
    Q_INVOKABLE void fileDialogCanceled();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void reloadFromDisk();
    Q_INVOKABLE void keepExternalVersion();
    Q_INVOKABLE void printDocument();
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void updateCursorState(int cursor, int selectionStart, int selectionEnd);
    // Returns {"valid": bool, "cursor": int, "selectionStart": int,
    // "selectionEnd": int} for a cursor/selection restored via primeUntitledRestore
    // / primeNamedRestore; only valid once, right after attachDocument().
    Q_INVOKABLE QVariantMap consumePendingCursorRestore();

    // Lets Backend surface a process/window-level status (e.g. "could not
    // open a new window") through the active tab's status property.
    void reportStatus(const QString &status) { setStatus(status); }

    // Forces a pending debounced autosave to run immediately, e.g. right
    // before the tab or window closes so the last edit isn't dropped.
    void flushPendingAutosave();

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void closeAfterSave();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);

private:
    enum class RestoreKind { None, Untitled, Named, NamedWithPendingEdits };

    void loadDocumentText(const QString &text);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    void applyDocumentTypography();
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void onAutosaveTimeout();
    void writeRecovery();
    void applyPendingRestore();
    void clearRecovery();
    QString recoveryPath() const;
    void watchCurrentFile();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_loading = false;
    bool m_closeAfterSave = false;
    bool m_formattingTypography = false;
    int m_formattedBlockCount = 0;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    QTimer m_wordCountTimer;
    QTimer m_recoveryTimer;
    QFileSystemWatcher m_fileWatcher;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    bool m_hasKnownFileContents = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    RestoreKind m_pendingRestoreKind = RestoreKind::None;
    QUrl m_pendingFileUrl;
    QString m_pendingText;
    int m_pendingCursor = -1;
    int m_pendingSelectionStart = -1;
    int m_pendingSelectionEnd = -1;
    bool m_hasCursorToConsume = false;
    int m_cursorPosition = 0;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;

    bool m_themeDarkMode = true;
    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
};
