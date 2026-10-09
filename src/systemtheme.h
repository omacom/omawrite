#pragma once

#include <QObject>

class QDBusVariant;

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

#ifdef QT_DBUS_LIB
private slots:
    void handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                    const QDBusVariant &value);
#endif

private:
    bool detectDarkMode() const;
    bool portalDarkMode(bool *known) const;
    bool qtDarkMode(bool *known) const;
    void setDarkMode(bool darkMode);
    qreal detectTextScale() const;
    void setTextScale(qreal textScale);

    bool m_darkMode = true;
    qreal m_textScale = 1.0;
};
