#pragma once

#include <QObject>
#include <QUrl>
#include <memory>
#include <vector>

class Backend;
class QWindow;
class SystemTheme;

// Owns one independent QML engine/backend per window. All routing/writes run on
// the GUI thread, so target checks and synchronous saves cannot interleave.
class WindowManager : public QObject {
    Q_OBJECT
public:
    explicit WindowManager(const QUrl &mainQml = QUrl(QStringLiteral("qrc:/Main.qml")), QObject *parent = nullptr);
    ~WindowManager() override;
    void start(const QStringList &files);
    void retranslate();
    void openFiles(const QStringList &files);
    QWindow *openFile(const QUrl &url);
    QWindow *newWindow();
    QList<QWindow *> windows() const;
    Backend *backendFor(QWindow *window) const;
    static QString documentKey(const QUrl &url);
signals:
    void noWindowsRemaining();
    void errorOccurred(const QString &message);
private slots:
    void releaseClosedWindow();
private:
    struct Entry;
    Entry *create(bool recoverDraft);
    Entry *owner(const QUrl &url) const;
    void remove(Entry *entry);
    void activate(Entry *entry);
    QUrl m_mainQml;
    std::unique_ptr<SystemTheme> m_theme;
    std::vector<std::unique_ptr<Entry>> m_entries;
    Entry *m_startupBlank = nullptr;
};
