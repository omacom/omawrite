#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QTextBlockFormat>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

class MarkdownHighlighter;
class QFont;
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
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeCodeBackground READ themeCodeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeCodeForeground READ themeCodeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(bool codeLineNumbers READ codeLineNumbers NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(qreal codeLineNumberMargin READ codeLineNumberMargin NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(QVariantList codeBlockLines READ codeBlockLines NOTIFY codeBlockLinesChanged)
    Q_PROPERTY(QString codeLineNumberFont READ codeLineNumberFont NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(QString codeLineNumberForeground READ codeLineNumberForeground NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(QString codeLineNumberBackground READ codeLineNumberBackground NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(qreal codeLineNumberSize READ codeLineNumberSize NOTIFY codeLineNumbersChanged)
    Q_PROPERTY(QString themeSelectionForeground READ themeSelectionForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QUrl backgroundImage READ backgroundImage NOTIFY backgroundImageChanged)
    Q_PROPERTY(qreal backgroundOpacity READ backgroundOpacity NOTIFY backgroundImageChanged)
    Q_PROPERTY(QString editorFont READ editorFont WRITE setEditorFont NOTIFY editorFontChanged)
    Q_PROPERTY(QString codeFont READ codeFont WRITE setCodeFont NOTIFY codeFontChanged)
    Q_PROPERTY(QVariantList codeBlockPanels READ codeBlockPanels NOTIFY codeBlockPanelsChanged)
    Q_PROPERTY(QStringList availableFonts READ availableFonts CONSTANT)

public:
    explicit Backend(QObject *parent = nullptr);
    explicit Backend(const QString &configPath, QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);
    void setConfigPath(const QString &configPath);
    QString configPath() const { return m_explicitConfigPath; }
    static QString defaultConfigPath();
    static QString generateSystemConfigToml(bool systemDarkMode = true);
    static int generateDefaultConfigFile(bool systemDarkMode = true,
                                         QString *outputMessage = nullptr);

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    // What the desktop asks for, times what the user asked for on top of it.
    qreal textScale() const { return m_desktopTextScale * m_textZoom; }
    void setDesktopTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QString themeCodeBackground() const { return m_themeCodeBackground; }
    QString themeCodeForeground() const { return m_themeCodeForeground; }
    bool codeLineNumbers() const { return m_codeLineNumbers; }
    qreal codeLineNumberMargin() const;
    Q_INVOKABLE QVariantList codeBlockLines() const;
    QString codeLineNumberFont() const {
        return !m_codeLineNumberFont.isEmpty() ? m_codeLineNumberFont : codeFont();
    }
    QString codeLineNumberForeground() const { return m_codeLineNumberForeground; }
    QString codeLineNumberBackground() const { return m_codeLineNumberBackground; }
    qreal codeLineNumberSize() const { return m_codeLineNumberSize; }
    void setCodeLineNumberFont(const QString &family);
    void setCodeLineNumberForeground(const QString &color);
    void setCodeLineNumberBackground(const QString &color);
    void setCodeLineNumberSize(qreal size);
    QString themeSelectionForeground() const { return m_themeSelectionForeground; }
    QUrl backgroundImage() const { return m_backgroundImage; }
    qreal backgroundOpacity() const { return m_backgroundOpacity; }
    QString editorFont() const { return m_editorFont; }
    void setEditorFont(const QString &family);
    QString codeFont() const { return !m_codeFont.isEmpty() ? m_codeFont : m_editorFont; }
    void setCodeFont(const QString &family);
    Q_INVOKABLE QVariantList codeBlockPanels() const;
    QStringList availableFonts() const;
    static QString defaultEditorFont();
    static QStringList selectableFontFamilies(const QStringList &families);
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString tomlValue(const QString &text);
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

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
    Q_INVOKABLE void copyAsHtml();
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
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
    Q_INVOKABLE void refreshCodeBlockOverlays();

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void backgroundImageChanged();
    void editorFontChanged();
    void saveFailed();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void codeLineNumbersChanged();
    void codeBlockLinesChanged();
    void codeBlockPanelsChanged();
    void codeFontChanged();

private:
    friend class OmawriteTest;

    void loadDocumentText(const QString &text);
    qreal steppedTextZoom(int direction) const;
    void setTextZoom(qreal zoom);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    QTextBlockFormat proseBlockFormat() const;
    QTextBlockFormat codeBlockFormat() const;
    void connectDocumentLayout();
    int countCodeBlocks() const;
    bool fenceBoundariesChanged();
    void applyDocumentTypography(bool preserveUndo = false);
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
    QString activeConfigPath() const;
    void loadOmarchyTheme();
    void watchOmarchyTheme();

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    // The desktop's light/dark setting, used when the Omarchy theme names none.
    bool m_systemDarkMode = true;
    qreal m_desktopTextScale = 1.0;
    qreal m_textZoom = 1.0;
    bool m_loading = false;
    bool m_formattingTypography = false;
    int m_lastChangePos = 0;
    int m_lastChangeAdded = 0;
    int m_lastChangeRemoved = 0;
    bool m_hasCodeBlocks = false;
    int m_codeBlockCount = 0;
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
    int m_caretPosition = 0;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    bool m_autoSaveSuspended = false;
    bool m_autoSaveHeld = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    QString m_explicitConfigPath;
    QString m_lastConfigFont;
    QString m_lastConfigCodeFont;
    QString m_lastConfigZoom;
    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themeCodeBackground;
    QString m_themeCodeForeground;
    bool m_codeLineNumbers = false;
    QString m_codeLineNumberFont;
    QString m_codeLineNumberForeground;
    QString m_codeLineNumberBackground;
    qreal m_codeLineNumberSize = 0.0;
    QString m_themeSelectionForeground;
    QUrl m_backgroundImage;
    qreal m_backgroundOpacity = 0.85;
    QString m_editorFont;
    QString m_codeFont;
    QFileSystemWatcher m_themeWatcher;
    QTimer m_themeReloadTimer;
};
