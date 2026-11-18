#pragma once

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>

// Keep Write's upstream identity for preferences, recovery data and IPC.
// Other applications' profiles are never discovered or imported implicitly.
namespace Profile {
inline QString ipcDirectoryName() { return QStringLiteral("omawrite-instance"); }
inline QString pipePrefix() { return QStringLiteral("omawrite-"); }
inline void configureIdentity() {
    QCoreApplication::setApplicationName(QStringLiteral("omawrite"));
    QCoreApplication::setOrganizationName(QStringLiteral("Omacom"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("omacom.io"));
}
inline bool activate() {
    QSettings settings;
    settings.sync();
    return settings.status() == QSettings::NoError;
}
inline QString dataLocation() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
inline QString runtimeLocation() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
}
}
