#include "BraveSearchClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextDocument>

namespace {

// Brave's "description" field can contain <strong> highlight tags around
// matched terms — same reasoning/approach as the app's other search clients'
// own stripHtml() helpers.
QString stripHtml(const QString &fragment)
{
    QTextDocument doc;
    doc.setHtml(fragment);
    return doc.toPlainText().simplified();
}

} // namespace

BraveSearchClient::BraveSearchClient(const QString &apiKey, QObject *parent)
    : QObject(parent), m_apiKey(apiKey)
{
}

void BraveSearchClient::search(const QString &query)
{
    QUrl url("https://api.search.brave.com/res/v1/web/search");
    QUrlQuery q;
    q.addQueryItem("q", query);
    q.addQueryItem("count", "5");
    url.setQuery(q);

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Subscription-Token", m_apiKey.toUtf8());

    QNetworkReply *reply = m_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, query]() {
        reply->deleteLater();

        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode == 401 || statusCode == 403) {
            emit searchCompleted(query, QStringLiteral(
                "Error: Brave Search API key is invalid or missing (check Settings → Inputs)."));
            return;
        }
        if (statusCode == 429) {
            emit searchCompleted(query, QStringLiteral(
                "Error: Brave Search rate limit reached — try again later."));
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            emit searchCompleted(query, QString());
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray results = doc.object().value("web").toObject().value("results").toArray();
        if (results.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        QStringList entries;
        int i = 0;
        for (const QJsonValue &v : results) {
            const QJsonObject obj = v.toObject();
            const QString title = obj.value("title").toString();
            const QString link = obj.value("url").toString();
            const QString description = stripHtml(obj.value("description").toString());
            if (title.isEmpty())
                continue;
            entries << QString("%1. %2 — %3 (%4)").arg(++i).arg(title, description, link);
        }

        if (entries.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        const QString resultsText = QString(
            "--- Web search results for: %1 ---\n"
            "%2\n"
            "--- End of search results (source: Brave Search) ---")
                .arg(query, entries.join("\n"));
        emit searchCompleted(query, resultsText);
    });
}
