#include "AutoHeightTextBrowser.h"
#include "EmojiRenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextBlockFormat>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QVector>
#include <QPair>
#include <QHash>
#include <QMimeData>
#include <QStringList>
#include <QFontInfo>
#include <QSettings>

namespace {

// Qt's rich-text HTML parser silently drops raw <svg>...</svg> markup
// (verified: it's neither rendered nor left behind as visible text — just
// discarded). Wrapping it as a base64 data-URI <img> instead makes it
// render correctly, since Qt rasterizes data-URI images via QImage — which,
// with the Qt Svg module linked, understands SVG. This is what lets a reply
// draw an actual vector chart (bars, curves, gradients) rather than only
// static HTML/CSS shapes.
QString wrapRawSvgAsImages(const QString &html)
{
    static const QRegularExpression svgTag(
        QStringLiteral("<svg[^>]*>.*?</svg>"),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);

    QList<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = svgTag.globalMatch(html);
    while (it.hasNext())
        matches.append(it.next());
    if (matches.isEmpty())
        return html;

    QString result = html;
    // Replace back-to-front so earlier matches' offsets stay valid as the
    // string is mutated.
    for (auto matchIt = matches.crbegin(); matchIt != matches.crend(); ++matchIt) {
        const QString svgMarkup = matchIt->captured(0);
        const QString dataUri = QStringLiteral("data:image/svg+xml;base64,")
            + QString::fromLatin1(svgMarkup.toUtf8().toBase64());
        result.replace(matchIt->capturedStart(), matchIt->capturedLength(),
                        QStringLiteral("<img src=\"%1\">").arg(dataUri));
    }
    return result;
}

// Locates ```html fenced code block(s) in `content` and builds one combined
// HTML document: text outside a block is converted from Markdown via Qt's
// own converter (so formatting stays consistent with the rest of the app),
// text inside a block is spliced in verbatim as real HTML (after
// wrapRawSvgAsImages()). Returns false (leaving htmlOut untouched) when
// there's no such block, so the caller can fall back to plain Markdown
// rendering.
// Shared with AutoHeightTextBrowser::containsHtmlBlock() below — keep the
// two in sync if this pattern ever changes.
const QRegularExpression &htmlFencePattern()
{
    static const QRegularExpression htmlFence(
        QStringLiteral("```html\\s*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    return htmlFence;
}

// `font` is the destination widget's own (already-polished) font — each
// throwaway QTextDocument used here to convert a Markdown segment to HTML
// starts out with Qt's generic default font, not the browser's actual
// themed one, and toHtml() bakes whatever font it has directly into the
// exported HTML as inline styles. Without setting it explicitly, the
// spliced-in segment would render at the wrong (generic) size once handed
// back to the real browser's setHtml(), overriding its QSS font-size.
bool extractHtmlBlocks(const QString &content, QString *htmlOut, const QFont &font)
{
    const QRegularExpression &htmlFence = htmlFencePattern();

    if (!content.contains(htmlFence))
        return false;

    QString result;
    int lastEnd = 0;
    QRegularExpressionMatchIterator it = htmlFence.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();

        const QString before = content.mid(lastEnd, match.capturedStart() - lastEnd);
        if (!before.trimmed().isEmpty()) {
            QTextDocument doc;
            doc.setDefaultFont(font);
            doc.setMarkdown(before);
            result += doc.toHtml();
        }

        result += wrapRawSvgAsImages(match.captured(1));
        lastEnd = match.capturedEnd();
    }

    const QString after = content.mid(lastEnd);
    if (!after.trimmed().isEmpty()) {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.setMarkdown(after);
        result += doc.toHtml();
    }

    *htmlOut = result;
    return true;
}

// LLMs (this Ollama models especially) frequently emit inline LaTeX math
// (`$\rightarrow$`, `\(\alpha\)`, bare `\times` etc.) even when not asked to.
// Qt's QTextDocument::setMarkdown() has no concept of LaTeX, so those
// sequences would otherwise show up completely literally in the chat
// bubble. This strips math delimiters and swaps the common LaTeX macros for
// their Unicode equivalent so the reply reads as plain text/Markdown.
QString sanitizeMathSegment(QString segment)
{
    // Math delimiters: keep the contents, drop the wrapper.
    static const QRegularExpression blockDollar(
        QStringLiteral("\\$\\$(.*?)\\$\\$"), QRegularExpression::DotMatchesEverythingOption);
    segment.replace(blockDollar, QStringLiteral("\\1"));
    static const QRegularExpression blockBracket(
        QStringLiteral("\\\\\\[(.*?)\\\\\\]"), QRegularExpression::DotMatchesEverythingOption);
    segment.replace(blockBracket, QStringLiteral("\\1"));
    static const QRegularExpression inlineParen(
        QStringLiteral("\\\\\\((.*?)\\\\\\)"), QRegularExpression::DotMatchesEverythingOption);
    segment.replace(inlineParen, QStringLiteral("\\1"));
    static const QRegularExpression inlineDollar(QStringLiteral("\\$([^$\\n]+?)\\$"));
    segment.replace(inlineDollar, QStringLiteral("\\1"));
    
    // Text-wrapping macros: \text{m}, \mathrm{kg}, etc. take a brace
    // argument rather than standing alone, so they don't fit the bare
    // macro table below — unwrap to just the argument (e.g. "m").
    // Doesn't handle nested braces in the argument, which real usage here
    // (units, short labels) never has.
    static const QVector<QString> textWrappers = {
        QStringLiteral("text"), QStringLiteral("mathrm"), QStringLiteral("mathit"),
        QStringLiteral("mathbf"), QStringLiteral("textbf"), QStringLiteral("textit"),
        QStringLiteral("textrm"), QStringLiteral("operatorname"),
        QStringLiteral("mathsf"), QStringLiteral("mathtt"),
    };
    for (const QString &name : textWrappers) {
        static QHash<QString, QRegularExpression> cache;
        auto cacheIt = cache.find(name);
        if (cacheIt == cache.end())
            cacheIt = cache.insert(name, QRegularExpression(
                QStringLiteral("\\\\%1\\{([^{}]*)\\}").arg(name)));
        segment.replace(cacheIt.value(), QStringLiteral("\\1"));
    }

    // LaTeX macro -> Unicode. Matched with a "not followed by a letter"
    // lookahead so e.g. "\le" doesn't fire inside "\leq".
    static const QVector<QPair<QString, QString>> macros = {
        {QStringLiteral("rightarrow"), QString::fromUtf8("\xe2\x86\x92")},   // →
        {QStringLiteral("Rightarrow"), QString::fromUtf8("\xe2\x87\x92")},   // ⇒
        {QStringLiteral("longrightarrow"), QString::fromUtf8("\xe2\x86\x92")},
        {QStringLiteral("leftarrow"), QString::fromUtf8("\xe2\x86\x90")},    // ←
        {QStringLiteral("Leftarrow"), QString::fromUtf8("\xe2\x87\x90")},    // ⇐
        {QStringLiteral("longleftarrow"), QString::fromUtf8("\xe2\x86\x90")},
        {QStringLiteral("leftrightarrow"), QString::fromUtf8("\xe2\x86\x94")}, // ↔
        {QStringLiteral("Leftrightarrow"), QString::fromUtf8("\xe2\x87\x94")}, // ⇔
        {QStringLiteral("mapsto"), QString::fromUtf8("\xe2\x86\xa6")},       // ↦
        {QStringLiteral("to"), QString::fromUtf8("\xe2\x86\x92")},          // →
        {QStringLiteral("times"), QString::fromUtf8("\xc3\x97")},          // ×
        {QStringLiteral("div"), QString::fromUtf8("\xc3\xb7")},            // ÷
        {QStringLiteral("cdot"), QString::fromUtf8("\xc2\xb7")},           // ·
        {QStringLiteral("pm"), QString::fromUtf8("\xc2\xb1")},             // ±
        {QStringLiteral("mp"), QString::fromUtf8("\xe2\x88\x93")},         // ∓
        {QStringLiteral("infty"), QString::fromUtf8("\xe2\x88\x9e")},      // ∞
        {QStringLiteral("approx"), QString::fromUtf8("\xe2\x89\x88")},     // ≈
        {QStringLiteral("simeq"), QString::fromUtf8("\xe2\x89\x83")},      // ≃
        {QStringLiteral("equiv"), QString::fromUtf8("\xe2\x89\xa1")},      // ≡
        {QStringLiteral("neq"), QString::fromUtf8("\xe2\x89\xa0")},        // ≠
        {QStringLiteral("leq"), QString::fromUtf8("\xe2\x89\xa4")},        // ≤
        {QStringLiteral("le"), QString::fromUtf8("\xe2\x89\xa4")},         // ≤
        {QStringLiteral("geq"), QString::fromUtf8("\xe2\x89\xa5")},        // ≥
        {QStringLiteral("ge"), QString::fromUtf8("\xe2\x89\xa5")},         // ≥
        {QStringLiteral("ll"), QString::fromUtf8("\xe2\x89\xaa")},         // ≪
        {QStringLiteral("gg"), QString::fromUtf8("\xe2\x89\xab")},         // ≫
        {QStringLiteral("propto"), QString::fromUtf8("\xe2\x88\x9d")},     // ∝
        {QStringLiteral("sum"), QString::fromUtf8("\xe2\x88\x91")},        // ∑
        {QStringLiteral("prod"), QString::fromUtf8("\xe2\x88\x8f")},       // ∏
        {QStringLiteral("int"), QString::fromUtf8("\xe2\x88\xab")},        // ∫
        {QStringLiteral("oint"), QString::fromUtf8("\xe2\x88\xae")},       // ∮
        {QStringLiteral("partial"), QString::fromUtf8("\xe2\x88\x82")},    // ∂
        {QStringLiteral("nabla"), QString::fromUtf8("\xe2\x88\x87")},      // ∇
        {QStringLiteral("forall"), QString::fromUtf8("\xe2\x88\x80")},     // ∀
        {QStringLiteral("exists"), QString::fromUtf8("\xe2\x88\x83")},     // ∃
        {QStringLiteral("nexists"), QString::fromUtf8("\xe2\x88\x84")},    // ∄
        {QStringLiteral("in"), QString::fromUtf8("\xe2\x88\x88")},         // ∈
        {QStringLiteral("notin"), QString::fromUtf8("\xe2\x88\x89")},      // ∉
        {QStringLiteral("subseteq"), QString::fromUtf8("\xe2\x8a\x86")},   // ⊆
        {QStringLiteral("subset"), QString::fromUtf8("\xe2\x8a\x82")},     // ⊂
        {QStringLiteral("supseteq"), QString::fromUtf8("\xe2\x8a\x87")},   // ⊇
        {QStringLiteral("supset"), QString::fromUtf8("\xe2\x8a\x83")},     // ⊃
        {QStringLiteral("cup"), QString::fromUtf8("\xe2\x88\xaa")},        // ∪
        {QStringLiteral("cap"), QString::fromUtf8("\xe2\x88\xa9")},        // ∩
        {QStringLiteral("setminus"), QString::fromUtf8("\xe2\x88\x96")},   // ∖
        {QStringLiteral("emptyset"), QString::fromUtf8("\xe2\x88\x85")},   // ∅
        {QStringLiteral("varnothing"), QString::fromUtf8("\xe2\x88\x85")}, // ∅
        {QStringLiteral("wedge"), QString::fromUtf8("\xe2\x88\xa7")},      // ∧
        {QStringLiteral("vee"), QString::fromUtf8("\xe2\x88\xa8")},        // ∨
        {QStringLiteral("neg"), QString::fromUtf8("\xc2\xac")},            // ¬
        {QStringLiteral("oplus"), QString::fromUtf8("\xe2\x8a\x95")},      // ⊕
        {QStringLiteral("otimes"), QString::fromUtf8("\xe2\x8a\x97")},     // ⊗
        {QStringLiteral("perp"), QString::fromUtf8("\xe2\x8a\xa5")},       // ⊥
        {QStringLiteral("parallel"), QString::fromUtf8("\xe2\x88\xa5")},   // ∥
        {QStringLiteral("angle"), QString::fromUtf8("\xe2\x88\xa0")},      // ∠
        {QStringLiteral("therefore"), QString::fromUtf8("\xe2\x88\xb4")},  // ∴
        {QStringLiteral("because"), QString::fromUtf8("\xe2\x88\xb5")},    // ∵
        {QStringLiteral("sim"), QString::fromUtf8("\xe2\x88\xbc")},        // ∼
        {QStringLiteral("ldots"), QString::fromUtf8("\xe2\x80\xa6")},      // …
        {QStringLiteral("cdots"), QString::fromUtf8("\xe2\x8b\xaf")},      // ⋯
        {QStringLiteral("vdots"), QString::fromUtf8("\xe2\x8b\xae")},      // ⋮
        {QStringLiteral("ddots"), QString::fromUtf8("\xe2\x8b\xb1")},      // ⋱
        {QStringLiteral("hbar"), QString::fromUtf8("\xe2\x84\x8f")},       // ℏ
        {QStringLiteral("ell"), QString::fromUtf8("\xe2\x84\x93")},        // ℓ
        {QStringLiteral("prime"), QString::fromUtf8("\xe2\x80\xb2")},      // ′
        {QStringLiteral("deg"), QString::fromUtf8("\xc2\xb0")},            // °
        {QStringLiteral("circ"), QString::fromUtf8("\xc2\xb0")},           // °
        {QStringLiteral("bullet"), QString::fromUtf8("\xe2\x80\xa2")},     // •
        // Greek letters (lowercase)
        {QStringLiteral("alpha"), QString::fromUtf8("\xce\xb1")},
        {QStringLiteral("beta"), QString::fromUtf8("\xce\xb2")},
        {QStringLiteral("gamma"), QString::fromUtf8("\xce\xb3")},
        {QStringLiteral("delta"), QString::fromUtf8("\xce\xb4")},
        {QStringLiteral("epsilon"), QString::fromUtf8("\xce\xb5")},
        {QStringLiteral("varepsilon"), QString::fromUtf8("\xce\xb5")},
        {QStringLiteral("zeta"), QString::fromUtf8("\xce\xb6")},
        {QStringLiteral("eta"), QString::fromUtf8("\xce\xb7")},
        {QStringLiteral("theta"), QString::fromUtf8("\xce\xb8")},
        {QStringLiteral("iota"), QString::fromUtf8("\xce\xb9")},
        {QStringLiteral("kappa"), QString::fromUtf8("\xce\xba")},
        {QStringLiteral("lambda"), QString::fromUtf8("\xce\xbb")},
        {QStringLiteral("mu"), QString::fromUtf8("\xce\xbc")},
        {QStringLiteral("nu"), QString::fromUtf8("\xce\xbd")},
        {QStringLiteral("xi"), QString::fromUtf8("\xce\xbe")},
        {QStringLiteral("pi"), QString::fromUtf8("\xcf\x80")},
        {QStringLiteral("rho"), QString::fromUtf8("\xcf\x81")},
        {QStringLiteral("sigma"), QString::fromUtf8("\xcf\x83")},
        {QStringLiteral("tau"), QString::fromUtf8("\xcf\x84")},
        {QStringLiteral("upsilon"), QString::fromUtf8("\xcf\x85")},
        {QStringLiteral("phi"), QString::fromUtf8("\xcf\x86")},
        {QStringLiteral("varphi"), QString::fromUtf8("\xcf\x86")},
        {QStringLiteral("chi"), QString::fromUtf8("\xcf\x87")},
        {QStringLiteral("psi"), QString::fromUtf8("\xcf\x88")},
        {QStringLiteral("omega"), QString::fromUtf8("\xcf\x89")},
        // Greek letters (uppercase)
        {QStringLiteral("Gamma"), QString::fromUtf8("\xce\x93")},
        {QStringLiteral("Delta"), QString::fromUtf8("\xce\x94")},
        {QStringLiteral("Theta"), QString::fromUtf8("\xce\x98")},
        {QStringLiteral("Lambda"), QString::fromUtf8("\xce\x9b")},
        {QStringLiteral("Xi"), QString::fromUtf8("\xce\x9e")},
        {QStringLiteral("Pi"), QString::fromUtf8("\xce\xa0")},
        {QStringLiteral("Sigma"), QString::fromUtf8("\xce\xa3")},
        {QStringLiteral("Upsilon"), QString::fromUtf8("\xce\xa5")},
        {QStringLiteral("Phi"), QString::fromUtf8("\xce\xa6")},
        {QStringLiteral("Psi"), QString::fromUtf8("\xce\xa8")},
        {QStringLiteral("Omega"), QString::fromUtf8("\xce\xa9")},
    };

    for (const auto &macro : macros) {
        static QHash<QString, QRegularExpression> cache;
        auto cacheIt = cache.find(macro.first);
        if (cacheIt == cache.end())
            cacheIt = cache.insert(macro.first, QRegularExpression(
                QStringLiteral("\\\\%1(?![A-Za-z])").arg(macro.first)));
        segment.replace(cacheIt.value(), macro.second);
    }

    return segment;
}

// Applies sanitizeMathSegment() everywhere except inside fenced/inline code,
// so a reply that's actually showing LaTeX *source* as an example isn't
// altered.
QString sanitizeLatexMath(const QString &content)
{
    static const QRegularExpression codeSpan(
        QStringLiteral("```.*?```|`[^`\\n]*`"),
        QRegularExpression::DotMatchesEverythingOption);

    QString result;
    int lastEnd = 0;
    QRegularExpressionMatchIterator it = codeSpan.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += sanitizeMathSegment(content.mid(lastEnd, match.capturedStart() - lastEnd));
        result += match.captured(0);
        lastEnd = match.capturedEnd();
    }
    result += sanitizeMathSegment(content.mid(lastEnd));
    return result;
}

// Substitutes emoji in already-converted HTML (as produced by
// QTextDocument::toHtml() or a spliced ```html block), never in Markdown
// *source* text — QTextDocument::setMarkdown() silently drops raw inline
// HTML it doesn't recognize as a known Markdown construct (verified: an
// injected <img> tag, and everything after it on that line, simply
// vanishes from the parsed result), so an <img> substituted in before
// setMarkdown() runs can never survive. Operating on the HTML output
// instead sidesteps that entirely. Skips <pre>/<code> blocks and the
// insides of tags themselves (attributes), so a reply showing emoji as
// literal source text isn't touched and attribute values can't be mangled.
// See EmojiRenderer.h for why substitution is needed at all instead of
// just relying on Qt's own emoji-font rendering.
QString substituteEmojiInHtml(const QString &html, int pixelSize)
{
    static const QRegularExpression skip(
        QStringLiteral("<pre[^>]*>.*?</pre>|<code[^>]*>.*?</code>|<[^>]*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    QString result;
    int lastEnd = 0;
    QRegularExpressionMatchIterator it = skip.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += EmojiRenderer::substituteWithImages(html.mid(lastEnd, match.capturedStart() - lastEnd), pixelSize);
        result += match.captured(0); // tag, or pre/code block, passed through untouched
        lastEnd = match.capturedEnd();
    }
    result += EmojiRenderer::substituteWithImages(html.mid(lastEnd), pixelSize);
    return result;
}

// Qt's rich-text CSS doesn't resolve "em"/"ex" units for <img width/height>
// (verified: silently resolves to 0), so there's no way to size an emoji
// image relative to the surrounding text purely in CSS — the pixel size has
// to be computed from the widget's actual font instead. ensurePolished()
// first since a freshly-constructed widget's font() can still be the
// pre-stylesheet default if nothing has forced QSS application yet.
//
// QFontInfo::pixelSize() — the font's nominal size, i.e. the "1em" a
// browser would use to size inline emoji — not QFontMetrics::height(),
// which is ascent+descent+leading and comes out noticeably taller than the
// actual text, making the emoji look oversized next to it.
int emojiPixelSize(QWidget *widget)
{
    widget->ensurePolished();
    return QFontInfo(widget->font()).pixelSize();
}

// QTextDocument::setMarkdown() collapses paragraph/list-item spacing to 0 —
// verified directly (both calling setMarkdown() straight on the browser and
// going through the toHtml()/setHtml() round trip above produce identical
// zero block margins), so this isn't a side effect of that round trip, it's
// just how dense Qt's own Markdown-to-block-format conversion is. Setting
// margins directly on each QTextBlockFormat after the fact is more reliable
// than patching Qt's generated HTML string, since it doesn't depend on the
// exact inline style syntax Qt happens to emit.
//
// Values come from Settings > Formatting (see SettingsDialog) rather than
// being hardcoded, defaulting to the same 8px/4px chosen when this was
// first added. The first block never gets a top margin even if it's a
// heading — there's nothing above it to separate from inside the bubble,
// and the bubble's own layout margin already provides that space.
void applyBlockSpacing(QTextDocument *doc)
{
    const QSettings settings;
    const int paragraphSpacing = settings.value("formatting/paragraphSpacing", 8).toInt();
    const int listItemSpacing = settings.value("formatting/listItemSpacing", 4).toInt();
    const int headingSpacingBefore = settings.value("formatting/headingSpacingBefore", 18).toInt();

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat fmt = block.blockFormat();
        const bool inList = block.textList() != nullptr;
        const bool isHeading = fmt.headingLevel() > 0;
        const bool isFirstBlock = (block == doc->begin());
        fmt.setTopMargin(isHeading && !isFirstBlock ? headingSpacingBefore : 0);
        fmt.setBottomMargin(inList ? listItemSpacing : paragraphSpacing);
        QTextCursor(block).setBlockFormat(fmt);
    }
}

} // namespace

AutoHeightTextBrowser::AutoHeightTextBrowser(QWidget *parent)
    : QTextBrowser(parent)
{
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setOpenExternalLinks(true); // links in markdown output open in the system browser, not "navigate" inside this widget
    setReadOnly(true);
    setTextInteractionFlags(Qt::TextBrowserInteraction | Qt::TextSelectableByMouse);

    // Keep the widget's own background out of the way — the QFrame bubble
    // around it (see ChatWidget::appendMessageBubble) supplies the color;
    // without this, QTextBrowser's own opaque viewport would paint over it.
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);

    // documentSizeChanged fires once layout has actually been recomputed —
    // more reliable than reacting to textChanged directly, which can fire
    // a frame before the new size is available and cause the height to lag
    // one keystroke/token behind.
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, &AutoHeightTextBrowser::adjustHeight);
}

void AutoHeightTextBrowser::setMarkdownWithHtmlBlocks(const QString &content)
{
    ensurePolished(); // font() below must reflect the actual QSS-applied font, not the pre-stylesheet default
    const QFont ownFont = font();
    const QString sanitized = sanitizeLatexMath(content);

    QString html;
    if (!extractHtmlBlocks(sanitized, &html, ownFont)) {
        QTextDocument doc;
        doc.setDefaultFont(ownFont); // see extractHtmlBlocks() for why this matters
        doc.setMarkdown(sanitized);
        html = doc.toHtml();
    }

    // Emoji substitution happens here, on the final HTML, and only here —
    // see substituteEmojiInHtml() for why doing it on the Markdown source
    // before setMarkdown() runs can't work.
    setHtml(substituteEmojiInHtml(html, QFontInfo(ownFont).pixelSize()));
    applyBlockSpacing(document());
    // adjustHeight() is wired to documentSizeChanged for content that
    // arrives later (streaming tokens, an async image load), but calling it
    // explicitly here too removes any dependency on that signal's exact
    // firing order relative to applyBlockSpacing()'s own edits — this is
    // the only path guaranteed to run after the document's *final* state
    // (content + emoji images + spacing) is fully settled.
    adjustHeight();
}

void AutoHeightTextBrowser::setPlainTextWithEmoji(const QString &content)
{
    setHtml(QStringLiteral("<div style=\"white-space:pre-wrap;\">%1</div>")
                .arg(EmojiRenderer::escapedPlainTextWithImages(content, emojiPixelSize(this))));
    adjustHeight();
}

bool AutoHeightTextBrowser::containsHtmlBlock(const QString &content)
{
    return content.contains(htmlFencePattern());
}

QVariant AutoHeightTextBrowser::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource
        && (name.scheme() == QLatin1String("http") || name.scheme() == QLatin1String("https"))) {
        // QTextBrowser checks its document's own resource cache before ever
        // calling loadResource() — reaching here at all means this URL
        // isn't cached yet, so only the "already in flight" case needs
        // guarding against (layout can ask for the same resource multiple
        // times while a fetch is pending).
        if (!m_pendingImageFetches.contains(name)) {
            m_pendingImageFetches.insert(name);
            QNetworkReply *reply = m_networkManager.get(QNetworkRequest(name));
            connect(reply, &QNetworkReply::finished, this, [this, reply, name]() {
                reply->deleteLater();
                m_pendingImageFetches.remove(name);

                if (reply->error() != QNetworkReply::NoError)
                    return;

                QImage image;
                if (!image.loadFromData(reply->readAll()))
                    return;

                document()->addResource(QTextDocument::ImageResource, name, image);
                // Forces Qt to redo layout now that the resource exists —
                // adjustHeight() is already wired to documentSizeChanged, so
                // the bubble grows to fit once the image is actually in.
                document()->markContentsDirty(0, document()->characterCount());
            });
        }
        return QVariant(); // nothing yet; picked up automatically once the fetch above completes
    }

    return QTextBrowser::loadResource(type, name);
}

QMimeData *AutoHeightTextBrowser::createMimeDataFromSelection() const
{
    QMimeData *mime = QTextBrowser::createMimeDataFromSelection();
    if (!mime || !mime->hasText())
        return mime;

    const QTextCursor cursor = textCursor();
    if (!cursor.hasSelection())
        return mime;

    // Qt's default plain-text extraction emits one U+FFFC (object
    // replacement character) per embedded image, in the same left-to-right
    // order the images appear in the document — so collecting the selected
    // images' resource names in order and zipping them positionally against
    // the placeholder characters is enough to restore the originals, without
    // needing to re-walk both structures in lockstep.
    QStringList imageNames;
    const int selStart = cursor.selectionStart();
    const int selEnd = cursor.selectionEnd();
    for (QTextBlock block = document()->findBlock(selStart);
         block.isValid() && block.position() < selEnd; block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            if (fragment.position() + fragment.length() <= selStart || fragment.position() >= selEnd)
                continue;
            if (fragment.charFormat().isImageFormat())
                imageNames << fragment.charFormat().toImageFormat().name();
        }
    }
    if (imageNames.isEmpty())
        return mime;

    QString text = mime->text();
    int nextImage = 0;
    for (int i = 0; i < text.size() && nextImage < imageNames.size(); ++i) {
        if (text.at(i) == QChar(0xFFFC)) {
            const QString emoji = EmojiRenderer::emojiForResourcePath(imageNames.at(nextImage));
            if (!emoji.isEmpty())
                text.replace(i, 1, emoji);
            ++nextImage;
        }
    }
    mime->setText(text);
    return mime;
}

void AutoHeightTextBrowser::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);
    // A width change affects wrapping, which affects height — recompute
    // against the new viewport width rather than waiting for content to
    // change again.
    document()->setTextWidth(viewport()->width());
    adjustHeight();
}

void AutoHeightTextBrowser::adjustHeight()
{
    document()->setTextWidth(viewport()->width());
    const int docHeight = static_cast<int>(document()->size().height());
    const int newHeight = docHeight + 2 * frameWidth() + 4;
    if (newHeight != height())
        setFixedHeight(newHeight);
}
