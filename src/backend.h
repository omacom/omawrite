#pragma once

#include <QFileSystemWatcher>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class DocumentTab;
class QLockFile;
class QWindow;

// Owns everything that is global to a window/process: theme, window geometry,
// clipboard helpers, and the list of open DocumentTabs. Its scalar
// Q_PROPERTYs (fileUrl, fileName, modified, status, wordCount) forward to the
// active tab, re-wired on every tab switch, so QML bindings written against
// "the current document" keep working unchanged regardless of tab count.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY fileUrlChanged)
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
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int activeTabIndex READ activeTabIndex WRITE setActiveTabIndex NOTIFY activeTabIndexChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void setParentWindow(QWindow *window);

    QUrl fileUrl() const;
    QString fileName() const;

    bool modified() const;
    QString status() const;
    int wordCount() const;
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool darkMode);
    qreal textScale() const { return m_textScale; }
    void setTextScale(qreal textScale);
    QString themeBackground() const { return m_themeBackground; }
    QString themeForeground() const { return m_themeForeground; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeSelection() const { return m_themeSelection; }
    QVariantList tabs() const;
    int activeTabIndex() const { return m_activeTabIndex; }
    void setActiveTabIndex(int index);
    static int countWords(const QString &text);
    static QString normalizedLinkUrl(const QString &clipboardText);
    static QString suggestedFileName(const QString &text);

    // Convenience overload acting on the active tab; used by QML and by
    // single-document call sites (tests) that predate tabs.
    Q_INVOKABLE void attachDocument(QObject *textDocument);
    // Attaches a specific tab's QTextDocument; used by the per-tab editor
    // delegate in Main.qml, which knows which DocumentTab it belongs to.
    Q_INVOKABLE void attachTabDocument(QObject *tabHandle, QObject *textDocument);
    Q_INVOKABLE void openDialog();
    Q_INVOKABLE void open(const QUrl &url);
    // Activates the tab already open on this file, or opens it in a new tab.
    Q_INVOKABLE void activateOrOpenTab(const QUrl &url);
    Q_INVOKABLE void newTab();
    Q_INVOKABLE void closeActiveTab();
    Q_INVOKABLE void discardActiveTab();
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
    // Like editorTextChanged(), but for a specific tab regardless of which
    // one is active -- background tabs still need word count/autosave/status
    // updates when their document text changes (e.g. during session restore).
    Q_INVOKABLE bool editorTextChangedForTab(QObject *tabHandle);
    Q_INVOKABLE QVariantList hiddenRangesAt(int position) const;
    Q_INVOKABLE void setSearchHighlight(const QString &query, int currentMatchStart);
    Q_INVOKABLE void updateCursorState(int cursor, int selectionStart, int selectionEnd);
    Q_INVOKABLE QVariantMap consumePendingCursorRestore();
    Q_INVOKABLE void openExternalUrl(const QUrl &url);
    Q_INVOKABLE QVariantMap windowGeometry() const;
    Q_INVOKABLE void saveWindowGeometry(int x, int y, int width, int height, bool maximized);
    // Synchronously flushes any pending autosave/session debounce timers;
    // called right before the window closes so nothing is lost mid-debounce.
    Q_INVOKABLE void flushAllPendingWrites();

signals:
    void fileUrlChanged();
    void modifiedChanged();
    void statusChanged();
    void wordCountChanged();
    void darkModeChanged();
    void textScaleChanged();
    void themeColorsChanged();
    void tabsChanged();
    void activeTabIndexChanged();
    void closeAfterSave();
    void openDialogRequested();
    void saveDialogRequested(const QUrl &suggestedUrl);
    void saveSucceeded();
    void externalChangeDetected(bool deleted, bool locallyModified);
    // The active tab is untitled and has unsaved content: QML should confirm
    // before discarding it (closeActiveTab() emits this instead of closing
    // outright).
    void closeActiveTabRequiresConfirmation();
    // The last remaining tab was closed; QML should close the window.
    void lastTabClosed();

private:
    DocumentTab *activeTab() const;
    DocumentTab *appendTab();
    void removeActiveTab();
    void connectActiveTabSignals();
    void disconnectActiveTabSignals();
    void loadOmarchyTheme();
    void watchOmarchyTheme();
    void claimSessionSlot();
    void loadSessionOrLegacyRecovery();
    void scheduleSessionWrite();
    void writeSessionFile();

    QList<DocumentTab *> m_tabs;
    int m_activeTabIndex = 0;
    QList<QMetaObject::Connection> m_activeTabConnections;
    QPointer<QWindow> m_parentWindow;
    bool m_darkMode = true;
    qreal m_textScale = 1.0;

    QString m_themeBackground;
    QString m_themeForeground;
    QString m_themeAccent;
    QString m_themeSelection;
    QFileSystemWatcher m_themeWatcher;

    QString m_sessionPath;
    std::unique_ptr<QLockFile> m_sessionLock;
    QTimer m_sessionTimer;
};
