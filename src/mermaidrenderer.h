#pragma once

#include <QObject>
#include <QHash>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QQueue>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// Optional external mmdc. It uses its own Node/Puppeteer/Chromium installation;
// Write does not bundle a browser. Missing/failed renders remain Markdown.
class MermaidRenderer : public QObject {
    Q_OBJECT
public:
    explicit MermaidRenderer(QObject *parent = nullptr);
    ~MermaidRenderer() override;
    void setGeneration(const QString &generation);
    QImage request(const QString &fencedMarkdown, bool dark, int width);
    static bool supported(const QString &diagram);
signals:
    void ready();
private:
    friend class MermaidTests;
    MermaidRenderer(QObject *parent, const QProcessEnvironment &environment, const QStringList &fallbackDirectories);
    static QStringList defaultSearchDirectories();
    struct Job { QString key, diagram; bool dark; int width; quint64 generation; };
    void stopProcess();
    void startNext();
    void finish(bool success);
    QString m_executable;
    QStringList m_argumentPrefix;
#ifdef Q_OS_WIN
    struct WindowsJob;
    std::unique_ptr<WindowsJob> m_windowsJob;
#endif
    QProcess m_process;
    QTimer m_timeout;
    QQueue<Job> m_queue;
    QHash<QString, QImage> m_cache;
    QSet<QString> m_attempted;
    QString m_generationKey;
    quint64 m_generation = 0;
    std::unique_ptr<QTemporaryDir> m_directory;
    Job m_current;
    bool m_running = false;
};
