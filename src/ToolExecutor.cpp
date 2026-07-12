#include "ToolExecutor.h"
#include "BuiltinTools.h"
#include "WebSearchClient.h"
#include "StackOverflowSearchClient.h"

ToolExecutor::ToolExecutor(QObject *parent) : QObject(parent) {}

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
            // One WebSearchClient per call rather than a shared member —
            // searchCompleted() doesn't carry a call index, so this is the
            // simplest way to keep concurrent searches from being confused
            // with each other. Small per-call overhead, but tool calls are
            // rare enough (one /api/chat round trip each) for that not to
            // matter.
            auto *client = new WebSearchClient(this);
            connect(client, &WebSearchClient::searchCompleted, this,
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
