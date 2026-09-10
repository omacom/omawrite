#include "workspacesession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace {
constexpr int sessionVersion = 1;

QJsonObject jsonTab(const QVariantMap &tab) {
    return {{QStringLiteral("id"), tab.value(QStringLiteral("id")).toString()},
            {QStringLiteral("fileUrl"), tab.value(QStringLiteral("fileUrl")).toString()},
            {QStringLiteral("text"), tab.value(QStringLiteral("text")).toString()},
            {QStringLiteral("cursorPosition"), tab.value(QStringLiteral("cursorPosition")).toInt()},
            {QStringLiteral("selectionStart"), tab.value(QStringLiteral("selectionStart")).toInt()},
            {QStringLiteral("selectionEnd"), tab.value(QStringLiteral("selectionEnd")).toInt()},
            {QStringLiteral("modified"), tab.value(QStringLiteral("modified")).toBool()}};
}

bool tabFromJson(const QJsonValue &value, QVariantMap *tab) {
    if (!value.isObject())
        return false;

    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    const QJsonValue fileUrl = object.value(QStringLiteral("fileUrl"));
    const QJsonValue text = object.value(QStringLiteral("text"));
    const QJsonValue cursorPosition = object.value(QStringLiteral("cursorPosition"));
    const QJsonValue selectionStart = object.value(QStringLiteral("selectionStart"));
    const QJsonValue selectionEnd = object.value(QStringLiteral("selectionEnd"));
    const QJsonValue modified = object.value(QStringLiteral("modified"));
    if (!id.isString() || id.toString().isEmpty() || !fileUrl.isString() || !text.isString()
            || !cursorPosition.isDouble() || !selectionStart.isDouble() || !selectionEnd.isDouble()
            || !modified.isBool())
        return false;

    const int textLength = text.toString().size();
    *tab = {{QStringLiteral("id"), id.toString()},
            {QStringLiteral("fileUrl"), fileUrl.toString()},
            {QStringLiteral("text"), text.toString()},
            {QStringLiteral("cursorPosition"), qBound(0, cursorPosition.toInt(), textLength)},
            {QStringLiteral("selectionStart"), qBound(0, selectionStart.toInt(), textLength)},
            {QStringLiteral("selectionEnd"), qBound(0, selectionEnd.toInt(), textLength)},
            {QStringLiteral("modified"), modified.toBool()}};
    return true;
}

QJsonObject jsonWindow(const QVariantMap &window) {
    QJsonArray tabs;
    for (const QVariant &value : window.value(QStringLiteral("tabs")).toList())
        tabs.append(jsonTab(value.toMap()));
    return {{QStringLiteral("id"), window.value(QStringLiteral("id")).toString()},
            {QStringLiteral("x"), window.value(QStringLiteral("x")).toInt()},
            {QStringLiteral("y"), window.value(QStringLiteral("y")).toInt()},
            {QStringLiteral("width"), window.value(QStringLiteral("width")).toInt()},
            {QStringLiteral("height"), window.value(QStringLiteral("height")).toInt()},
            {QStringLiteral("maximized"), window.value(QStringLiteral("maximized")).toBool()},
            {QStringLiteral("activeTabId"), window.value(QStringLiteral("activeTabId")).toString()},
            {QStringLiteral("tabs"), tabs}};
}

bool windowFromJson(const QJsonValue &value, QVariantMap *window) {
    if (!value.isObject())
        return false;

    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    const QJsonValue activeTabId = object.value(QStringLiteral("activeTabId"));
    const QJsonValue tabs = object.value(QStringLiteral("tabs"));
    if (!id.isString() || id.toString().isEmpty() || !object.value(QStringLiteral("x")).isDouble()
            || !object.value(QStringLiteral("y")).isDouble()
            || !object.value(QStringLiteral("width")).isDouble()
            || !object.value(QStringLiteral("height")).isDouble()
            || !object.value(QStringLiteral("maximized")).isBool() || !activeTabId.isString()
            || !tabs.isArray())
        return false;

    QVariantList restoredTabs;
    QSet<QString> tabIds;
    for (const QJsonValue &tabValue : tabs.toArray()) {
        QVariantMap tab;
        if (!tabFromJson(tabValue, &tab) || tabIds.contains(tab.value(QStringLiteral("id")).toString()))
            return false;
        tabIds.insert(tab.value(QStringLiteral("id")).toString());
        restoredTabs.append(tab);
    }
    if (restoredTabs.isEmpty() || !tabIds.contains(activeTabId.toString()))
        return false;

    *window = {{QStringLiteral("id"), id.toString()},
               {QStringLiteral("x"), object.value(QStringLiteral("x")).toInt()},
               {QStringLiteral("y"), object.value(QStringLiteral("y")).toInt()},
               {QStringLiteral("width"), object.value(QStringLiteral("width")).toInt()},
               {QStringLiteral("height"), object.value(QStringLiteral("height")).toInt()},
               {QStringLiteral("maximized"), object.value(QStringLiteral("maximized")).toBool()},
               {QStringLiteral("activeTabId"), activeTabId.toString()},
               {QStringLiteral("tabs"), restoredTabs}};
    return true;
}
}

WorkspaceSession::WorkspaceSession(const QString &stateDirectory) : m_stateDirectory(stateDirectory) {}

QVariantList WorkspaceSession::windows() const {
    return m_windows;
}

QVariantList WorkspaceSession::tabs(const QString &windowId) const {
    for (const QVariant &value : m_windows) {
        const QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() == windowId)
            return window.value(QStringLiteral("tabs")).toList();
    }
    return {};
}

QString WorkspaceSession::activeTabId(const QString &windowId) const {
    for (const QVariant &value : m_windows) {
        const QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() == windowId)
            return window.value(QStringLiteral("activeTabId")).toString();
    }
    return {};
}

QString WorkspaceSession::createWindow(int x, int y, int width, int height, bool maximized) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_windows.append(QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("x"), x},
                                 {QStringLiteral("y"), y},
                                 {QStringLiteral("width"), width},
                                 {QStringLiteral("height"), height},
                                 {QStringLiteral("maximized"), maximized},
                                 {QStringLiteral("activeTabId"), QString()},
                                 {QStringLiteral("tabs"), QVariantList()}});
    return id;
}

QString WorkspaceSession::createTab(const QString &windowId, const QUrl &fileUrl,
                                    const QString &text, int cursorPosition,
                                    int selectionStart, int selectionEnd, bool modified) {
    if (!findOpenLocalFile(fileUrl).isEmpty())
        return {};

    for (QVariant &value : m_windows) {
        QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() != windowId)
            continue;

        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVariantList tabs = window.value(QStringLiteral("tabs")).toList();
        tabs.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("fileUrl"), fileUrl.toString()},
                                {QStringLiteral("text"), text},
                                {QStringLiteral("cursorPosition"), qBound(0, cursorPosition, text.size())},
                                {QStringLiteral("selectionStart"), qBound(0, selectionStart, text.size())},
                                {QStringLiteral("selectionEnd"), qBound(0, selectionEnd, text.size())},
                                {QStringLiteral("modified"), modified}});
        window.insert(QStringLiteral("tabs"), tabs);
        window.insert(QStringLiteral("activeTabId"), id);
        value = window;
        return id;
    }
    return {};
}

bool WorkspaceSession::updateTab(const QString &windowId, const QString &tabId,
                                 const QUrl &fileUrl, const QString &text,
                                 int cursorPosition, int selectionStart,
                                 int selectionEnd, bool modified) {
    for (QVariant &windowValue : m_windows) {
        QVariantMap window = windowValue.toMap();
        if (window.value(QStringLiteral("id")).toString() != windowId)
            continue;
        QVariantList tabs = window.value(QStringLiteral("tabs")).toList();
        for (QVariant &tabValue : tabs) {
            QVariantMap tab = tabValue.toMap();
            if (tab.value(QStringLiteral("id")).toString() != tabId)
                continue;
            tab.insert(QStringLiteral("fileUrl"), fileUrl.toString());
            tab.insert(QStringLiteral("text"), text);
            tab.insert(QStringLiteral("cursorPosition"), qBound(0, cursorPosition, text.size()));
            tab.insert(QStringLiteral("selectionStart"), qBound(0, selectionStart, text.size()));
            tab.insert(QStringLiteral("selectionEnd"), qBound(0, selectionEnd, text.size()));
            tab.insert(QStringLiteral("modified"), modified);
            tabValue = tab;
            window.insert(QStringLiteral("tabs"), tabs);
            windowValue = window;
            return true;
        }
        return false;
    }
    return false;
}

QString WorkspaceSession::findOpenLocalFile(const QUrl &fileUrl) const {
    if (!fileUrl.isLocalFile())
        return {};

    const QString targetPath = QFileInfo(fileUrl.toLocalFile()).absoluteFilePath();
    for (const QVariant &windowValue : m_windows) {
        for (const QVariant &tabValue : windowValue.toMap().value(QStringLiteral("tabs")).toList()) {
            const QUrl openUrl(tabValue.toMap().value(QStringLiteral("fileUrl")).toString());
            if (openUrl.isLocalFile()
                    && QFileInfo(openUrl.toLocalFile()).absoluteFilePath() == targetPath)
                return tabValue.toMap().value(QStringLiteral("id")).toString();
        }
    }
    return {};
}

bool WorkspaceSession::setActiveTab(const QString &windowId, const QString &tabId) {
    for (QVariant &value : m_windows) {
        QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() != windowId)
            continue;

        for (const QVariant &tabValue : window.value(QStringLiteral("tabs")).toList()) {
            if (tabValue.toMap().value(QStringLiteral("id")).toString() != tabId)
                continue;
            window.insert(QStringLiteral("activeTabId"), tabId);
            value = window;
            return true;
        }
        return false;
    }
    return false;
}

bool WorkspaceSession::moveActiveTab(const QString &windowId, int direction) {
    if (direction != -1 && direction != 1)
        return false;

    for (QVariant &value : m_windows) {
        QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() != windowId)
            continue;

        QVariantList tabs = window.value(QStringLiteral("tabs")).toList();
        const QString activeTabId = window.value(QStringLiteral("activeTabId")).toString();
        for (int index = 0; index < tabs.size(); ++index) {
            if (tabs.at(index).toMap().value(QStringLiteral("id")).toString() != activeTabId)
                continue;

            const int destination = index + direction;
            if (destination < 0 || destination >= tabs.size())
                return false;
            tabs.swapItemsAt(index, destination);
            window.insert(QStringLiteral("tabs"), tabs);
            value = window;
            return true;
        }
        return false;
    }
    return false;
}

bool WorkspaceSession::removeTab(const QString &windowId, const QString &tabId) {
    for (QVariant &value : m_windows) {
        QVariantMap window = value.toMap();
        if (window.value(QStringLiteral("id")).toString() != windowId)
            continue;

        QVariantList tabs = window.value(QStringLiteral("tabs")).toList();
        for (int index = 0; index < tabs.size(); ++index) {
            if (tabs.at(index).toMap().value(QStringLiteral("id")).toString() != tabId)
                continue;

            tabs.removeAt(index);
            window.insert(QStringLiteral("tabs"), tabs);
            if (window.value(QStringLiteral("activeTabId")).toString() == tabId) {
                window.insert(QStringLiteral("activeTabId"), tabs.isEmpty()
                              ? QString()
                              : tabs.at(qMin(index, tabs.size() - 1)).toMap()
                                    .value(QStringLiteral("id")).toString());
            }
            value = window;
            return true;
        }
        return false;
    }
    return false;
}

bool WorkspaceSession::restore() {
    QFile file(sessionPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != sessionVersion
            || !root.value(QStringLiteral("windows")).isArray())
        return false;

    QVariantList restoredWindows;
    QSet<QString> windowIds;
    QSet<QString> openLocalPaths;
    for (const QJsonValue &value : root.value(QStringLiteral("windows")).toArray()) {
        QVariantMap window;
        if (!windowFromJson(value, &window)
                || windowIds.contains(window.value(QStringLiteral("id")).toString()))
            return false;
        for (const QVariant &tabValue : window.value(QStringLiteral("tabs")).toList()) {
            const QUrl fileUrl(tabValue.toMap().value(QStringLiteral("fileUrl")).toString());
            if (!fileUrl.isLocalFile())
                continue;
            const QString path = QFileInfo(fileUrl.toLocalFile()).absoluteFilePath();
            if (openLocalPaths.contains(path))
                return false;
            openLocalPaths.insert(path);
        }
        windowIds.insert(window.value(QStringLiteral("id")).toString());
        restoredWindows.append(window);
    }
    if (restoredWindows.isEmpty())
        return false;

    m_windows = restoredWindows;
    return true;
}

bool WorkspaceSession::saveNow() const {
    if (m_windows.isEmpty())
        return false;

    QDir().mkpath(m_stateDirectory);
    QSaveFile file(sessionPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QJsonArray windows;
    for (const QVariant &value : m_windows)
        windows.append(jsonWindow(value.toMap()));
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), sessionVersion},
                                         {QStringLiteral("windows"), windows}})
                   .toJson(QJsonDocument::Compact));
    return file.commit();
}

QString WorkspaceSession::sessionPath() const {
    return QDir(m_stateDirectory).filePath(QStringLiteral("session.json"));
}
