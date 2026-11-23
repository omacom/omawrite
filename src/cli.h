#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// Answer terminal-only requests before constructing the GUI. --version is
// retained for the Windows installer; normal upstream calls need no version.
namespace Cli {
QString usage();
std::optional<int> handleArguments(const QStringList &arguments, const QString &version = QString());
std::optional<int> handleArguments(int argc, char *argv[], const QString &version = QString());
}
