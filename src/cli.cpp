#include "cli.h"

#include "backend.h"
#include "systemtheme.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>

QString Cli::usage() {
    return QStringLiteral(
        "Usage: omawrite [OPTIONS] [FILE]\n"
        "\n"
        "Opens FILE, a Markdown file, or an empty document without one. Omawrite\n"
        "is a graphical app and keeps running until its window is closed.\n"
        "\n"
        "Options:\n"
        "  -c, --config PATH  Load configuration from PATH\n"
        "  -g, --generate     Generate default config from system settings and exit\n"
        "  -h, --help         Show this message and exit\n"
        "  -v, --version      Show the version and exit\n");
}

std::optional<int> Cli::parseOptions(const QStringList &arguments, Options *options,
                                     QString *errorMessage) {
    Options parsed;
    const auto fail = [errorMessage](const QString &message) -> std::optional<int> {
        if (errorMessage)
            *errorMessage = message;
        QTextStream(stderr) << message << QLatin1Char('\n');
        return 1;
    };

    bool endOfOptions = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &arg = arguments.at(i);
        if (!endOfOptions && arg == QLatin1String("--")) {
            endOfOptions = true;
            continue;
        }

        if (!endOfOptions && (arg == QLatin1String("-c") || arg == QLatin1String("--config"))) {
            if (i + 1 >= arguments.size())
                return fail(QStringLiteral("omawrite: %1 requires a file path").arg(arg));
            parsed.configPath = arguments.at(++i);
            if (parsed.configPath.isEmpty())
                return fail(QStringLiteral("omawrite: %1 requires a file path").arg(arg));
            const QFileInfo info(parsed.configPath);
            if (!info.exists() || !info.isFile()) {
                return fail(QStringLiteral("omawrite: config file not found: %1")
                                .arg(parsed.configPath));
            }
            parsed.configPath = info.absoluteFilePath();
            continue;
        }

        if (!endOfOptions && arg.startsWith(QLatin1String("--config="))) {
            parsed.configPath = arg.mid(9);
            if (parsed.configPath.isEmpty())
                return fail(QStringLiteral("omawrite: --config requires a file path"));
            const QFileInfo info(parsed.configPath);
            if (!info.exists() || !info.isFile()) {
                return fail(QStringLiteral("omawrite: config file not found: %1")
                                .arg(parsed.configPath));
            }
            parsed.configPath = info.absoluteFilePath();
            continue;
        }

        // Leave Qt's own flags untouched unless `--` ended option parsing.
        // A bare `-notes.md` with no argument after it is treated as a file name.
        if (!endOfOptions && arg.startsWith(QLatin1Char('-')) && i + 1 < arguments.size()
                && (arg == QLatin1String("-platform")
                    || arg == QLatin1String("-platformpluginpath")
                    || arg == QLatin1String("-platformtheme")
                    || arg == QLatin1String("-plugin")
                    || arg == QLatin1String("-qmljsdebugger")
                    || arg == QLatin1String("-qwindowgeometry")
                    || arg == QLatin1String("-qwindowicon")
                    || arg == QLatin1String("-qwindowtitle")
                    || arg == QLatin1String("-session")
                    || arg == QLatin1String("-style")
                    || arg == QLatin1String("-stylesheet"))) {
            ++i;
            continue;
        }
        if (!endOfOptions
                && (arg == QLatin1String("-reverse")
                    || arg == QLatin1String("-nograb")
                    || arg == QLatin1String("-dograb"))) {
            continue;
        }

        if (parsed.filePath.isEmpty())
            parsed.filePath = arg;
    }

    if (options)
        *options = parsed;
    return std::nullopt;
}

std::optional<int> Cli::handleArguments(const QStringList &arguments, const QString &version,
                                        Options *options) {
    if (arguments.size() < 2)
        return std::nullopt;

    for (int i = 1; i < arguments.size(); ++i) {
        const QString &arg = arguments.at(i);
        if (arg == QLatin1String("--"))
            break;
        if (arg == QLatin1String("-h") || arg == QLatin1String("--help")) {
            QTextStream(stdout) << usage();
            return 0;
        }
        if (arg == QLatin1String("-v") || arg == QLatin1String("--version")) {
            QTextStream(stdout) << QStringLiteral("omawrite %1\n").arg(version);
            return 0;
        }
        if (arg == QLatin1String("-g") || arg == QLatin1String("--generate")) {
            if (QCoreApplication::organizationName().isEmpty())
                QCoreApplication::setOrganizationName(QStringLiteral("Omacom"));
            if (QCoreApplication::organizationDomain().isEmpty())
                QCoreApplication::setOrganizationDomain(QStringLiteral("omacom.io"));
            if (QCoreApplication::applicationName().isEmpty())
                QCoreApplication::setApplicationName(QStringLiteral("omawrite"));
            const SystemTheme systemTheme;
            return Backend::generateDefaultConfigFile(systemTheme.darkMode());
        }
    }

    return parseOptions(arguments, options);
}

std::optional<int> Cli::handleArguments(int argc, char *argv[], const QString &version,
                                        Options *options) {
    QStringList arguments;
    arguments.reserve(argc);
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));

    return handleArguments(arguments, version, options);
}
