#include "MapEmbedWidget.h"

#include <QWebEngineView>
#include <QVBoxLayout>
#include <QUrl>
#include <QUrlQuery>

MapEmbedWidget::MapEmbedWidget(const QString &query, int zoom, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);

    m_webView = new QWebEngineView;
    // A fixed height (rather than AutoHeightTextBrowser's fit-to-content
    // approach) since a map has no natural "content height" — this matches
    // roughly what the reference screenshots show for an embedded map.
    m_webView->setFixedHeight(320);

    QUrl url(QStringLiteral("https://maps.google.com/maps"));
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("q"), query);
    urlQuery.addQueryItem(QStringLiteral("z"), QString::number(zoom));
    urlQuery.addQueryItem(QStringLiteral("output"), QStringLiteral("embed"));
    url.setQuery(urlQuery);
    m_webView->setUrl(url);

    layout->addWidget(m_webView);
}
