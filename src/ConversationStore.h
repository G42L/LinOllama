#pragma once

#include <QObject>
#include <QVector>
#include "Conversation.h"

// Owns the in-memory list of conversations and mirrors it to disk as one
// JSON file per conversation, under
// QStandardPaths::AppDataLocation + "/conversations/<id>.json".
//
// Kept deliberately simple: writes happen synchronously and immediately
// after any mutation. Conversation histories are small text, so this is
// cheap enough to not need batching or a background thread for a
// single-user desktop app.
class ConversationStore : public QObject
{
    Q_OBJECT

public:
    explicit ConversationStore(QObject *parent = nullptr);

    // Loads all conversations found on disk, newest-created first.
    void loadAll();

    const QVector<Conversation> &conversations() const { return m_conversations; }
    const Conversation *find(const QString &id) const;

    // Creates a new, empty conversation pinned to the given model, persists
    // it immediately, and returns its id.
    QString createConversation(const QString &model);

    // Creates a new conversation pre-seeded with `messages` (appended one at
    // a time via appendMessage(), so title-deriving and persistence work
    // exactly as they would for a normal conversation) — used when editing
    // an earlier message forks into a new chat rather than truncating the
    // original (see ChatWidget::editMessage()). Returns the new id.
    QString createConversationWithMessages(const QString &model, const QVector<ChatMessage> &messages);

    // Adds a conversation read back from an exported JSON file (see
    // MainWindow's sidebar "Export conversation…"/"Import conversation…")
    // as a new entry — a fresh id is always assigned (ignoring whatever was
    // in the file), so importing the same export twice, or an export
    // that happens to collide with an existing conversation's id, can
    // never overwrite anything already in the store. Everything else
    // (title, model, messages, keepAliveSeconds, createdAt) is kept as-is
    // from the file. Persists immediately and returns the new id.
    QString importConversation(Conversation conversation);

    // Appends a message to the given conversation, auto-derives a title
    // from the first user message if the conversation doesn't have one yet,
    // and persists the change.
    void appendMessage(const QString &conversationId, const ChatMessage &message);

    // Replaces (or appends, if empty) the last assistant message — used
    // while a streaming response is arriving token-by-token, so we don't
    // write partial garbage as a permanent new message each time. In-memory
    // only; call finalizeStreamingAssistantMessage() once streaming ends.
    void updateStreamingAssistantMessage(const QString &conversationId, const QString &fullContentSoFar);

    // Persists the conversation to disk once streaming has finished —
    // call this after the last updateStreamingAssistantMessage() call.
    void finalizeStreamingAssistantMessage(const QString &conversationId);

    void renameConversation(const QString &id, const QString &newTitle);
    void deleteConversation(const QString &id);

    // Updates the conversation's pinned model and persists the change —
    // only meaningful before it has any messages (ChatWidget disables its
    // model combo once a conversation has messages, since Ollama has no
    // notion of switching models mid-chat). Called as the user changes the
    // combo for a still-empty conversation, so streamAssistantReplyForCurrentHistory()
    // reading this conversation's own model back stays in sync with what
    // they actually picked, not just whatever it was created with.
    void setConversationModel(const QString &id, const QString &model);

    // Updates the conversation's own keep_alive preference and persists the
    // change — unlike setConversationModel(), this is never locked once a
    // conversation has messages, since it only affects how long the model
    // stays loaded *after* the next reply, not anything about the request
    // history itself. Takes effect starting with the next turn sent.
    void setConversationKeepAlive(const QString &id, int keepAliveSeconds);

    // Drops every message from `index` onward (inclusive), and persists the
    // change — used when editing or retrying an earlier user message, since
    // everything after it has to be regenerated. No-op if index is out of range.
    void truncateMessagesFrom(const QString &conversationId, int index);

signals:
    // Emitted after any structural change (create/delete/rename/load) —
    // NOT on every streaming token, to avoid the sidebar list churning
    // mid-response. Message content updates are read directly by whichever
    // widget is displaying that conversation.
    void conversationListChanged();

private:
    QString storageDir() const;
    QString filePathFor(const QString &id) const;
    void persist(const Conversation &conversation) const;
    int indexOf(const QString &id) const;

    QVector<Conversation> m_conversations;
};
