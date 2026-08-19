#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QString>

// General web search backend for the "web_search" tool — backed by the
// Brave Search API (api.search.brave.com), a keyed general-purpose search
// engine (free tier: 2,000 queries/month). Supersedes the app's earlier
// Wikipedia-only WebSearchClient, which existed specifically because no
// free/keyless general web search API exists (see that class's own former
// header comment) — Brave requires a user-supplied key (Settings → Inputs)
// but in exchange covers current events and arbitrary topics, not just
// encyclopedic ones.
class BraveSearchClient : public QObject
{
    Q_OBJECT

public:
    explicit BraveSearchClient(const QString &apiKey, QObject *parent = nullptr);

    // Result always arrives via searchCompleted(). An empty resultsText
    // means no results were found; a resultsText starting with "Error: "
    // means the request itself failed in a way worth surfacing to the
    // model/user (e.g. an invalid or missing API key) rather than being
    // treated as an ordinary "no results" case.
    void search(const QString &query);

signals:
    void searchCompleted(const QString &query, const QString &resultsText);

private:
    QString m_apiKey;
    QNetworkAccessManager m_manager;
};
