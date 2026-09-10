#pragma once

#include <QString>
#include <QUrl>
#include <QVariantList>

class WorkspaceSession {
public:
    explicit WorkspaceSession(const QString &stateDirectory);

    QVariantList windows() const;
    QString createWindow(int x, int y, int width, int height, bool maximized);
    QString createTab(const QString &windowId, const QUrl &fileUrl, const QString &text,
                      int cursorPosition, int selectionStart, int selectionEnd, bool modified);
    QString findOpenLocalFile(const QUrl &fileUrl) const;
    bool setActiveTab(const QString &windowId, const QString &tabId);
    bool moveActiveTab(const QString &windowId, int direction);
    bool restore();
    bool saveNow() const;

private:
    QString sessionPath() const;

    QString m_stateDirectory;
    QVariantList m_windows;
};
