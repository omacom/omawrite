#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>

namespace {
const QString PortalService = QStringLiteral("org.freedesktop.portal.Desktop");

struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

// Messages built directly so a hung portal never blocks the UI thread on
// QDBusInterface's synchronous Introspect call (including from ~PortalFilePicker).
QDBusMessage portalCall(const QString &path, const QString &interface, const QString &method) {
    return QDBusMessage::createMethodCall(PortalService, path, interface, method);
}

QString portalToken() {
    return QStringLiteral("omawrite_%1").arg(QRandomGenerator::global()->generate());
}

// The portal takes folders as NUL-terminated byte strings.
QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

PortalFileFilter markdownFilter() {
    return {PortalFilePicker::tr("Markdown files"),
            {{0, QStringLiteral("*.md")}, {0, QStringLiteral("*.markdown")}}};
}

PortalFileFilters documentFilters() {
    return {
        markdownFilter(),
        {PortalFilePicker::tr("All files"), {{0, QStringLiteral("*")}}},
    };
}
} // namespace

PortalFilePicker::PortalFilePicker(QObject *parent, const QDBusConnection &bus)
    : QObject(parent), m_bus(bus) {
    registerPortalFilterTypes();
}

PortalFilePicker::~PortalFilePicker() {
    closePendingRequest();
    clearPending();
}

void PortalFilePicker::openFile(const QString &folder) {
    const QString initialFolder =
        !folder.isEmpty() && QDir(folder).exists() ? folder : QDir::homePath();
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Open"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(initialFolder));
    options.insert(QStringLiteral("filters"), QVariant::fromValue(documentFilters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(markdownFilter()));
    requestFile(QStringLiteral("OpenFile"), tr("Open File"), options, Action::Open);
}

void PortalFilePicker::saveFile(const QUrl &suggestedUrl) {
    const QString fallbackName = tr("Untitled") + QStringLiteral(".md");
    const QFileInfo target(suggestedUrl.isLocalFile()
                               ? suggestedUrl.toLocalFile()
                               : QDir::home().filePath(fallbackName));
    const QString folder = target.absolutePath();
    const QString initialFolder =
        !folder.isEmpty() && QDir(folder).exists() ? folder : QDir::homePath();
    const QString fileName = target.fileName().isEmpty() ? fallbackName : target.fileName();

    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Save"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(initialFolder));
    options.insert(QStringLiteral("current_name"), fileName);
    options.insert(QStringLiteral("filters"), QVariant::fromValue(documentFilters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(markdownFilter()));
    requestFile(QStringLiteral("SaveFile"), tr("Save File"), options, Action::Save);
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return m_bus.connect(
        PortalService, m_pendingPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(handleResponse(uint,QVariantMap)));
}

void PortalFilePicker::closePendingRequest() {
    if (m_pendingPath.isEmpty())
        return;
    m_bus.send(portalCall(m_pendingPath, QStringLiteral("org.freedesktop.portal.Request"),
                          QStringLiteral("Close")));
}

void PortalFilePicker::requestFile(const QString &method, const QString &title,
                                   QVariantMap options, Action action) {
    // A request whose Response never arrives (the portal crashed or was
    // killed with the dialog up) must not wedge every later dialog behind
    // it: close the stuck one best-effort and replace it.
    if (m_pendingAction != Action::None) {
        closePendingRequest();
        clearPending();
    }

    QDBusConnection bus = m_bus;
    if (!bus.isConnected()) {
        emit failed(tr("The desktop's file chooser (xdg-desktop-portal) is not available."));
        return;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pendingAction = action;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(tr("Could not listen for the file chooser's answer."));
        return;
    }

    const int generation = ++m_pendingGeneration;
    QDBusMessage call = portalCall(QStringLiteral("/org/freedesktop/portal/desktop"),
                                   QStringLiteral("org.freedesktop.portal.FileChooser"), method);
    call << QString() << title << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    m_pendingWatcher = watcher;

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher)
            m_pendingWatcher = nullptr;

        // Superseded by a newer request, or a response may already have
        // been handled while the reply was in flight.
        if (generation != m_pendingGeneration || m_pendingAction == Action::None)
            return;

        if (reply.isError()) {
            clearPending();
            const QDBusError::ErrorType type = reply.error().type();
            if (type == QDBusError::ServiceUnknown || type == QDBusError::UnknownObject
                || type == QDBusError::UnknownInterface)
                emit failed(tr("The desktop's file chooser (xdg-desktop-portal) is not available."));
            else
                emit failed(tr("The file chooser failed: %1").arg(reply.error().message()));
            return;
        }

        // Old portal versions can hand back a different request path than the
        // handle_token predicts; move our subscription over if so.
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            m_bus.disconnect(
                PortalService, m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleResponse(uint,QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(tr("Could not listen for the file chooser's answer."));
            }
        }
    });
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Action action = m_pendingAction;
    clearPending();
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (response != 0 || uris.isEmpty()) {
        emit cancelled();
        return;
    }

    const QUrl url(uris.first());
    if (action == Action::Open)
        emit openSelected(url);
    else if (action == Action::Save)
        emit saveSelected(url);
}

void PortalFilePicker::clearPending() {
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty()) {
        m_bus.disconnect(
            PortalService, m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    }
    m_pendingPath.clear();
    m_pendingAction = Action::None;
}
