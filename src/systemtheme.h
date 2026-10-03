#pragma once

#include <QObject>
#include <QVariant>

// Follows the desktop's dark mode and text size. The platform details live in
// exactly one of systemtheme_linux.cpp or systemtheme_macos.cpp; this header
// and the shared systemtheme.cpp stay platform-neutral.
class SystemTheme : public QObject {
    Q_OBJECT

public:
    explicit SystemTheme(QObject *parent = nullptr);

    bool darkMode() const { return m_darkMode; }
    qreal textScale() const { return m_textScale; }

signals:
    void darkModeChanged(bool darkMode);
    void textScaleChanged(qreal textScale);

public slots:
    void refresh();

private slots:
    // The freedesktop portal's SettingChanged signal lands here on Linux;
    // D-Bus delivers the variant argument as a plain QVariant.
    void portalSettingChanged(const QString &nameSpace, const QString &key,
                              const QVariant &value);

private:
    // Platform hooks; each systemtheme_<platform>.cpp defines all of them.
    bool detectDarkMode() const;
    qreal detectTextScale() const;
    void startWatching();

    bool qtDarkMode(bool *known) const;
    void setDarkMode(bool darkMode);
    void setTextScale(qreal textScale);

    bool m_darkMode = true;
    qreal m_textScale = 1.0;
};
