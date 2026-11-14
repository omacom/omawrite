#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>
#include <functional>

class MarkdownHighlighter;
class SpellingController;
class SpellingDictionary;
class QFont;
class QTextDocument;
class QWindow;
class QLockFile;
class QPageLayout;
namespace Printing { struct Job; }

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool routesFileOpen READ routesFileOpen NOTIFY routesFileOpenChanged)
    Q_PROPERTY(int selectionWordCount READ selectionWordCount NOTIFY selectionWordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelectionForeground READ themeSelectionForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themePanelBackground READ themePanelBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeCodeBackground READ themePanelBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString editorFont READ editorFont WRITE setEditorFont NOTIFY editorFontChanged)
    Q_PROPERTY(QStringList availableFonts READ availableFonts CONSTANT)
    Q_PROPERTY(QObject *spelling READ spelling CONSTANT)
    Q_PROPERTY(bool clipboardImageAvailable READ clipboardImageAvailable NOTIFY clipboardChanged)

public:
    explicit Backend(QObject *parent = nullptr, bool recoverDraft = true);
    // Deterministic dictionary seam for isolated source/action integration tests.
    explicit Backend(std::unique_ptr<SpellingDictionary> dictionary, QObject *parent = nullptr, bool recoverDraft = true);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    int selectionWordCount() const { return m_selectionWordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    // What the desktop asks for, times what the user asked for on top of it.
    qreal textScale() const { return m_desktopTextScale * m_textZoom; }
    void setDesktopTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const { return readableOn(m_themeAccent); }
    QString themeSelection() const { return m_themeSelection; }
    QString themePanelBackground() const { return m_themePanelBackground; }
    QString themeCodeBackground() const { return m_themePanelBackground; } // Feature compatibility alias.
    QString themeSelectionForeground() const { return m_themeSelectionForeground; }
    QString editorFont() const { return m_editorFont; }
    void setEditorFont(const QString &family);
    QStringList availableFonts() const;
    static QString defaultEditorFont();
    static QStringList selectableFontFamilies(const QStringList &families);
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString untitledName();
    static QString tomlValue(const QString &text);
    static QString readableOn(const QString &color);
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

    QObject *spelling() const;
    Q_INVOKABLE bool replaceSpelling(const QVariantMap &token, const QString &replacement);
    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void open(const QUrl &url);
    void openOrStartFile(const QUrl &url);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAsDialog();
    Q_INVOKABLE void saveAs(const QUrl &url);
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void reloadFromDisk();
    Q_INVOKABLE void keepExternalVersion();
    Q_INVOKABLE void increaseTextSize();
    Q_INVOKABLE void decreaseTextSize();
    Q_INVOKABLE void resetTextSize();
    Q_INVOKABLE void holdAutoSave(bool held);
    Q_INVOKABLE void printDocument();
    Printing::Job printJob(const QPageLayout &layout, bool appendices = true) const;
    Q_INVOKABLE void copyAsHtml(int start = -1, int end = -1, QObject *renderedDocument = nullptr, bool plainMarkup = true);
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void finishClosing();
    bool hasRecoverySnapshot() const;
    bool hasRecoveryStorage() const { return !m_recoveryPath.isEmpty(); }
    bool routesFileOpen() const { return bool(m_openRouter); }
    void setOpenRouter(std::function<void(const QUrl &)> router);
    void setWriteGuard(std::function<bool(const QUrl &)> guard) { m_writeGuard = std::move(guard); }
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    bool clipboardImageAvailable() const;
    // Returns Markdown for the caller to insert in the editor's undo stack.
    Q_INVOKABLE QString pasteClipboardImage(const QString &proposedRelativePath = QString());
    // Local files only; an empty URL means unavailable or disallowed.
    Q_INVOKABLE QUrl resolveImageUrl(const QString &path) const;
    Q_INVOKABLE bool copyImage(const QString &path);
    // Current document only. Copy, never delete: undo and other documents stay safe.
    Q_INVOKABLE bool renameAttachment(const QString &oldPath, const QString &newRelativePath);
    Q_INVOKABLE bool locateAttachment(const QString &oldPath, const QUrl &localFile,
                                     const QString &newRelativePath = QString());
    Q_INVOKABLE QString documentText() const { return currentDocumentText(); }
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE void setCaretPosition(int position);
    Q_INVOKABLE QString linkUrlAt(int position) const;
    Q_INVOKABLE QVariantMap emphasisSpanAt(int position, const QString &marker) const;
    Q_INVOKABLE int cursorPositionStep(int position, int direction) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE QVariantList searchMatches(const QString &query) const;
    Q_INVOKABLE void replaceRange(int start, int end, const QString &text);
    Q_INVOKABLE bool insideFence(int position) const;
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);
    Q_INVOKABLE void documentFontChanged();
    Q_INVOKABLE void countSelectedText(const QString &text);
    Q_INVOKABLE QVariantMap readingPosition(const QUrl &url) const;
    Q_INVOKABLE void rememberReadingPosition(const QUrl &url, qreal ratio, bool readMode, int caret,
                                              int anchor = -1, qreal offset = 0);
    Q_INVOKABLE bool revealCurrentFile();

public slots:
    // The untitled name follows the language.
    void retranslate();

signals:
    void clipboardChanged();
    void routesFileOpenChanged();
    void newWindowRequested();
    void fileUrlChanged();
    void attachmentUrlsChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void selectionWordCountChanged();
    void documentAboutToReplace();
    void documentLoaded();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void editorFontChanged();
    void saveFailed();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);

private:
    friend class OmawriteEditorTest;
    friend class OmawriteAttachmentsTest;
    friend class OmawriteTest;

    void loadDocumentText(const QString &text);
    qreal steppedTextZoom(int direction) const;
    void setTextZoom(qreal zoom);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void reportPrintFailure(const QString &message);
    void saveTo(const QUrl &url);
    QString attachmentStoragePath() const;
    bool materializeAttachments(const QUrl &destination, QString *error) const;
    bool updateAttachment(const QString &oldPath, const QString &newPath, const QString &source);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void refreshSelectionWordCount();
    static QString positionKey(const QUrl &url);
    static QString explorerRevealArguments(QString path);
    void scheduleWordCount();
    void applyDocumentTypography();
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void writeRecovery();
    void scheduleAutoSave();
    void autoSave();
    void suspendAutoSave();
    void restoreRecovery();
    void clearRecovery();
    bool diskMatchesKnownContents() const;
    void reportExternalChange(bool deleted);
    static bool savesUnchanged(const QByteArray &raw);
    void setKnownFileContents(const QByteArray &contents, bool known);
    void unwatchFiles();
    void watchCurrentFile();
    void checkWatchedFile();
    void loadOmarchyTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    int m_selectionWordCount = 0;
    QString m_selectionText;
    QFutureWatcher<int> m_selectionCountWatcher;
    quint64 m_selectionGeneration = 0, m_selectionJobGeneration = 0;
    bool m_selectionCounting = false;
    bool m_darkMode = true;
    // The desktop's light/dark setting, used when the Omarchy theme names none.
    bool m_systemDarkMode = true;
    qreal m_desktopTextScale = 1.0;
    qreal m_textZoom = 1.0;
    bool m_loading = false;
    bool m_formattingTypography = false;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    QTimer m_wordCountTimer;
    // Counting a large document stalls typing, so counts run on a worker
    // thread. A load outdates the count in flight, and its result is dropped.
    QFutureWatcher<int> m_wordCountWatcher;
    bool m_wordCountStale = false;
    bool m_wordCountAgain = false;
    QTimer m_recoveryTimer;
    QTimer m_autoSaveTimer;
    QFileSystemWatcher m_fileWatcher;
    QTimer m_fileCheckTimer;
    bool m_fileMissing = false;
    QPointer<QTextDocument> m_document;
    QPointer<QWindow> m_parentWindow;
    QPointer<MarkdownHighlighter> m_highlighter;
    SpellingController *m_spelling = nullptr;
    int m_caretPosition = 0;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    // Conflict fingerprint only; never an accepted baseline for auto-save.
    QByteArray m_lastReportedFileState;
    bool m_autoSaveSuspended = false;
    bool m_autoSaveHeld = false;
    bool m_recoverDraft = true;
    bool m_bypassRouting = false;
    std::function<void(const QUrl &)> m_openRouter;
    std::function<bool(const QUrl &)> m_writeGuard;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;
    QMap<QString, QString> m_stagedAttachments; // logical relative path -> immutable storage ID
    QSet<QString> m_materializedAttachments; // use the current document's copy after a successful save

    // Capture the theme root independently of the OS home-directory API.
    // Tests redirect this private path, never the user's process environment.
    QString m_themeHome;
    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themePanelBackground;
    QString m_themeSelectionForeground;
    QString m_editorFont;
    QFileSystemWatcher m_themeWatcher;
    QTimer m_themeReloadTimer;
};
