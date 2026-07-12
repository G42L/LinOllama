#include "HtmlEmbedWidget.h"

#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebChannel>
#include <QVBoxLayout>
#include <QTimer>
#include <QPointer>
#include <QFile>
#include <QScrollArea>
#include <QScrollBar>

namespace {
// Qt6::WebChannel registers this resource itself once linked — the same
// script a page would load via `qrc:///qtwebchannel/qwebchannel.js` if it
// had been navigated to a real qrc:// URL, read directly here instead since
// setHtml()'s content has no meaningful base URL of its own to resolve a
// <script src> against. Defines the `QWebChannel` JS class the bridge-setup
// script below (in HtmlEmbedWidget's constructor) then uses.
QString qWebChannelJsSource()
{
    QFile file(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

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

// The object actually registered with QWebChannel (as "qtBridge") is this
// minimal stand-in, not HtmlEmbedWidget itself — QWebChannel introspects
// and exposes *every* property of whatever object it's given, and
// HtmlEmbedWidget being a QWidget means dozens of inherited ones (geometry,
// palette, sizeHint, ...) that have no NOTIFY signal, none of which the page
// actually needs. Registering the real widget flooded the log with one
// warning per such property and needlessly exposed all of it to page JS;
// this exposes exactly one slot and nothing else. Defined here (not in the
// header) since nothing outside this file needs it — see the trailing
// #include "HtmlEmbedWidget.moc" this requires, being a QObject defined
// entirely within a .cpp.
class ScrollBridge : public QObject
{
    Q_OBJECT

public:
    explicit ScrollBridge(QObject *parent = nullptr) : QObject(parent) {}

signals:
    void scrollRequested(int deltaY);

public slots:
    void handleParentScrollRequest(int deltaY) { emit scrollRequested(deltaY); }
};

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

    // Bridges the page's own JS (as `qtBridge` — see the injected script
    // below) to handleParentScrollRequest() via a minimal ScrollBridge
    // rather than registering this widget directly — see that class's own
    // comment for why.
    auto *bridge = new ScrollBridge(this);
    connect(bridge, &ScrollBridge::scrollRequested, this, &HtmlEmbedWidget::handleParentScrollRequest);
    m_channel = new QWebChannel(page);
    m_channel->registerObject(QStringLiteral("qtBridge"), bridge);
    page->setWebChannel(m_channel);

    // Detects, from inside the page itself, exactly when a wheel scroll
    // would go nowhere (already at the top/bottom of whatever's actually
    // scrollable under the cursor — the page's own document, or a nested
    // `overflow: auto` container) and hands that one wheel event off to
    // handleParentScrollRequest() instead of letting Chromium just eat it.
    // This has to live in the page's own JS: only it can synchronously know
    // its own scroll state at the moment a wheel event arrives — by the
    // time a C++-side wheelEvent() override could ask via runJavaScript(),
    // Chromium has already consumed the event.
    QWebEngineScript bridgeScript;
    bridgeScript.setName(QStringLiteral("chatScrollChaining"));
    bridgeScript.setInjectionPoint(QWebEngineScript::DocumentCreation);
    bridgeScript.setWorldId(QWebEngineScript::MainWorld);
    bridgeScript.setRunsOnSubFrames(false);
    bridgeScript.setSourceCode(qWebChannelJsSource() + QStringLiteral(R"JS(
        (function() {
            document.addEventListener('DOMContentLoaded', function() {
                if (typeof qt === 'undefined' || !qt.webChannelTransport || typeof QWebChannel === 'undefined')
                    return;
                new QWebChannel(qt.webChannelTransport, function(channel) {
                    var bridge = channel.objects.qtBridge;

                    function isScrollable(el) {
                        if (!el || el === document.documentElement || el === document.body)
                            return false;
                        var overflowY = window.getComputedStyle(el).overflowY;
                        return (overflowY === 'auto' || overflowY === 'scroll') && el.scrollHeight > el.clientHeight;
                    }
                    function findScrollableAncestor(el) {
                        while (el && el !== document.body && el !== document.documentElement) {
                            if (isScrollable(el))
                                return el;
                            el = el.parentElement;
                        }
                        return null; // nothing but the document itself scrolls here
                    }

                    document.addEventListener('wheel', function(event) {
                        var target = findScrollableAncestor(event.target);
                        var atTop, atBottom;
                        if (target) {
                            atTop = target.scrollTop <= 0;
                            atBottom = (target.scrollTop + target.clientHeight) >= (target.scrollHeight - 1);
                        } else {
                            atTop = window.scrollY <= 0;
                            atBottom = (window.scrollY + window.innerHeight) >= (document.documentElement.scrollHeight - 1);
                        }
                        if ((event.deltaY < 0 && atTop) || (event.deltaY > 0 && atBottom)) {
                            event.preventDefault();
                            bridge.handleParentScrollRequest(Math.round(event.deltaY));
                        }
                    }, { passive: false });
                });
            });
        })();
    )JS"));
    page->scripts().insert(bridgeScript);

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

void HtmlEmbedWidget::handleParentScrollRequest(int deltaY)
{
    QScrollArea *scrollArea = nullptr;
    for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
        if (auto *candidate = qobject_cast<QScrollArea *>(w)) {
            scrollArea = candidate;
            break;
        }
    }
    if (!scrollArea)
        return;

    // JS wheel deltaY is already in roughly the same units a QScrollBar
    // step expects (pixels for the common "line/pixel" delta mode most
    // mice and trackpads report) — good enough for this to feel like a
    // continuation of the same scroll gesture without needing exact 1:1
    // fidelity with the browser's own internal scroll speed.
    if (QScrollBar *bar = scrollArea->verticalScrollBar())
        bar->setValue(bar->value() + deltaY);
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

// Required for ScrollBridge above — a QObject with Q_OBJECT defined
// entirely within this .cpp (not declared in the header), which needs its
// own moc-generated code included explicitly since AUTOMOC only scans
// header files for that by default.
#include "HtmlEmbedWidget.moc"
