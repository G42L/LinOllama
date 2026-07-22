#pragma once

#include <QTextBrowser>
#include <QTextDocument>
#include <QFrame>
#include <QNetworkAccessManager>
#include <QSet>
#include <QVector>
#include <QString>

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

    // Renders `content` as Markdown. Any ```html fenced code block within it
    // gets spliced in and rendered as real HTML (tables, styled divs, etc.)
    // instead of a literal code listing, and any raw <svg>...</svg> markup
    // within such a block gets auto-wrapped into a renderable <img> (see
    // AutoHeightTextBrowser.cpp — Qt's rich-text HTML parser silently drops
    // inline <svg> tags otherwise, so this is what lets a reply draw an
    // actual vector chart). Any *other* fenced code block (```python,
    // ```bash, ```javascript, etc. — see CodeHighlighter for the full list)
    // gets syntax-highlighted instead of rendered as Qt's own plain,
    // uncolored, one-<pre>-per-line default. `dark` picks which of Theme's
    // two syntax color sets to use — pass ThemeManager::isDarkActive().
    // Text outside any fenced block is still converted from Markdown first,
    // so all of these can mix in one reply. Safe to always call instead of
    // setMarkdown() directly.
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
    //
    // `renderHtmlBlocksLive` controls how a ```html fence is treated when it
    // *does* reach here (default true, matching the streaming-preview
    // behavior above): live-injected raw markup when true, or syntax-
    // highlighted as HTML source (same as any other language) when false —
    // used by ChatWidget's raw/source-view toggle, so "View source" shows a
    // properly formatted (highlighted, emoji-substituted) code listing for
    // the ```html block instead of a second live Chromium rendering (which
    // has its own DOM, not ours, so it never gets our highlighting/emoji
    // treatment anyway — that gap is exactly what the source view is for).
    void setMarkdownWithHtmlBlocks(const QString &content, bool dark, bool renderHtmlBlocksLive = true);

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

    // A no-op override — deliberately does *not* call the base
    // implementation. QTextBrowser treats every activated link (other than
    // ones setOpenExternalLinks(true) redirects automatically) as internal
    // navigation: it calls this to actually load and *replace* the
    // document's content with whatever the link points to. With
    // setOpenExternalLinks(false) (see the constructor), that now includes
    // this widget's own "copycode:<index>" Copy links — clicking one was
    // observed to print "QTextBrowser: No document for copycode:N" and wipe
    // the entire message bubble, since there's obviously no real document at
    // that "address" to navigate to. All link handling here goes through
    // onAnchorClicked() instead (which still fires normally — anchorClicked()
    // is emitted independently of this method, earlier in the same click),
    // so this widget never wants QTextBrowser's own navigation at all.
    void doSetSource(const QUrl &name, QTextDocument::ResourceType type) override;

private slots:
    void adjustHeight();
    // Handles both link kinds this widget ever shows: a real http(s) link
    // (opened via QDesktopServices, replicating what setOpenExternalLinks
    // (true) used to do automatically) and a "copycode:<index>" anchor next
    // to a syntax-highlighted code block, which instead copies that block's
    // original (un-highlighted) source text to the clipboard — see
    // m_codeBlockTexts. openExternalLinks has to be off for this to see
    // http(s) clicks at all: Qt suppresses anchorClicked() entirely for
    // external links when that's on, handling them itself instead.
    void onAnchorClicked(const QUrl &url);

private:
    QNetworkAccessManager m_networkManager;
    QSet<QUrl> m_pendingImageFetches;

    // One entry per syntax-highlighted code block in the most recent
    // setMarkdownWithHtmlBlocks() call, in source order — index N's "Copy"
    // link uses a "copycode:N" href, resolved back to this original text
    // (not the highlighted HTML) in onAnchorClicked(). Rebuilt from scratch
    // on every call, since content re-renders wholesale rather than
    // patching in place (streaming tokens included).
    QVector<QString> m_codeBlockTexts;
};
