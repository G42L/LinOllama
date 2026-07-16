#include "WebSearchClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextDocument>

namespace {

// Search snippets come back with <span class="searchmatch"> highlighting
// and HTML entities (&#039; etc.) — routing through QTextDocument strips
// both properly rather than a regex tag-stripper that would leave entities
// behind.
QString stripHtml(const QString &fragment)
{
    QTextDocument doc;
    doc.setHtml(fragment);
    return doc.toPlainText().simplified();
}

} // namespace

WebSearchClient::WebSearchClient(QObject *parent) : QObject(parent) {}

void WebSearchClient::search(const QString &query)
{
    QUrl url("https://en.wikipedia.org/w/api.php");
    QUrlQuery q;
    q.addQueryItem("action", "query");
    q.addQueryItem("list", "search");
    q.addQueryItem("format", "json");
    q.addQueryItem("srlimit", "5");
    q.addQueryItem("srsearch", query);
    url.setQuery(q);

    QNetworkRequest request(url);
    // Wikimedia's API etiquette asks for a descriptive, non-generic User-Agent.
    request.setHeader(QNetworkRequest::UserAgentHeader, "LinOllama/1.0 (local desktop chat client)");

    QNetworkReply *reply = m_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, query]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit searchCompleted(query, QString());
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray results = doc.object().value("query").toObject().value("search").toArray();
        if (results.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        QStringList entries;
        int i = 0;
        for (const QJsonValue &v : results) {
            const QJsonObject obj = v.toObject();
            const QString title = obj.value("title").toString();
            const QString snippet = stripHtml(obj.value("snippet").toString());
            if (title.isEmpty())
                continue;
            entries << QString("%1. %2 — %3").arg(++i).arg(title, snippet);
        }

        if (entries.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        const QString resultsText = QString(
            "--- Wikipedia search results for: %1 ---\n"
            "%2\n"
            "--- End of search results (source: Wikipedia, not a general web search) ---")
                .arg(query, entries.join("\n"));
        emit searchCompleted(query, resultsText);
    });
}
