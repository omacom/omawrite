// The Linux backend reads appearance and text size from the xdg-desktop-portal
// (what GNOME and omarchy drive) and keeps listening for changes over D-Bus.

#include "systemtheme.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>

namespace {
QVariant unwrapVariant(QVariant value) {
    while (value.canConvert<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}

bool colorSchemeIsDark(const QVariant &value, bool *known) {
    bool ok = false;
    const uint scheme = unwrapVariant(value).toUInt(&ok);
    if (!ok)
        return false;

    if (scheme == 1) {
        *known = true;
        return true;
    }
    if (scheme == 2) {
        *known = true;
        return false;
    }

    return false;
}

// GNOME's text-scaling-factor is the desktop-wide "apparent text size" knob;
// omarchy drives it from `omarchy display text size`, anchored so the default
// 12px maps to 1.0. Ignore nonsense values and cap the range GNOME allows.
qreal sanitizedTextScale(const QVariant &value, bool *known) {
    bool ok = false;
    const qreal scale = unwrapVariant(value).toDouble(&ok);
    if (!ok || scale <= 0)
        return 1.0;

    *known = true;
    return qBound(0.5, scale, 3.0);
}

// Ask the desktop portal for a single setting, returning an invalid variant
// when the portal is missing or slow to answer; the short timeout keeps a
// stalled portal from holding up the GUI thread.
QVariant portalSetting(const QString &nameSpace, const QString &key) {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};

    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("Read"));
    request << nameSpace << key;

    const QDBusReply<QDBusVariant> reply(bus.call(request, QDBus::Block, 150));
    if (!reply.isValid())
        return {};

    return reply.value().variant();
}

bool gsettingsSchemeIsDark(const QVariant &value, bool *known) {
    const QString scheme = unwrapVariant(value).toString();
    if (scheme.contains(QStringLiteral("prefer-dark"))) {
        *known = true;
        return true;
    }
    if (scheme.contains(QStringLiteral("prefer-light"))) {
        *known = true;
        return false;
    }

    return false;
}
}

bool SystemTheme::detectDarkMode() const {
    bool known = false;

    const QVariant scheme = portalSetting(QStringLiteral("org.freedesktop.appearance"),
                                          QStringLiteral("color-scheme"));
    if (scheme.isValid()) {
        const bool dark = colorSchemeIsDark(scheme, &known);
        if (known)
            return dark;
    }

    const bool qtDark = qtDarkMode(&known);
    if (known)
        return qtDark;

    return true;
}

qreal SystemTheme::detectTextScale() const {
    const QVariant factor = portalSetting(QStringLiteral("org.gnome.desktop.interface"),
                                          QStringLiteral("text-scaling-factor"));
    if (!factor.isValid())
        return 1.0;

    bool known = false;
    return sanitizedTextScale(factor, &known);
}

void SystemTheme::startWatching() {
    QDBusConnection::sessionBus().connect(
        QString(),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Settings"),
        QStringLiteral("SettingChanged"),
        this,
        SLOT(portalSettingChanged(QString,QString,QVariant)));
}

void SystemTheme::portalSettingChanged(const QString &nameSpace, const QString &key,
                                       const QVariant &value) {
    if (key == QStringLiteral("text-scaling-factor")) {
        if (nameSpace != QStringLiteral("org.gnome.desktop.interface"))
            return;

        bool known = false;
        const qreal scale = sanitizedTextScale(value, &known);
        if (known)
            setTextScale(scale);
        return;
    }

    if (key != QStringLiteral("color-scheme"))
        return;

    bool known = false;
    bool dark = false;
    if (nameSpace == QStringLiteral("org.freedesktop.appearance"))
        dark = colorSchemeIsDark(value, &known);
    else if (nameSpace == QStringLiteral("org.gnome.desktop.interface"))
        dark = gsettingsSchemeIsDark(value, &known);
    else
        return;

    if (known)
        setDarkMode(dark);
    else
        refresh();
}
