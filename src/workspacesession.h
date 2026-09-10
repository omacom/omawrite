#pragma once

#include <QString>
#include <QUrl>
#include <QVariantList>

class WorkspaceSession {
public:
    explicit WorkspaceSession(const QString &stateDirectory);

    QVariantList windows() const;
    QVariantMap window(const QString &windowId) const;
    QVariantMap tab(const QString &tabId) const;
    QVariantList tabs(const QString &windowId) const;
    QString activeTabId(const QString &windowId) const;
    QString createWindow(int x, int y, int width, int height, bool maximized);
    QString createTab(const QString &windowId, const QUrl &fileUrl, const QString &text,
                      int cursorPosition, int selectionStart, int selectionEnd, bool modified);
    bool updateTab(const QString &windowId, const QString &tabId, const QUrl &fileUrl,
                   const QString &text, int cursorPosition, int selectionStart,
                   int selectionEnd, bool modified);
    QString findOpenLocalFile(const QUrl &fileUrl) const;
    QString windowIdForTab(const QString &tabId) const;
    bool setActiveTab(const QString &windowId, const QString &tabId);
    bool moveActiveTab(const QString &windowId, int direction);
    bool removeTab(const QString &windowId, const QString &tabId);
    bool removeWindow(const QString &windowId);
    bool setExternalChange(const QString &tabId, bool changed);
    bool updateWindowGeometry(const QString &windowId, int x, int y, int width, int height,
                              bool maximized);
    bool restore();
    bool saveNow() const;

private:
    QString sessionPath() const;

    QString m_stateDirectory;
    QVariantList m_windows;
};
