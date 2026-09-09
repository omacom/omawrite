#include "buffersession.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QUuid>

namespace {
constexpr int sessionVersion = 1;

QJsonObject jsonBuffer(const QVariantMap &buffer) {
    return {{QStringLiteral("id"), buffer.value(QStringLiteral("id")).toString()},
            {QStringLiteral("fileUrl"), buffer.value(QStringLiteral("fileUrl")).toString()},
            {QStringLiteral("text"), buffer.value(QStringLiteral("text")).toString()},
            {QStringLiteral("cursorPosition"), buffer.value(QStringLiteral("cursorPosition")).toInt()},
            {QStringLiteral("selectionStart"), buffer.value(QStringLiteral("selectionStart")).toInt()},
            {QStringLiteral("selectionEnd"), buffer.value(QStringLiteral("selectionEnd")).toInt()},
            {QStringLiteral("modified"), buffer.value(QStringLiteral("modified")).toBool()}};
}

bool bufferFromJson(const QJsonValue &value, QVariantMap *buffer) {
    if (!value.isObject())
        return false;

    const QJsonObject object = value.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    const QJsonValue fileUrl = object.value(QStringLiteral("fileUrl"));
    const QJsonValue text = object.value(QStringLiteral("text"));
    const QJsonValue cursorPosition = object.value(QStringLiteral("cursorPosition"));
    const QJsonValue selectionStart = object.value(QStringLiteral("selectionStart"));
    const QJsonValue selectionEnd = object.value(QStringLiteral("selectionEnd"));
    const QJsonValue modified = object.value(QStringLiteral("modified"));
    if (!id.isString() || id.toString().isEmpty() || !fileUrl.isString() || !text.isString()
            || !cursorPosition.isDouble() || !selectionStart.isDouble() || !selectionEnd.isDouble()
            || !modified.isBool())
        return false;

    const int textLength = text.toString().size();
    *buffer = {{QStringLiteral("id"), id.toString()},
               {QStringLiteral("fileUrl"), fileUrl.toString()},
               {QStringLiteral("text"), text.toString()},
               {QStringLiteral("cursorPosition"), qBound(0, cursorPosition.toInt(), textLength)},
               {QStringLiteral("selectionStart"), qBound(0, selectionStart.toInt(), textLength)},
               {QStringLiteral("selectionEnd"), qBound(0, selectionEnd.toInt(), textLength)},
               {QStringLiteral("modified"), modified.toBool()}};
    return true;
}
}

BufferSession::BufferSession(const QString &stateDirectory) : m_stateDirectory(stateDirectory) {}

QVariantList BufferSession::buffers() const {
    return m_buffers;
}

QString BufferSession::activeBufferId() const {
    return m_activeBufferId;
}

QString BufferSession::createBuffer() {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_buffers.append(QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("fileUrl"), QString()},
                                 {QStringLiteral("text"), QString()},
                                 {QStringLiteral("cursorPosition"), 0},
                                 {QStringLiteral("selectionStart"), 0},
                                 {QStringLiteral("selectionEnd"), 0},
                                 {QStringLiteral("modified"), false}});
    m_activeBufferId = id;
    return id;
}

QString BufferSession::openBuffer(const QUrl &fileUrl, const QString &text) {
    const QString url = fileUrl.toString();
    for (const QVariant &value : m_buffers) {
        const QVariantMap buffer = value.toMap();
        if (buffer.value(QStringLiteral("fileUrl")).toString() == url) {
            m_activeBufferId = buffer.value(QStringLiteral("id")).toString();
            return m_activeBufferId;
        }
    }

    const QString id = createBuffer();
    updateBuffer(id, url, text, 0, 0, 0, false);
    return id;
}

bool BufferSession::selectBuffer(const QString &id) {
    for (const QVariant &value : m_buffers) {
        if (value.toMap().value(QStringLiteral("id")).toString() == id) {
            m_activeBufferId = id;
            return true;
        }
    }
    return false;
}

bool BufferSession::updateBuffer(const QString &id, const QString &fileUrl, const QString &text,
                                 int cursorPosition, int selectionStart, int selectionEnd,
                                 bool modified) {
    for (QVariant &value : m_buffers) {
        QVariantMap buffer = value.toMap();
        if (buffer.value(QStringLiteral("id")).toString() != id)
            continue;

        buffer.insert(QStringLiteral("fileUrl"), fileUrl);
        buffer.insert(QStringLiteral("text"), text);
        buffer.insert(QStringLiteral("cursorPosition"), qBound(0, cursorPosition, text.size()));
        buffer.insert(QStringLiteral("selectionStart"), qBound(0, selectionStart, text.size()));
        buffer.insert(QStringLiteral("selectionEnd"), qBound(0, selectionEnd, text.size()));
        buffer.insert(QStringLiteral("modified"), modified);
        value = buffer;
        return true;
    }
    return false;
}

bool BufferSession::restore() {
    QFile file(sessionPath());
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return false;

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != sessionVersion
            || !root.value(QStringLiteral("activeBufferId")).isString()
            || !root.value(QStringLiteral("buffers")).isArray())
        return false;

    QVariantList restoredBuffers;
    QSet<QString> identifiers;
    for (const QJsonValue &value : root.value(QStringLiteral("buffers")).toArray()) {
        QVariantMap buffer;
        if (!bufferFromJson(value, &buffer) || identifiers.contains(buffer.value(QStringLiteral("id")).toString()))
            return false;
        identifiers.insert(buffer.value(QStringLiteral("id")).toString());
        restoredBuffers.append(buffer);
    }
    const QString activeBufferId = root.value(QStringLiteral("activeBufferId")).toString();
    if (restoredBuffers.isEmpty() || !identifiers.contains(activeBufferId))
        return false;

    m_buffers = restoredBuffers;
    m_activeBufferId = activeBufferId;
    return true;
}

bool BufferSession::saveNow() const {
    if (m_buffers.isEmpty() || m_activeBufferId.isEmpty())
        return false;

    QDir().mkpath(m_stateDirectory);
    QSaveFile file(sessionPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QJsonArray buffers;
    for (const QVariant &value : m_buffers)
        buffers.append(jsonBuffer(value.toMap()));
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), sessionVersion},
                                         {QStringLiteral("activeBufferId"), m_activeBufferId},
                                         {QStringLiteral("buffers"), buffers}})
                   .toJson(QJsonDocument::Compact));
    return file.commit();
}

QString BufferSession::sessionPath() const {
    return QDir(m_stateDirectory).filePath(QStringLiteral("session.json"));
}
