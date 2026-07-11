#pragma once

#include <QString>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>

// A single turn in a conversation. role is "user", "assistant", or "system"
// to match Ollama's /api/chat message schema directly — no translation
// layer needed between what's stored and what's sent to the API.
struct ChatMessage
{
    QString role;
    QString content;
    QDateTime timestamp;

    // File attachments (see ChatWidget's "+" button). attachmentNames is
    // display-only, for the small "Attached: ..." line shown in the bubble.
    // attachmentsContext holds the text extracted from non-image
    // attachments at attach-time (delimited per file), kept separate from
    // `content` so the bubble only ever shows what the person actually
    // typed — it's appended back on when building the API request (see
    // ChatWidget::onSendClicked), on every subsequent turn too, since Ollama
    // resends full history each call. imagesBase64 holds image attachments,
    // sent via Ollama's per-message "images" field for vision-capable models.
    QStringList attachmentNames;
    QString attachmentsContext;
    QStringList imagesBase64;

    // Wikipedia search results fetched once (see WebSearchClient) when this
    // message was sent with the "Search the web" tool on — kept separate
    // from `content` for the same reason attachmentsContext is: the bubble
    // shows only what the person actually typed, while this gets appended
    // back on when building the API request, on every subsequent turn too.
    QString webSearchContext;

    QJsonObject toJson() const
    {
        return QJsonObject{
            {"role", role},
            {"content", content},
            {"timestamp", timestamp.toString(Qt::ISODate)},
            {"attachmentNames", QJsonArray::fromStringList(attachmentNames)},
            {"attachmentsContext", attachmentsContext},
            {"imagesBase64", QJsonArray::fromStringList(imagesBase64)},
            {"webSearchContext", webSearchContext}
        };
    }

    static ChatMessage fromJson(const QJsonObject &obj)
    {
        ChatMessage m;
        m.role = obj.value("role").toString();
        m.content = obj.value("content").toString();
        m.timestamp = QDateTime::fromString(obj.value("timestamp").toString(), Qt::ISODate);
        for (const QJsonValue &v : obj.value("attachmentNames").toArray())
            m.attachmentNames << v.toString();
        m.attachmentsContext = obj.value("attachmentsContext").toString();
        for (const QJsonValue &v : obj.value("imagesBase64").toArray())
            m.imagesBase64 << v.toString();
        m.webSearchContext = obj.value("webSearchContext").toString();
        return m;
    }
};

struct Conversation
{
    QString id;          // UUID, also the filename stem on disk
    QString title;        // shown in the sidebar; derived from the first user message until renamed
    QString model;         // Ollama model name this conversation is pinned to
    QDateTime createdAt;
    QVector<ChatMessage> messages;

    QJsonObject toJson() const
    {
        QJsonArray msgArray;
        for (const ChatMessage &m : messages)
            msgArray.append(m.toJson());

        return QJsonObject{
            {"id", id},
            {"title", title},
            {"model", model},
            {"createdAt", createdAt.toString(Qt::ISODate)},
            {"messages", msgArray}
        };
    }

    static Conversation fromJson(const QJsonObject &obj)
    {
        Conversation c;
        c.id = obj.value("id").toString();
        c.title = obj.value("title").toString();
        c.model = obj.value("model").toString();
        c.createdAt = QDateTime::fromString(obj.value("createdAt").toString(), Qt::ISODate);
        for (const QJsonValue &v : obj.value("messages").toArray())
            c.messages.append(ChatMessage::fromJson(v.toObject()));
        return c;
    }
};
