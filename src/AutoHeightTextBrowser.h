#pragma once

#include <QTextBrowser>
#include <QFrame>
#include <QNetworkAccessManager>
#include <QSet>

class QMimeData;

// A read-only QTextBrowser that resizes itself to exactly fit its content,
// with no internal scrollbar and no height limit — used for chat message
// bubbles, which live inside the outer message-list QScrollArea, so that's
// the only thing that should ever need to scroll. Also the widget that
// gives assistant replies real Markdown rendering (bold, lists, code
// blocks, tables, links) via Qt's native QTextDocument::setMarkdown(),
// instead of showing raw "**bold**"-style source text.
class AutoHeightTextBrowser : public QTextBrowser
{
    Q_OBJECT

public:
    explicit AutoHeightTextBrowser(QWidget *parent = nullptr);

    // Renders `content` as Markdown, except any ```html fenced code
    // block(s) within it — those get spliced in and rendered as real HTML
    // (tables, styled divs, etc.) instead of a literal code listing, and any
    // raw <svg>...</svg> markup within such a block gets auto-wrapped into a
    // renderable <img> (see AutoHeightTextBrowser.cpp — Qt's rich-text HTML
    // parser silently drops inline <svg> tags otherwise, so this is what
    // lets a reply draw an actual vector chart). Text outside an html block
    // is still converted from Markdown first, so the two can mix in one
    // reply. Falls back to plain setMarkdown() when no ```html block is
    // present, so this is safe to always call instead of setMarkdown()
    // directly.
    //
    // Scope note: this rides on QTextBrowser's built-in HTML support, which
    // is a static HTML4/CSS2.1-ish subset — no JavaScript, no live
    // embeds/iframes, no interactive maps or canvases. ChatWidget now
    // extracts ```html blocks out *before* ever reaching this method for a
    // finalized reply, routing each one to a real Chromium view instead
    // (see HtmlEmbedWidget) — so in practice this splicing path only still
    // runs for a message still actively streaming in, where the live
    // preview stays static until the reply finishes and gets re-rendered
    // through HtmlEmbedWidget. MapEmbedWidget is the equivalent for ```map.
    void setMarkdownWithHtmlBlocks(const QString &content);

    // Renders `content` literally — no Markdown interpretation — while still
    // substituting recognized emoji characters for the bundled Noto Emoji
    // images, the same as setMarkdownWithHtmlBlocks() does. Used for the
    // user's own message bubble (and error text), which must show exactly
    // what they typed but should still get colored emoji instead of tofu
    // boxes. A plain setPlainText() can't embed images at all, so this
    // HTML-escapes the text and renders it via setHtml() instead, wrapped in
    // a pre-wrap block so whitespace/line breaks still look like plain text.
    void setPlainTextWithEmoji(const QString &content);

    // True if `content` contains at least one ```html fenced block — used
    // by ChatWidget to decide whether a reply needs the raw/rendered toggle
    // button at all (plain Markdown replies never get one).
    static bool containsHtmlBlock(const QString &content);

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Qt only resolves local/embedded resources (data URIs, qrc, disk
    // files) by default — an <img src="https://..."> would otherwise just
    // show as a broken image. This fetches http(s) image URLs
    // asynchronously and caches them into the document once downloaded,
    // returning nothing for one that's still in flight (adjustHeight()
    // reruns automatically once the fetch completes and the resource is
    // cached, via markContentsDirty()).
    QVariant loadResource(int type, const QUrl &name) override;

    // Copying an emoji <img> alone would otherwise paste as the Unicode
    // object replacement character (￼) — it has no text behind it, just an
    // image. This restores the original emoji character(s) in the plain-text
    // clipboard flavor (the rendered/HTML flavor is untouched, so pasting
    // into a rich-text target still shows the image) — see
    // EmojiRenderer::emojiForResourcePath().
    QMimeData *createMimeDataFromSelection() const override;

private slots:
    void adjustHeight();

private:
    QNetworkAccessManager m_networkManager;
    QSet<QUrl> m_pendingImageFetches;
};
