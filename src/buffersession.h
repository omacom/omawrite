#pragma once

#include <QString>
#include <QUrl>
#include <QVariantList>

class BufferSession {
public:
    explicit BufferSession(const QString &stateDirectory);

    QVariantList buffers() const;
    QString activeBufferId() const;
    QString createBuffer();
    QString openBuffer(const QUrl &fileUrl, const QString &text);
    bool selectBuffer(const QString &id);
    bool closeBuffer(const QString &id);
    bool updateBuffer(const QString &id, const QString &fileUrl, const QString &text,
                      int cursorPosition, int selectionStart, int selectionEnd, bool modified);
    bool restore();
    bool saveNow() const;

private:
    QString sessionPath() const;

    QString m_stateDirectory;
    QVariantList m_buffers;
    QString m_activeBufferId;
};
