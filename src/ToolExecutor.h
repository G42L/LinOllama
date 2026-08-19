#pragma once

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QVector>

class OllamaClient;
class RagStore;

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

    // Wires the shared OllamaClient/RagStore instances (both app-level
    // singletons, owned by MainWindow — see main.cpp) that
    // search_knowledge_base needs: embedding the query goes through
    // ollamaClient, similarity search goes through ragStore. Connects to
    // ollamaClient's embeddingsFetched() the first time this is called
    // (guarded against a second call reconnecting it, since ChatWidget
    // calls this once at construction with values that never change
    // afterward — unlike setBraveApiKey/setEmbeddingModel, there's no live
    // Settings toggle for *which* OllamaClient/RagStore instance to use).
    void setKnowledgeBase(OllamaClient *ollamaClient, RagStore *ragStore);

    // Embedding model used to embed the query text for search_knowledge_base
    // — mirrors Settings' Knowledge Base tab, same "caller mirrors on every
    // change" pattern as setBraveApiKey. Empty means "not configured",
    // handled the same defensive way BraveSearchClient's empty-key case is:
    // ChatWidget is responsible for not letting the tool be enabled while
    // this is empty, but a call reaching this class anyway just fails
    // cleanly rather than crashing.
    void setEmbeddingModel(const QString &model) { m_embeddingModel = model; }

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

private slots:
    // Connected to OllamaClient::embeddingsFetched() by setKnowledgeBase().
    // Filters by requestId against m_pendingKbQueries, since that signal is
    // shared with RagIngestionController's own (much larger, batched)
    // embedding requests during document ingestion — a query embedding
    // request from here always looks like a single-vector result, which is
    // otherwise indistinguishable from an ingestion batch of exactly one
    // chunk, so the requestId filter is what actually disambiguates them.
    void onEmbeddingsFetched(const QString &requestId, const QVector<QVector<float>> &embeddings);

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

    OllamaClient *m_ollamaClient = nullptr; // not owned — see setKnowledgeBase()
    RagStore *m_ragStore = nullptr;         // not owned
    QString m_embeddingModel;

    // One in-flight search_knowledge_base query embedding request, keyed by
    // its requestId — mirrors m_batchesByConversation's role but for the
    // one extra async hop (embed query -> topKSimilar()) this tool needs
    // that no other tool does.
    struct PendingKbQuery
    {
        QString conversationId;
        int callIndex = 0;
        QString name;
        QJsonObject arguments;
    };
    QHash<QString, PendingKbQuery> m_pendingKbQueries;
};
