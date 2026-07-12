#include "StackOverflowSearchClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextDocument>

namespace {

// Excerpts come back with <b> highlighting around matched terms and HTML
// entities — same reasoning/approach as WebSearchClient's own stripHtml().
QString stripHtml(const QString &fragment)
{
    QTextDocument doc;
    doc.setHtml(fragment);
    return doc.toPlainText().simplified();
}

} // namespace

StackOverflowSearchClient::StackOverflowSearchClient(QObject *parent) : QObject(parent) {}

void StackOverflowSearchClient::search(const QString &query)
{
    // /search/excerpts is the API's own "give me a snippet per match"
    // endpoint — a plain /search only returns question metadata (title,
    // score, answer count), no body text to actually summarize.
    QUrl url("https://api.stackexchange.com/2.3/search/excerpts");
    QUrlQuery q;
    q.addQueryItem("order", "desc");
    q.addQueryItem("sort", "relevance");
    q.addQueryItem("site", "stackoverflow");
    q.addQueryItem("pagesize", "5");
    q.addQueryItem("q", query);
    url.setQuery(q);

    QNetworkRequest request(url);
    // The Stack Exchange API always gzip-compresses its responses
    // regardless of what (if anything) the client asks for — Qt's own
    // QNetworkAccessManager decompresses a gzip Content-Encoding
    // transparently, so reply->readAll() below already gets plain JSON,
    // no manual decompression needed here.
    QNetworkReply *reply = m_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, query]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit searchCompleted(query, QString());
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray items = doc.object().value("items").toArray();
        if (items.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        QStringList entries;
        int i = 0;
        for (const QJsonValue &v : items) {
            const QJsonObject obj = v.toObject();
            // The API HTML-entity-encodes both fields (e.g. "What&#39;s" for
            // "What's") — title has no markup to strip, but still needs the
            // same decoding pass as excerpt.
            const QString title = stripHtml(obj.value("title").toString());
            const QString excerpt = stripHtml(obj.value("excerpt").toString());
            if (title.isEmpty() || excerpt.isEmpty())
                continue;

            const bool isAnswer = obj.value("item_type").toString() == "answer";
            const QString link = isAnswer
                ? QString("https://stackoverflow.com/a/%1").arg(obj.value("answer_id").toVariant().toString())
                : QString("https://stackoverflow.com/q/%1").arg(obj.value("question_id").toVariant().toString());
            const QString kind = isAnswer ? "Answer to" : "Question";

            entries << QString("%1. [%2] %3 — %4 (%5)").arg(++i).arg(kind, title, excerpt, link);
        }

        if (entries.isEmpty()) {
            emit searchCompleted(query, QString());
            return;
        }

        const QString resultsText = QString(
            "--- Stack Overflow search results for: %1 ---\n"
            "%2\n"
            "--- End of search results (source: Stack Overflow, via the public Stack Exchange API) ---")
                .arg(query, entries.join("\n"));
        emit searchCompleted(query, resultsText);
    });
}
