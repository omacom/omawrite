#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

#include "buffersession.h"
#include "workspacesession.h"

class MarkdownHighlighter;
class QTextDocument;
class QWindow;
class QLockFile;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QVariantList buffers READ buffers NOTIFY buffersChanged)
    Q_PROPERTY(QString activeBufferId READ activeBufferId NOTIFY activeBufferChanged)
    Q_PROPERTY(int activeCursorPosition READ activeCursorPosition NOTIFY activeBufferChanged)
    Q_PROPERTY(int activeSelectionStart READ activeSelectionStart NOTIFY activeBufferChanged)
    Q_PROPERTY(int activeSelectionEnd READ activeSelectionEnd NOTIFY activeBufferChanged)
    Q_PROPERTY(QString activeBufferText READ activeBufferText NOTIFY activeBufferChanged)
    Q_PROPERTY(bool restoringActiveBuffer READ restoringActiveBuffer NOTIFY activeBufferChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    Backend(const QString &stateDirectory, QObject *parent = nullptr);
    Backend(WorkspaceSession *workspaceSession, const QString &windowId,
            QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QVariantList buffers() const { return m_workspaceSession
            ? m_workspaceSession->tabs(m_workspaceWindowId) : m_bufferSession.buffers(); }
    QString activeBufferId() const { return m_workspaceSession
            ? m_workspaceSession->activeTabId(m_workspaceWindowId) : m_bufferSession.activeBufferId(); }
    int activeCursorPosition() const { return m_cursorPosition; }
    int activeSelectionStart() const { return m_selectionStart; }
    int activeSelectionEnd() const { return m_selectionEnd; }
    QString activeBufferText() const { return m_activeBufferText; }
    bool restoringActiveBuffer() const { return m_restoringActiveBuffer; }
    QString fileName() const;
    Q_INVOKABLE QString bufferTitle(const QVariantMap &buffer, int index) const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE QString newBuffer();
    Q_INVOKABLE bool selectBuffer(const QString &id);
    Q_INVOKABLE bool moveActiveBuffer(int direction);
    Q_INVOKABLE bool closeActiveBuffer();
    Q_INVOKABLE bool discardActiveBuffer();
    Q_INVOKABLE void prepareForApplicationClose();
    Q_INVOKABLE void finishActiveBufferRestore();
    Q_INVOKABLE void updateActiveEditorState(int cursorPosition, int selectionStart, int selectionEnd);
    Q_INVOKABLE void openDialog();
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
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void openExternalUrl(const QUrl &url);
    void reportExternalChange(bool deleted);
    void refreshBuffers();
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);

signals:
    void fileUrlChanged();
    void buffersChanged();
    void activeBufferChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void closeAfterSave();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void newWindowRequested();
    void openTabRequested(const QString &tabId);
    void windowEmptied();

private:
    void initializeRuntime();
    void loadDocumentText(const QString &text);
    void loadActiveBuffer();
    void persistActiveBuffer();
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
    void writeRecovery();
    void restoreRecovery();
    void clearRecovery();
    QString recoveryPath() const;
    void watchCurrentFile();
    void loadOmarchyTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    qreal m_textScale = 1.0;
    bool m_loading = false;
    bool m_closeAfterSave = false;
    bool m_formattingTypography = false;
    int m_formattedBlockCount = 0;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    int m_cursorPosition = 0;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    QString m_activeBufferText;
    QTimer m_wordCountTimer;
    QTimer m_recoveryTimer;
    QFileSystemWatcher m_fileWatcher;
    BufferSession m_bufferSession;
    WorkspaceSession *m_workspaceSession = nullptr;
    QString m_workspaceWindowId;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    bool m_hasKnownFileContents = false;
    bool m_applicationClosing = false;
    bool m_restoringActiveBuffer = false;
    bool m_ignoringInitialCursorReset = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QFileSystemWatcher m_themeWatcher;
};
