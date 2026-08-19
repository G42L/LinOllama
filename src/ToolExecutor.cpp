#include "ToolExecutor.h"
#include "BuiltinTools.h"
#include "BraveSearchClient.h"
#include "StackOverflowSearchClient.h"
#include "OllamaClient.h"
#include "RagStore.h"

#include <QUuid>
#include <algorithm>

ToolExecutor::ToolExecutor(QObject *parent) : QObject(parent) {}

void ToolExecutor::setKnowledgeBase(OllamaClient *ollamaClient, RagStore *ragStore)
{
    const bool alreadyWired = (m_ollamaClient != nullptr);
    m_ollamaClient = ollamaClient;
    m_ragStore = ragStore;
    if (!alreadyWired && m_ollamaClient)
        connect(m_ollamaClient, &OllamaClient::embeddingsFetched, this, &ToolExecutor::onEmbeddingsFetched);
}

void ToolExecutor::executeToolCalls(const QString &conversationId, const QJsonArray &toolCalls)
{
    Batch &batch = m_batchesByConversation[conversationId];
    batch.results.clear();
    batch.results.resize(toolCalls.size());
    batch.pending = toolCalls.size();

    if (toolCalls.isEmpty()) {
        m_batchesByConversation.remove(conversationId);
        emit allToolCallsCompleted(conversationId, {});
        return;
    }

    for (int i = 0; i < toolCalls.size(); ++i) {
        const BuiltinTools::ParsedToolCall parsedCall = BuiltinTools::parseToolCall(toolCalls[i].toObject());
        const QString &name = parsedCall.name;
        const QJsonObject &arguments = parsedCall.arguments;

        if (name == BuiltinTools::kCalculate) {
            const QString expression = arguments.value("expression").toString();
            completeCall(conversationId, i, name, arguments, BuiltinTools::evaluateExpression(expression));
        } else if (name == BuiltinTools::kCurrentDateTime) {
            completeCall(conversationId, i, name, arguments, BuiltinTools::currentDateTimeText());
        } else if (name == BuiltinTools::kWebSearch) {
            const QString query = arguments.value("query").toString();
            // One BraveSearchClient per call rather than a shared member —
            // searchCompleted() doesn't carry a call index, so this is the
            // simplest way to keep concurrent searches from being confused
            // with each other. Small per-call overhead, but tool calls are
            // rare enough (one /api/chat round trip each) for that not to
            // matter.
            auto *client = new BraveSearchClient(m_braveApiKey, this);
            connect(client, &BraveSearchClient::searchCompleted, this,
                    [this, conversationId, i, name, arguments, client](const QString &, const QString &resultsText) {
                        client->deleteLater();
                        completeCall(conversationId, i, name, arguments,
                                     resultsText.isEmpty() ? QStringLiteral("No results found.") : resultsText);
                    });
            client->search(query);
        } else if (name == BuiltinTools::kStackOverflowSearch) {
            const QString query = arguments.value("query").toString();
            // Same one-client-per-call reasoning as web_search above.
            auto *client = new StackOverflowSearchClient(this);
            connect(client, &StackOverflowSearchClient::searchCompleted, this,
                    [this, conversationId, i, name, arguments, client](const QString &, const QString &resultsText) {
                        client->deleteLater();
                        completeCall(conversationId, i, name, arguments,
                                     resultsText.isEmpty() ? QStringLiteral("No results found.") : resultsText);
                    });
            client->search(query);
        } else if (name == BuiltinTools::kSearchKnowledgeBase) {
            const QString query = arguments.value("query").toString();
            if (!m_ollamaClient || !m_ragStore || m_embeddingModel.isEmpty()) {
                // Shouldn't normally happen — ChatWidget gates enabling this
                // tool on a configured knowledge base — but fail cleanly
                // rather than dereferencing a null pointer if it does.
                completeCall(conversationId, i, name, arguments,
                             QStringLiteral("Error: knowledge base isn't configured (check Settings)."));
                continue;
            }
            const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_pendingKbQueries[requestId] = PendingKbQuery{conversationId, i, name, arguments};
            m_ollamaClient->requestEmbeddings(requestId, m_embeddingModel, {query});
        } else {
            completeCall(conversationId, i, name, arguments,
                         QStringLiteral("Error: unknown tool \"%1\"").arg(name));
        }
    }
}

void ToolExecutor::completeCall(const QString &conversationId, int callIndex, const QString &name,
                                 const QJsonObject &arguments, const QString &resultText)
{
    auto it = m_batchesByConversation.find(conversationId);
    if (it == m_batchesByConversation.end())
        return; // stale — this conversation's batch was already replaced/removed

    Batch &batch = it.value();
    batch.results[callIndex] = ToolCallResult{name, arguments, resultText};
    emit toolCallCompleted(conversationId, callIndex, name, arguments, resultText);

    if (--batch.pending == 0) {
        const QVector<ToolCallResult> finished = batch.results;
        m_batchesByConversation.remove(conversationId);
        emit allToolCallsCompleted(conversationId, finished);
    }
}

void ToolExecutor::onEmbeddingsFetched(const QString &requestId, const QVector<QVector<float>> &embeddings)
{
    auto it = m_pendingKbQueries.find(requestId);
    if (it == m_pendingKbQueries.end())
        return; // not ours — e.g. one of RagIngestionController's own batch requests

    const PendingKbQuery query = it.value();
    m_pendingKbQueries.erase(it);

    if (embeddings.isEmpty() || embeddings.first().isEmpty()) {
        completeCall(query.conversationId, query.callIndex, query.name, query.arguments,
                     QStringLiteral("Error: couldn't embed the search query (check that Ollama is reachable)."));
        return;
    }

    // A fixed cutoff rather than a Settings-exposed knob (see this file's
    // own design notes) — below this, a match is more likely noise from an
    // unrelated document than something actually relevant, and returning it
    // anyway would just give the model something plausible-looking to
    // reason around incorrectly.
    constexpr float kMinRelevanceScore = 0.3f;

    QVector<RagStore::ScoredChunk> matches = m_ragStore->topKSimilar(embeddings.first(), /*topK=*/5);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                  [](const RagStore::ScoredChunk &c) { return c.score < kMinRelevanceScore; }),
                  matches.end());
    if (matches.isEmpty()) {
        completeCall(query.conversationId, query.callIndex, query.name, query.arguments,
                     QStringLiteral("No relevant documents found in the knowledge base."));
        return;
    }

    QStringList entries;
    for (int i = 0; i < matches.size(); ++i) {
        const RagStore::ScoredChunk &chunk = matches[i];
        entries << QString("%1. [%2, chunk %3] %4")
                       .arg(i + 1).arg(chunk.documentName).arg(chunk.chunkIndex + 1).arg(chunk.text);
    }
    const QString resultText = QString(
        "--- Knowledge base results for: %1 ---\n"
        "%2\n"
        "--- End of knowledge base results ---")
            .arg(query.arguments.value("query").toString(), entries.join("\n"));
    completeCall(query.conversationId, query.callIndex, query.name, query.arguments, resultText);
}
