// The macOS backend follows the system appearance through Qt's cross-platform
// QStyleHints; the freedesktop portal and its text-size knob do not exist here.

#include "systemtheme.h"

bool SystemTheme::detectDarkMode() const {
    bool known = false;
    const bool dark = qtDarkMode(&known);
    return known ? dark : true;
}

qreal SystemTheme::detectTextScale() const {
    return 1.0;
}

void SystemTheme::startWatching() {}

void SystemTheme::portalSettingChanged(const QString &, const QString &,
                                       const QVariant &) {}
