#include "HtmlEmbedWidget.h"

#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QVBoxLayout>
#include <QTimer>
#include <QPointer>

namespace {
// One off-the-record profile shared by every HtmlEmbedWidget instance in
// the app's lifetime, rather than one per widget (which would mean a whole
// separate Chromium network context per chat message) — still fully
// isolated from the default profile (no persistent storage at all, see the
// class comment), just not re-created every time. Constructed lazily on
// first use; leaked at exit like any other app-lifetime singleton, which is
// the normal pattern for a QWebEngineProfile that needs to outlive every
// view using it.
QWebEngineProfile *sharedOffTheRecordProfile()
{
    static QWebEngineProfile *profile = new QWebEngineProfile(); // no storage name given = off-the-record
    return profile;
}
}

HtmlEmbedWidget::HtmlEmbedWidget(const QString &html, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);

    auto *page = new QWebEnginePage(sharedOffTheRecordProfile(), this);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, false);

    m_webView = new QWebEngineView;
    m_webView->setPage(page);
    // Provisional height, corrected once the page (and, per the delayed
    // recheck in onLoadFinished(), whatever it draws asynchronously after
    // its own load event) actually reports its real content size.
    m_webView->setFixedHeight(320);

    connect(m_webView, &QWebEngineView::loadFinished, this, &HtmlEmbedWidget::onLoadFinished);

    layout->addWidget(m_webView);

    m_webView->setHtml(html);
}

void HtmlEmbedWidget::onLoadFinished(bool ok)
{
    if (!ok)
        return;
    updateHeightFromContent();
    // A second pass shortly after — covers content whose real size only
    // settles after the page's own load event (chart libraries drawing to
    // canvas, web fonts reflowing text, etc.), which the first check above
    // can otherwise catch mid-reflow.
    QTimer::singleShot(400, this, &HtmlEmbedWidget::updateHeightFromContent);
}

void HtmlEmbedWidget::updateHeightFromContent()
{
    if (!m_webView)
        return;

    QPointer<HtmlEmbedWidget> self(this);
    m_webView->page()->runJavaScript(
        QStringLiteral("Math.max(document.body.scrollHeight, document.documentElement.scrollHeight)"),
        [self](const QVariant &result) {
            // The widget (or its view) may have been destroyed by the time
            // this callback fires — e.g. the conversation was switched away
            // from mid-flight — so both need re-checking, not just captured
            // by value, before touching either.
            if (!self || !self->m_webView)
                return;
            bool ok = false;
            const int height = result.toInt(&ok);
            if (ok && height > 0) {
                // Capped so one runaway/misbehaving reply can't blow out the
                // whole chat pane's scroll height.
                self->m_webView->setFixedHeight(qBound(80, height + 4, 2000));
            }
        });
}
