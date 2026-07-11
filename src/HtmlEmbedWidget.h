#pragma once

#include <QWidget>

class QWebEngineView;

// Embeds a real Chromium view for a ```html fenced block in an assistant
// reply, for the cases AutoHeightTextBrowser's QTextBrowser-based rendering
// (the default path — see ChatWidget::renderAssistantContent()) genuinely
// can't handle: that's a static HTML4/CSS2.1-ish subset with no JavaScript
// engine and no <canvas>, so a reply that draws a live chart (Chart.js, D3,
// etc.) or does anything else script-driven renders as inert markup there.
// This is the "give it a real browser" escape hatch for exactly that case.
//
// The HTML comes from the model, not a trusted source, so this runs in a
// shared off-the-record QWebEngineProfile (in-memory only — no persistent
// cookies/cache/local storage, and nothing here is shared with the rest of
// the app), with local file:// access and JS-spawned popups disabled. It
// deliberately does NOT block outbound network requests: most real
// JS-driven HTML (chart libraries especially) loads its own library from a
// CDN via <script src=...>, and blocking that would defeat the entire
// point of offering this over the static renderer. That's an accepted
// tradeoff, not an oversight — rendered content can make outbound requests
// same as any web page would.
class HtmlEmbedWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HtmlEmbedWidget(const QString &html, QWidget *parent = nullptr);

private slots:
    void onLoadFinished(bool ok);
    // Queries the loaded document's actual content height via JS and
    // resizes the view to fit it — a web page has no Qt-native "size hint,"
    // so this is what makes the embed feel like part of the chat bubble
    // instead of a fixed-size iframe with dead space or clipped content.
    void updateHeightFromContent();

private:
    QWebEngineView *m_webView = nullptr;
};
