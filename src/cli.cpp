#include "cli.h"

#include <QTextStream>

QString Cli::usage() {
    return QStringLiteral(
        "Usage: omawrite [FILE]\n"
        "\n"
        "Opens FILE, a Markdown file, or an empty document without one. Omawrite\n"
        "is a graphical app and keeps running until its window is closed.\n"
        "\n"
        "Options:\n"
        "  -h, --help  Show this message and exit\n");
}

std::optional<int> Cli::handleArguments(const QStringList &arguments) {
    if (arguments.size() < 2)
        return std::nullopt;

    const QString &first = arguments.at(1);
    if (first == QLatin1String("-h") || first == QLatin1String("--help")) {
        QTextStream(stdout) << usage();
        return 0;
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
