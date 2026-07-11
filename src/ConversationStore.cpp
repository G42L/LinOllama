#include "ConversationStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>
#include <QDebug>
#include <algorithm>

ConversationStore::ConversationStore(QObject *parent) : QObject(parent) {}

QString ConversationStore::storageDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/conversations";
}

QString ConversationStore::filePathFor(const QString &id) const
{
    return storageDir() + "/" + id + ".json";
}

void ConversationStore::loadAll()
{
    m_conversations.clear();

    QDir dir(storageDir());
    if (!dir.exists()) {
        dir.mkpath(".");
        emit conversationListChanged();
        return;
    }

    const QStringList files = dir.entryList({"*.json"}, QDir::Files);
    for (const QString &fileName : files) {
        QFile file(dir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly))
            continue;

        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject())
            continue;

        m_conversations.append(Conversation::fromJson(doc.object()));
    }

    // Newest first, so a fresh conversation always lands at the top of the sidebar.
    std::sort(m_conversations.begin(), m_conversations.end(),
              [](const Conversation &a, const Conversation &b) {
                  return a.createdAt > b.createdAt;
              });

    emit conversationListChanged();
}

const Conversation *ConversationStore::find(const QString &id) const
{
    for (const Conversation &c : m_conversations) {
        if (c.id == id)
            return &c;
    }
    return nullptr;
}

int ConversationStore::indexOf(const QString &id) const
{
    for (int i = 0; i < m_conversations.size(); ++i) {
        if (m_conversations[i].id == id)
            return i;
    }
    return -1;
}

QString ConversationStore::createConversation(const QString &model)
{
    Conversation c;
    c.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    c.title = "New conversation";
    c.model = model;
    c.createdAt = QDateTime::currentDateTime();

    m_conversations.prepend(c);
    persist(c);
    emit conversationListChanged();
    return c.id;
}

QString ConversationStore::createConversationWithMessages(const QString &model, const QVector<ChatMessage> &messages)
{
    const QString id = createConversation(model);
    for (const ChatMessage &m : messages)
        appendMessage(id, m);
    return id;
}

void ConversationStore::appendMessage(const QString &conversationId, const ChatMessage &message)
{
    const int idx = indexOf(conversationId);
    if (idx < 0)
        return;

    Conversation &c = m_conversations[idx];
    c.messages.append(message);

    // Derive a title from the first user message the first time one arrives,
    // rather than leaving every conversation labeled "New conversation".
    if (c.title == "New conversation" && message.role == "user") {
        QString derived = message.content.left(60).simplified();
        if (message.content.length() > 60)
            derived += "...";
        // An attachment-only message (no typed text) still deserves a real
        // title rather than staying "New conversation" forever.
        if (derived.isEmpty() && !message.attachmentNames.isEmpty())
            derived = message.attachmentNames.join(", ").left(60);
        if (!derived.isEmpty())
            c.title = derived;
    }

    persist(c);
    // Deliberately not emitting conversationListChanged() here — see header
    // comment. The sidebar's title only needs to refresh once, which the
    // caller (ChatWidget/MainWindow) can trigger by re-reading find() after
    // a user message is sent, without doing it on every streamed token.
}

void ConversationStore::updateStreamingAssistantMessage(const QString &conversationId, const QString &fullContentSoFar)
{
    const int idx = indexOf(conversationId);
    if (idx < 0)
        return;

    Conversation &c = m_conversations[idx];
    if (!c.messages.isEmpty() && c.messages.last().role == "assistant") {
        c.messages.last().content = fullContentSoFar;
    } else {
        ChatMessage m;
        m.role = "assistant";
        m.content = fullContentSoFar;
        m.timestamp = QDateTime::currentDateTime();
        c.messages.append(m);
    }
    // Deliberately not persisting on every token — that would be a disk
    // write per streamed chunk. The caller persists once when streaming
    // finishes (chatDone), via appendMessage's persist() call on the final
    // content, or an explicit final persist() call.
}

void ConversationStore::finalizeStreamingAssistantMessage(const QString &conversationId)
{
    const int idx = indexOf(conversationId);
    if (idx < 0)
        return;
    persist(m_conversations[idx]);
}

void ConversationStore::renameConversation(const QString &id, const QString &newTitle)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return;
    m_conversations[idx].title = newTitle;
    persist(m_conversations[idx]);
    emit conversationListChanged();
}

void ConversationStore::deleteConversation(const QString &id)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return;

    QFile::remove(filePathFor(id));
    m_conversations.removeAt(idx);
    emit conversationListChanged();
}

void ConversationStore::truncateMessagesFrom(const QString &conversationId, int index)
{
    const int idx = indexOf(conversationId);
    if (idx < 0)
        return;

    Conversation &c = m_conversations[idx];
    if (index < 0 || index >= c.messages.size())
        return;

    c.messages.resize(index);
    persist(c);
}

void ConversationStore::setConversationModel(const QString &id, const QString &model)
{
    const int idx = indexOf(id);
    if (idx < 0)
        return;

    Conversation &c = m_conversations[idx];
    if (c.model == model)
        return;

    c.model = model;
    persist(c);
}

void ConversationStore::persist(const Conversation &conversation) const
{
    QDir dir(storageDir());
    if (!dir.exists())
        dir.mkpath(".");

    QFile file(filePathFor(conversation.id));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Failed to write conversation file:" << file.fileName();
        return;
    }

    const QJsonDocument doc(conversation.toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
}
