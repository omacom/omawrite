#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QString>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MarkdownHighlighter;
class QTextDocument;
class QWindow;
class QLockFile;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale NOTIFY textScaleChanged)
    Q_PROPERTY(bool vimMode READ vimMode WRITE setVimMode NOTIFY vimModeChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal textScale);
    bool vimMode() const { return m_vimMode; }
    void setVimMode(bool vimMode);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    static int countWords(const QString &text);
    Q_INVOKABLE static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
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
    Q_INVOKABLE QString clipboardUrl(bool selection = false) const;
    Q_INVOKABLE QString clipboardText(bool selection = false) const;
    Q_INVOKABLE void setClipboardText(const QString &text, bool selection = false) const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE void beginHistoryNavigation();
    Q_INVOKABLE void endHistoryNavigation();
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void openExternalUrl(const QUrl &url);
    Q_INVOKABLE QUrl resolvePath(const QString &path) const;
    Q_INVOKABLE void beginEditBlock();
    Q_INVOKABLE void endEditBlock();
    // The document as it stands this instant. TextEdit caches its text and
    // refreshes it when the document emits a change, which an open edit block
    // holds back, so a command partway through its own edits must ask here.
    Q_INVOKABLE QString documentText() const { return currentDocumentText(); }
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void vimModeChanged();
    void themeColorsChanged();
    void closeAfterSave();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void documentLoaded();
    void externalChangeDetected(bool deleted, bool locallyModified);

private:
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
    void joinTypographyEdit(int start, int end);
    void reapplyTypographyToChange();
    int lastChangeEdge() const;
    bool lastChangeEndedAWord() const;
    void endUndoRun();
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
    bool m_vimMode = false;
    int m_editBlockDepth = 0;
    QTextCursor m_editBlockCursor;
    bool m_loading = false;
    bool m_closeAfterSave = false;
    bool m_formattingTypography = false;
    bool m_navigatingHistory = false;
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

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QFileSystemWatcher m_themeWatcher;
};
