#pragma once

#include <QObject>
#include <QtSystemDetection>

#ifdef Q_OS_LINUX
class QDBusVariant;
#endif

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
#ifdef Q_OS_LINUX
    void handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                    const QDBusVariant &value);
#endif

private:
    bool detectDarkMode() const;
#ifdef Q_OS_LINUX
    bool portalDarkMode(bool *known) const;
#endif
    bool qtDarkMode(bool *known) const;
    void setDarkMode(bool darkMode);
    qreal detectTextScale() const;
    void setTextScale(qreal textScale);

    bool m_darkMode = true;
    qreal m_textScale = 1.0;
};
