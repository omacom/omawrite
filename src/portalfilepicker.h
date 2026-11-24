#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

class QDBusPendingCallWatcher;

// The open and save dialogs through xdg-desktop-portal's FileChooser, so they
// always use the desktop's own out-of-process chooser rather than falling back
// to Qt Quick's non-native QML FileDialog.
class PortalFilePicker : public QObject {
    Q_OBJECT

public:
    // The session bus; tests pass a private one.
    explicit PortalFilePicker(QObject *parent = nullptr,
                              const QDBusConnection &bus = QDBusConnection::sessionBus());
    ~PortalFilePicker() override;

    Q_INVOKABLE void openFile(const QString &folder = {});
    Q_INVOKABLE void saveFile(const QUrl &suggestedUrl);

signals:
    void openSelected(const QUrl &url);
    void saveSelected(const QUrl &url);
    void cancelled();
    void failed(const QString &message);

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action { None, Open, Save };

    void requestFile(const QString &method, const QString &title, QVariantMap options, Action action);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    void closePendingRequest();

    QDBusConnection m_bus;
    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    // Bumped per request so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
