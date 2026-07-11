#pragma once

#include <QWidget>

class QWebEngineView;

// Embeds a real, interactive Google Maps view (pan/zoom, live tiles) in a
// chat bubble — for a ```map fenced block in an assistant reply (see
// ChatWidget::renderAssistantContent()). This is the one piece of chat
// content that genuinely needs a live web view: AutoHeightTextBrowser's
// QTextBrowser-based HTML rendering has no JavaScript, so it can show a
// static map image at best, never a pannable one.
//
// Scope/caveat: uses Google's classic keyless embed URL pattern
// (maps.google.com/maps?q=...&output=embed) — the same "Share > Embed a
// map" flow Google's own site offers with no API key. It's not Google's
// officially documented Maps Embed API (which requires a key and supports
// multiple markers/richer styling); this is simpler but undocumented, so it
// could change or stop working without notice. It also only supports a
// single query/marker — not the multi-pin itinerary maps some reference UIs
// show.
class MapEmbedWidget : public QWidget
{
    Q_OBJECT

public:
    // query is a place name/address Google Maps can resolve on its own
    // (e.g. "Tokyo Tower, Tokyo, Japan") — there's no local geocoding here,
    // Maps' own search does the resolving.
    explicit MapEmbedWidget(const QString &query, int zoom = 12, QWidget *parent = nullptr);

private:
    QWebEngineView *m_webView = nullptr;
};
