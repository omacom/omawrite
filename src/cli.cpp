#include "cli.h"

#include <QTextStream>

QString Cli::usage() {
    return QStringLiteral(
        "Omawrite is a dead-simple Markdown writing app.\n"
        "\n"
        "Usage:\n"
        "  omawrite [FILE]\n"
        "\n"
        "Opens the Omawrite window. FILE is the Markdown file to open; without\n"
        "one, Omawrite starts on an empty document, or on the draft recovered\n"
        "from the last session.\n"
        "\n"
        "Options:\n"
        "  -h, --help    Show this message and exit\n"
        "\n"
        "Omawrite is a graphical app: it needs a desktop session, and it keeps\n"
        "running until the window is closed. Scripts and agents that only want\n"
        "a file on screen should background it:\n"
        "\n"
        "  omawrite draft.md &\n"
        "\n"
        "Everything else is a keyboard shortcut. Ctrl+? lists them in the app,\n"
        "and https://github.com/omacom-io/omawrite#shortcuts has the same list.\n");
}

std::optional<int> Cli::handleArguments(const QStringList &arguments) {
    for (const QString &argument : arguments.mid(1)) {
        if (argument == QLatin1String("-h") || argument == QLatin1String("--help")) {
            QTextStream(stdout) << usage();
            return 0;
        }

        // A leading dash means an option was meant, not a file. Answering with
        // the usage beats opening a window for a file that cannot exist.
        if (argument.startsWith(QLatin1Char('-')) && argument != QLatin1String("-")) {
            QTextStream(stderr) << QStringLiteral("omawrite: unrecognized option '%1'\n\n")
                                       .arg(argument)
                                << usage();
            return 1;
        }
    }

    return std::nullopt;
}

std::optional<int> Cli::handleArguments(int argc, char *argv[]) {
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));

    return handleArguments(arguments);
}
