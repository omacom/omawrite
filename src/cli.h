#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// Command line handling that runs before the GUI starts, so `omawrite --help`
// answers in the terminal instead of opening a window.
namespace Cli {

QString usage();

// The exit code main should return when the arguments are answered without
// starting the app, or nothing when Omawrite should open as usual.
std::optional<int> handleArguments(const QStringList &arguments);
std::optional<int> handleArguments(int argc, char *argv[]);

}
