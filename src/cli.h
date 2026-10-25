#pragma once

#include <QString>
#include <QStringList>
#include <optional>

// Command line handling that runs before the GUI starts, so `omawrite --help`
// and `omawrite --version` answer in the terminal instead of opening a window.
namespace Cli {

struct Options {
    QString configPath;
    QString filePath;
};

QString usage();

// Extracts `-c` / `--config` and the document path from the command line, or
// writes an error and returns an exit code when the config flag is malformed
// or points to a missing file.
std::optional<int> parseOptions(const QStringList &arguments, Options *options,
                                QString *errorMessage = nullptr);

// The exit code main should return when the arguments are answered without
// starting the app, or nothing when Omawrite should open as usual. Anything
// else, Qt's own options included, is left for the app.
std::optional<int> handleArguments(const QStringList &arguments, const QString &version,
                                   Options *options = nullptr);
std::optional<int> handleArguments(int argc, char *argv[], const QString &version,
                                   Options *options = nullptr);

}
