#pragma once

#include <QString>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>

#include "OllamaClient.h" // for kKeepAliveUseServerDefault

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

    // Legacy field from the old eager-prefetch "Search the web" hack
    // (searched before the message was even sent, results glued onto the
    // user's own message). Superseded by real tool-calling (see toolCalls/
    // toolName below), but kept and still round-tripped so conversations
    // saved before that switchover keep working exactly as before.
    QString webSearchContext;

    // Present (non-empty) on an assistant message that called one or more
    // tools instead of (or before) answering — Ollama's own /api/chat
    // "message.tool_calls" shape, stored and resent verbatim so the model
    // sees exactly what it itself asked for on the next round. `content` is
    // typically empty on a message like this. See ChatWidget's tool-calling
    // round-trip (streamAssistantReplyForCurrentHistory() /
    // continueTurnAfterToolResults()) and ToolCallSectionWidget for how
    // this pairs up with the "tool" role message(s) that follow it.
    QJsonArray toolCalls;

    // Present on a role == "tool" message: which tool this result came
    // from — Ollama's API itself doesn't require this (matching is purely
    // positional against the preceding assistant message's toolCalls), but
    // it's what lets ToolCallSectionWidget label the result without having
    // to re-derive it from toolCalls[i].function.name at render time.
    QString toolName;

    // The model's reasoning trace (Ollama's message.thinking field — see
    // docs.ollama.com/capabilities/thinking) for an assistant message that
    // had one, so a reloaded conversation can still show a "Thought for Ns"
    // section above the answer instead of only the final content. Empty for
    // messages from before this field existed, or from a model that doesn't
    // support thinking — ChatWidget only shows the section at all when this
    // is non-empty (see renderConversation()).
    QString thinking;

    QJsonObject toJson() const
    {
        return QJsonObject{
            {"role", role},
            {"content", content},
            {"timestamp", timestamp.toString(Qt::ISODate)},
            {"attachmentNames", QJsonArray::fromStringList(attachmentNames)},
            {"attachmentsContext", attachmentsContext},
            {"imagesBase64", QJsonArray::fromStringList(imagesBase64)},
            {"webSearchContext", webSearchContext},
            {"toolCalls", toolCalls},
            {"toolName", toolName},
            {"thinking", thinking}
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
        m.toolCalls = obj.value("toolCalls").toArray();
        m.toolName = obj.value("toolName").toString();
        m.thinking = obj.value("thinking").toString();
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

    // How long Ollama keeps this conversation's model loaded after a reply
    // — the lighter-weight, per-conversation sibling of the tray/Settings
    // "offload model" button (which force-unloads whatever's loaded *right
    // now*). See OllamaClient::sendChatMessage()'s own comment for the
    // sentinel/-1/0/seconds meaning. Unlike model, this can be changed at
    // any point in the conversation's life, not just before its first
    // message — see ConversationStore::setConversationKeepAlive().
    int keepAliveSeconds = kKeepAliveUseServerDefault;

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
            {"messages", msgArray},
            {"keepAliveSeconds", keepAliveSeconds}
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
        // Missing key (conversations saved before this field existed) falls
        // back to the same sentinel as a freshly-created Conversation would
        // default to, not some arbitrary JSON default.
        c.keepAliveSeconds = obj.contains("keepAliveSeconds")
            ? obj.value("keepAliveSeconds").toInt()
            : kKeepAliveUseServerDefault;
        return c;
    }
};
