#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QVector>

// One resolved tool call, ready to be turned into a "tool" role message and
// (for display) a ToolCallSectionWidget — see ChatWidget's tool-calling
// round-trip.
struct ToolCallResult
{
    QString name;
    QJsonObject arguments;
    QString resultText;
};

// Executes a batch of tool_calls (Ollama's own /api/chat "message.tool_calls"
// array — see OllamaClient::chatToolCalls()) against the app's built-in
// tools (see BuiltinTools.h) and reports back once every call in the batch
// has a result. Calculator/date-time resolve synchronously; web_search is a
// real network round trip, so results generally do NOT arrive in the same
// order they were requested — callers should key off callIndex, not
// assume ordering.
class ToolExecutor : public QObject
{
    Q_OBJECT

public:
    explicit ToolExecutor(QObject *parent = nullptr);

    // Key used to authenticate web_search calls against the Brave Search
    // API (see BraveSearchClient). Settings' "Inputs" tab is the source of
    // truth; the caller (ChatWidget) mirrors it here whenever it changes,
    // including once at startup. An empty key isn't specially handled
    // here — ChatWidget is responsible for not letting web_search be
    // enabled in the first place while no key is configured (see its own
    // Tools-menu toggle), so a call reaching this class with an empty key
    // shouldn't normally happen; if it does anyway, BraveSearchClient's own
    // 401/403 handling reports it as an actionable error.
    void setBraveApiKey(const QString &key) { m_braveApiKey = key; }

    // Starts executing every call in toolCalls for conversationId. Replaces
    // any batch already in flight for that same conversationId (shouldn't
    // normally happen — ChatWidget only starts one tool round at a time per
    // conversation — but this keeps a stray late result from an abandoned
    // batch from ever being reported).
    void executeToolCalls(const QString &conversationId, const QJsonArray &toolCalls);

signals:
    // One per call, as each resolves — lets the caller flip a per-call
    // "Running…" placeholder to its real result incrementally, rather than
    // waiting for the slowest call in the batch.
    void toolCallCompleted(const QString &conversationId, int callIndex, const QString &toolName,
                            const QJsonObject &arguments, const QString &resultText);

    // Fired once every call in the batch has resolved, in callIndex order
    // (unlike toolCallCompleted(), which fires in resolution order).
    void allToolCallsCompleted(const QString &conversationId, const QVector<ToolCallResult> &results);

private:
    void completeCall(const QString &conversationId, int callIndex, const QString &name,
                       const QJsonObject &arguments, const QString &resultText);

    struct Batch
    {
        QVector<ToolCallResult> results;
        int pending = 0;
    };
    // Keyed by conversationId — one in-flight batch per conversation, same
    // one-per-key convention as OllamaClient's m_chatStreams.
    QHash<QString, Batch> m_batchesByConversation;
    QString m_braveApiKey;
};
