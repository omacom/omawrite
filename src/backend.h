#pragma once

#include <QFont>
#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <QVector>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <memory>

class MarkdownHighlighter;
class QTextDocument;
class QWindow;
class QLockFile;
class QPrinter;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileUrlChanged)
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int wordCount READ wordCount NOTIFY wordCountChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale WRITE setTextScale NOTIFY textScaleChanged)
    Q_PROPERTY(int editorFontSize READ editorFontSize WRITE setEditorFontSize
               NOTIFY editorFontSizeChanged)
    Q_PROPERTY(QString editorFontFamily READ editorFontFamily WRITE setEditorFontFamily
               NOTIFY editorFontFamilyChanged)
    Q_PROPERTY(QString caretStyle READ caretStyle WRITE setCaretStyle NOTIFY caretStyleChanged)
    Q_PROPERTY(bool caretBlink READ caretBlink WRITE setCaretBlink NOTIFY caretBlinkChanged)
    Q_PROPERTY(int editorMeasureChars READ editorMeasureChars WRITE setEditorMeasureChars
               NOTIFY editorMeasureCharsChanged)
    Q_PROPERTY(qreal printMarginMm READ printMarginMm WRITE setPrintMarginMm
               NOTIFY printMarginMmChanged)
    Q_PROPERTY(qreal printFontPointSize READ printFontPointSize
               WRITE setPrintFontPointSize NOTIFY printFontPointSizeChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)
    Q_PROPERTY(QString appCommit READ appCommit CONSTANT)
    Q_PROPERTY(QUrl appCommitUrl READ appCommitUrl CONSTANT)
    Q_PROPERTY(int wordTarget READ wordTarget WRITE setWordTarget NOTIFY wordTargetChanged)
    Q_PROPERTY(bool paragraphOnReturn READ paragraphOnReturn WRITE setParagraphOnReturn
               NOTIFY paragraphOnReturnChanged)
    Q_PROPERTY(bool autosave READ autosave WRITE setAutosave NOTIFY autosaveChanged)
    Q_PROPERTY(int autosaveDelayMs READ autosaveDelayMs WRITE setAutosaveDelayMs
               NOTIFY autosaveDelayMsChanged)
    Q_PROPERTY(QString themeBackground READ themeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeForeground READ themeForeground NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeSelection READ themeSelection NOTIFY themeColorsChanged)
    Q_PROPERTY(QString themeCodeBackground READ themeCodeBackground NOTIFY themeColorsChanged)
    Q_PROPERTY(int tabCount READ tabCount NOTIFY tabsChanged)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex NOTIFY tabsChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(QString tabTitle READ tabTitle NOTIFY tabsChanged)
    Q_PROPERTY(bool nativeMacChrome READ nativeMacChrome NOTIFY nativeMacChromeChanged)
    Q_PROPERTY(bool canAssembleThisFolder READ canAssembleThisFolder NOTIFY fileUrlChanged)

public:
    // Converts a pixel-sized editor font into the resolution-independent
    // point size a QPrinter needs. Exposed for testing.
    static QFont printFont(const QFont &editorFont, qreal screenDpi);

    // The name a print job carries. macOS offers it as the default filename in
    // Save as PDF, so it has to be the document's name without the Markdown
    // extension rather than the app's idea of an untitled job.
    static QString printJobName(const QString &documentFileName);

    // Text operations the writing process keeps asking for by hand. Each takes
    // the document and a cursor offset and returns {"text", "cursor"}, so they
    // are pure and testable; the editor applies the result.
    Q_INVOKABLE static QVariantMap moveParagraph(const QString &text, int cursor,
                                                 int delta);
    Q_INVOKABLE static QVariantMap explodeSentences(const QString &text, int cursor);
    Q_INVOKABLE static QVariantMap collapseSentences(const QString &text, int cursor);

    // One entry per ATX heading: {"level", "title", "position"}.
    Q_INVOKABLE static QVariantList outlineFor(const QString &text);

    // Splits a paragraph into sentences. Exposed because the boundaries are a
    // heuristic worth testing on its own.
    static QStringList splitSentences(const QString &paragraph);

    // Starts a second Omawrite, optionally on a file. Exposed so main.cpp can
    // reuse it for Finder's open-document events.
    static bool launchNewInstance(const QString &filePath = QString());

    // The .app enclosing an executable directory, or an empty string when the
    // executable is not inside a bundle. Takes the directory rather than
    // reading it from the application so it can be tested off a real bundle.
    static QString enclosingBundlePath(const QString &executableDirPath);

    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);
    QWindow *parentWindow() const;
    static QList<Backend *> liveWindows();
    static int storedWindowCount();
    bool nativeMacChrome() const { return m_nativeMacChrome; }
    void setNativeMacChrome(bool nativeMacChrome);
    // Persist every live window, then ignore destructor writes so tearing
    // windows down one at a time cannot shrink the session to the last one.
    static void prepareToQuit();
    static void allowSessionWrites();

    QUrl fileUrl() const { return m_fileUrl; }
    QString fileName() const;

    bool modified() const { return m_modified; }
    QString status() const { return m_status; }
    int wordCount() const { return m_wordCount; }
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal textScale);
    int editorFontSize() const { return m_editorFontSize; }
    void setEditorFontSize(int editorFontSize);
    QString editorFontFamily() const { return m_editorFontFamily; }
    void setEditorFontFamily(const QString &family);
    QString caretStyle() const { return m_caretStyle; }
    void setCaretStyle(const QString &caretStyle);
    bool caretBlink() const { return m_caretBlink; }
    void setCaretBlink(bool caretBlink);
    int editorMeasureChars() const { return m_editorMeasureChars; }
    void setEditorMeasureChars(int measureChars);
    qreal printMarginMm() const { return m_printMarginMm; }
    void setPrintMarginMm(qreal marginMm);
    // A page is not a screen: the editor's size in pixels means nothing on
    // paper, so the printed size is its own setting in points.
    qreal printFontPointSize() const { return m_printFontPointSize; }
    void setPrintFontPointSize(qreal pointSize);

    // The family actually used: the request when the system has it, and the
    // bundled face when it does not, so a font named in settings on one
    // machine cannot leave another with a fallback nobody chose.
    // Families offered in Preferences: the faces shipped with the app first,
    // then whatever else is installed.
    Q_INVOKABLE static QStringList availableFontFamilies();

    // A line holding nothing but a file path embeds that file when the
    // document is rendered: iA Writer's content blocks. Returns the markdown
    // with every such line replaced by the file's content, and the source
    // untouched -- the document on disk stays the path it was written as.
    //
    // documentDirectory is the folder the document itself lives in. Paths
    // resolve inside it and nowhere else, so a document cannot read its way up
    // and out of the folder it was opened from.
    static QString expandContentBlocks(const QString &markdown,
                                       const QString &documentDirectory,
                                       const QVariantMap &liveTexts = {});

    // Whether a line is a content block, and what it names. Exposed because
    // the shape of the line is the part worth testing directly.
    struct ContentBlock {
        QString path;
        QString caption;
    };
    static bool contentBlockOnLine(const QString &line, ContentBlock *block);

    // The three iA writing faces the app carries, most proportional first --
    // Quattro is what iA Writer itself writes in, so it leads.
    static QStringList bundledFontFamilies();

    static QString resolveFontFamily(const QString &requested,
                                     const QStringList &availableFamilies);

    // 0 means no target. The first draft is deliberately written long, so the
    // draft goal is the target plus a quarter.
    // Baked in at build time, so About names the commit this binary is.
    static QString appVersion();
    static QString appCommit();

    // Where to read that commit. Empty when there is nothing worth linking:
    // no commit recorded, or a tree with uncommitted changes, where the page
    // would show code the binary was not built from.
    static QUrl appCommitUrl();

    int wordTarget() const { return m_wordTarget; }
    void setWordTarget(int wordTarget);
    Q_INVOKABLE static int draftTargetFor(int wordTarget);

    // Whether Return at the end of a line opens a new Markdown paragraph --
    // two newlines, so a blank line stands between them -- or just breaks the
    // line once, which is what most writers expect a Return to do.
    bool paragraphOnReturn() const { return m_paragraphOnReturn; }
    void setParagraphOnReturn(bool paragraphOnReturn);

    bool autosave() const { return m_autosave; }
    void setAutosave(bool autosave);
    int autosaveDelayMs() const { return m_autosaveDelayMs; }
    void setAutosaveDelayMs(int autosaveDelayMs);

    // Whether the debounce may write the document's own file, rather than the
    // recovery snapshot. Exposed so the guardrails can be tested directly.
    bool canAutosaveToFile() const;
    bool pathNeverRead() const { return m_pathNeverRead; }

    // Where the session file lives. Public so tests can read hot-exit state.
    // recoveryPath is the same path: crash and quit share session.json.
    static QString sessionPath();
    QString recoveryPath() const;

    int tabCount() const;
    int activeTabIndex() const { return m_activeTab; }
    QVariantList tabs() const;
    QString tabTitle() const;
    Q_INVOKABLE void newTab();
    Q_INVOKABLE bool closeTab(int index);
    Q_INVOKABLE void setActiveTab(int index);
    Q_INVOKABLE void persistSession();
    Q_INVOKABLE void adoptTabFrom(QObject *sourceWindow, int index);
    Q_INVOKABLE void takeDetachedTab(QObject *sourceWindow, int index);
    Q_INVOKABLE void moveTab(int from, int to);
    Q_INVOKABLE void finishTabDrag(int index, qreal globalX, qreal globalY);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QString themeCodeBackground() const { return m_themeCodeBackground; }
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);
    static QString tomlValue(const QString &text);

    Q_INVOKABLE void attachDocument(QObject *textDocument);
    Q_INVOKABLE void renderPreview(QObject *textDocument);
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
    Q_INVOKABLE void resetEditorFontSize();
    Q_INVOKABLE void printDocument();
    // Collate the named Document's folder into one printable Markdown file:
    // content-block includes are absorbed into their parents; leftover .md
    // siblings concatenate in filename order. Untitled drafts have no folder.
    bool canAssembleThisFolder() const;
    static QVariantMap assembleFolder(const QString &directory,
                                       const QVariantMap &liveTexts = {});
    Q_INVOKABLE QVariantMap assembleThisFolder();
    Q_INVOKABLE bool saveAssembledMarkdown(const QUrl &url, const QString &markdown);
    Q_INVOKABLE bool saveAssembledPdf(const QUrl &url, const QString &markdown);
    Q_INVOKABLE void printAssembledMarkdown(const QString &markdown);
    Q_INVOKABLE void newWindow();
    Q_INVOKABLE void bringAllWindowsToFront();
    Q_INVOKABLE QString clipboardUrl() const;
    Q_INVOKABLE QString clipboardText() const;
    Q_INVOKABLE bool editorTextChanged();
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void openExternalUrl(const QUrl &url);
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void editorFontSizeChanged();
    void editorFontFamilyChanged();
    void caretStyleChanged();
    void caretBlinkChanged();
    void editorMeasureCharsChanged();
    void printMarginMmChanged();
    void printFontPointSizeChanged();
    void wordTargetChanged();
    void paragraphOnReturnChanged();
    void autosaveChanged();
    void autosaveDelayMsChanged();
    void themeColorsChanged();
    void tabsChanged();
    void nativeMacChromeChanged();
    void closeWindowRequested();
    void newWindowRequested();
    void detachTabRequested(int index);
    void tabCloseNeedsConfirm(int index);
    void closeAfterSave();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void saveFailed();
    void externalChangeDetected(bool deleted, bool locallyModified);
    void externalFileAppeared(bool locallyModified);

private:
    void openPath(const QUrl &url, bool mayStartNewFile);
    void loadDocumentText(const QString &text);
    void setFileUrl(const QUrl &url);
    void setModified(bool modified);
    void setStatus(const QString &status);
    void saveTo(const QUrl &url);
    QUrl suggestedSaveUrl() const;
    QString currentDocumentText() const;
    // The document as it is rendered: content blocks resolved against the
    // folder the document was opened from.
    QString renderableDocumentText() const;
    void setWordCount(int words);
    void refreshWordCount();
    void scheduleWordCount();
    void applyDocumentTypography();
    // Fenced code sits on a panel. A character background only paints behind
    // the glyphs, so it ends ragged at each line and leaves gaps through the
    // leading; a block background fills the line.
    // Where the fences are, as character positions, so the editor can draw a
    // panel behind them. Qt Quick's TextEdit paints character backgrounds but
    // ignores block ones, so the panel cannot come from the document itself.
public:
    Q_INVOKABLE QVariantList fencedRanges() const;
private:
    // The block formatting a rendered document needs: the editor's leading,
    // paragraph spacing the markdown reader drops, and the panel behind code.
    // Preview and print share it, or the page comes out set differently from
    // what was on screen. forPrint takes the light palette: paper is white
    // whatever the app's theme is.
    void styleRenderedDocument(QTextDocument *document, bool forPrint) const;

public:
    // Exposed so a test can render exactly what the printer would.
    void styleRenderedDocumentForTest(QTextDocument *document, bool forPrint) const {
        styleRenderedDocument(document, forPrint);
    }

private:
    void reapplyTypographyToChange();
    void scheduleRecovery();
    void persistDocument();
    void writeRecovery();
    void restoreRecovery();
    void clearRecovery();
    void ensureTab();
    bool isBlankUntitled() const;
    int indexOfLocalPath(const QString &path) const;
    int nextUntitledNumber() const;
    QJsonObject sessionObject() const;
    static void writeSessionFile(const QJsonArray &windows);
    void loadTabFields(int index);
    void storeTabFields(int index);
    void setKnownFileContents(const QByteArray &contents, bool known);
    void watchCurrentFile();
    void syncRepresentedFile();
    void loadOmarchyTheme();
    void watchOmarchyTheme();
    void printRenderedMarkdown(QPrinter *printer, const QString &markdown) const;
    QVariantMap liveDocumentTexts() const;

    QUrl m_fileUrl;
    bool m_modified = false;
    QString m_status;
    int m_wordCount = 0;
    bool m_darkMode = true;
    qreal m_textScale = 1.0;
    int m_editorFontSize = 20;
    QString m_editorFontFamily;
    QString m_caretStyle;
    bool m_caretBlink = true;
    int m_editorMeasureChars = 65;
    qreal m_printMarginMm = 15.0;
    qreal m_printFontPointSize = 11.0;
    int m_wordTarget = 0;
    bool m_paragraphOnReturn = false;
    bool m_autosave = true;
    int m_autosaveDelayMs = 750;
    // An outside edit the writer has not answered yet. Autosave must not pick
    // a version for them while that dialog is standing.
    bool m_externalChangeUnanswered = false;
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
    bool m_nativeMacChrome = false;
    QPointer<MarkdownHighlighter> m_highlighter;
    QString m_lastDocumentText;
    QByteArray m_lastKnownFileContents;
    QString m_lastKnownFileText;
    bool m_hasKnownFileContents = false;
    // Set where this document takes a name without having read what is on it,
    // and cleared the moment anything settles the question -- a read, a write,
    // or the writer answering the dialog. It is not the same question as
    // m_hasKnownFileContents, which asks whether we hold a copy to compare
    // against; a path we have never looked at is one nothing can watch.
    bool m_pathNeverRead = false;
    QString m_recoveryPath;
    std::unique_ptr<QLockFile> m_recoveryLock;

    struct DocumentTab {
        QUrl fileUrl;
        QString cachedText;
        bool modified = false;
        bool pathNeverRead = false;
        QByteArray lastKnownFileContents;
        QString lastKnownFileText;
        bool hasKnownFileContents = false;
        bool externalChangeUnanswered = false;
        int untitledNumber = 0;
    };
    QVector<DocumentTab> m_tabs;
    int m_activeTab = 0;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QString m_themeLighterBackground;
    QString m_themeCodeBackground;
    QFileSystemWatcher m_themeWatcher;
};
