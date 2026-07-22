#include "AutoHeightTextBrowser.h"
#include "EmojiRenderer.h"
#include "CodeHighlighter.h"
#include "Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextBlockFormat>
#include <QTextFormat>
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
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QTimer>

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

// Matches ```html fenced blocks specifically — kept separate from the
// general code-fence pattern below since ```html is treated as live,
// renderable content (charts/tables via HtmlEmbedWidget), not a source-code
// example to display as text. Shared with
// AutoHeightTextBrowser::containsHtmlBlock(), which decides whether a reply
// needs the raw/rendered toggle button at all.
const QRegularExpression &htmlFencePattern()
{
    static const QRegularExpression htmlFence(
        QStringLiteral("```html\\s*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    return htmlFence;
}

// Matches ANY fenced code block, capturing the (possibly empty) language tag
// separately from its content — a superset of htmlFencePattern() above.
const QRegularExpression &codeFencePattern()
{
    static const QRegularExpression fence(
        QStringLiteral("```([A-Za-z0-9_+#-]*)[ \\t]*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);
    return fence;
}

// QTextDocument::toHtml() always returns a *complete* document (its own
// <!DOCTYPE>/<html>/<head>/<body> wrapper), never just a fragment. Splicing
// multiple such documents back-to-back (as convertMarkdownWithCodeBlocks()
// below does, alternating Markdown-derived segments with real HTML/<pre>
// blocks) and feeding the concatenation to one setHtml() call confuses Qt's
// parser: verified directly that a <pre> block placed right after another
// segment's closing </html> gets its internal line breaks collapsed away
// (every source line runs together with no separator), even though the
// exact same <pre> renders correctly when it's the only content in the
// document. Extracting each segment's own <body>...</body> inner content
// first — so the final concatenation is one coherent flow of body content,
// not several nested "documents" glued together — avoids that entirely.
QString bodyInnerHtml(const QString &fullHtml)
{
    const int bodyOpenEnd = fullHtml.indexOf(QLatin1Char('>'), fullHtml.indexOf(QStringLiteral("<body")));
    const int bodyCloseStart = fullHtml.lastIndexOf(QStringLiteral("</body>"));
    if (bodyOpenEnd < 0 || bodyCloseStart < 0 || bodyCloseStart <= bodyOpenEnd)
        return fullHtml; // shouldn't happen for Qt's own toHtml() output — safe fallback either way
    return fullHtml.mid(bodyOpenEnd + 1, bodyCloseStart - bodyOpenEnd - 1);
}

// Builds one combined HTML document from `content`: text outside a fenced
// code block is converted from Markdown via Qt's own converter (so
// formatting stays consistent with the rest of the app); text inside one is
// spliced in as real HTML — either live-rendered content for a ```html
// block (after wrapRawSvgAsImages(), unchanged from before), or a
// syntax-highlighted <pre> for any other language (see CodeHighlighter).
// This runs unconditionally now, even with no fence at all present, since
// Qt's own setMarkdown()/toHtml() for a fenced block discards the language
// tag entirely and renders every source line as its own separate,
// uncolored <pre> block (verified directly against Qt) — there's no
// "acceptable" code-fence handling to fall back to.
//
// `font` is the destination widget's own (already-polished) font — each
// throwaway QTextDocument used here to convert a Markdown segment to HTML
// starts out with Qt's generic default font, not the browser's actual
// themed one, and toHtml() bakes whatever font it has directly into the
// exported HTML as inline styles. Without setting it explicitly, the
// spliced-in segment would render at the wrong (generic) size once handed
// back to the real browser's setHtml(), overriding its QSS font-size.
//
// `codeBlockTexts` (cleared first) collects each highlighted block's
// original, un-highlighted source in order — index N here matches the
// "copycode:N" href generated for its Copy link, which
// AutoHeightTextBrowser::onAnchorClicked() resolves back through the
// widget's own m_codeBlockTexts. A ```html block doesn't get a Copy link at
// all: it's rendered as live content (a chart, a table), not shown as
// source text there's any point copying.
QString convertMarkdownWithCodeBlocks(const QString &content, const QFont &font, bool dark,
                                       QVector<QString> *codeBlockTexts)
{
    const QRegularExpression &fence = codeFencePattern();

    if (!content.contains(fence)) {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.setMarkdown(content);
        return doc.toHtml();
    }

    const QString codeBg = Theme::colorToken(QStringLiteral("codeBg"), dark);
    const QString linkColor = Theme::colorToken(QStringLiteral("secondaryText"), dark);
    const int linkPx = qMax(1, qRound(QFontInfo(font).pixelSize() * 0.85));
    // A fixed size from Settings > Formatting rather than scaled off the
    // surrounding text (like the spacing settings there, and unlike linkPx
    // above) — it's an icon, not text, so it doesn't need to track the
    // font-size slider the same way. Deliberately bigger than the header
    // line's own text by default — a small icon matched to text size read
    // as fussy/hard to hit; letting it overhang the line it's on is fine,
    // Qt just grows that line's own height to fit rather than clipping or
    // overlapping the block below (verified directly).
    const int copyIconPx = QSettings().value(QStringLiteral("formatting/copyIconSize"), 16).toInt();
    // Computed once — every code block in this message shares the same
    // theme/size, so there's no reason to re-rasterize the icon per block.
    const QString copyIconUri = Theme::themedIconDataUri(QStringLiteral(":/icons/copy.svg"), dark, copyIconPx,
                                                           QStringLiteral("secondaryText"));

    QString result;
    int lastEnd = 0;
    QRegularExpressionMatchIterator it = fence.globalMatch(content);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();

        const QString before = content.mid(lastEnd, match.capturedStart() - lastEnd);
        if (!before.trimmed().isEmpty()) {
            QTextDocument doc;
            doc.setDefaultFont(font);
            doc.setMarkdown(before);
            result += bodyInnerHtml(doc.toHtml());
        }

        const QString language = match.captured(1).trimmed();
        const QString code = match.captured(2);
        if (language.compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
            result += wrapRawSvgAsImages(code);
        } else {
            // <pre> doesn't collapse whitespace (confirmed — indentation
            // survives correctly already), so a plain trailing space per
            // source line gives a right inset approximating the 6px CSS
            // padding Qt's rich-text engine won't honor on <pre> itself.
            // Right only, deliberately not left: this widget still allows
            // selecting/copying text directly (TextSelectableByMouse), not
            // just via the Copy icon, and a *leading* space on every line
            // would get dragged into that selection too — for
            // indentation-sensitive code (Python, YAML), that silently
            // corrupts it on paste. A trailing space is comparatively
            // harmless if it ends up in a manual copy. Applied to a
            // separate copy fed to the highlighter — codeBlockTexts below
            // still gets the real, unpadded `code`, so the Copy button
            // never copies these extra spaces either way.
            QStringList paddedLines = code.split(QLatin1Char('\n'));
            for (QString &line : paddedLines)
                line = line + QLatin1Char(' ');
            const QString highlighted = CodeHighlighter::highlightToHtml(
                paddedLines.join(QLatin1Char('\n')), language, dark);
            // The language label and Copy link both get the same
            // background-color as the code itself — that's what makes
            // applyBlockSpacing() treat them as part of the same code block
            // (it detects "is this line part of a code block" purely from
            // whether the block carries that background brush, verified
            // directly) and collapse the gaps between them, so the whole
            // thing reads as one continuous panel instead of three stacked
            // ones. No language tag on the fence — just skip that label
            // instead of showing an empty one. Flush left, matching the
            // code below (which is deliberately not left-inset either —
            // see the comment on paddedLines above).
            if (!language.isEmpty()) {
                result += QStringLiteral("<div style=\"background-color:%1; color:%2; font-size:%3px;\">%4</div>")
                    .arg(codeBg).arg(linkColor).arg(linkPx).arg(language.toHtmlEscaped());
            }
            // Qt's rich-text CSS doesn't honor margin/padding on an inline
            // <img> (verified: silently dropped, same as the <pre> padding
            // limitation found earlier) — trailing &nbsp;s after the icon,
            // inside this same right-aligned div, push it left of the true
            // right edge instead, leaving visible space to its right.
            result += QStringLiteral(
                "<div style=\"background-color:%1; text-align:right;\"><a href=\"copycode:%2\" "
                "style=\"text-decoration:none;\"><img src=\"%3\" width=\"%4\" height=\"%4\" "
                "style=\"vertical-align:middle;\"></a>&nbsp;&nbsp;&nbsp;</div>"
                "<pre style=\"background-color:%1; font-family:'Monospace';\">%5</pre>")
                .arg(codeBg).arg(codeBlockTexts->size()).arg(copyIconUri).arg(copyIconPx).arg(highlighted);
            // Only the actual source goes on the clipboard — the language
            // label and Copy link above are display-only, never part of
            // what gets copied.
            codeBlockTexts->append(code);
        }
        lastEnd = match.capturedEnd();
    }

    const QString after = content.mid(lastEnd);
    if (!after.trimmed().isEmpty()) {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.setMarkdown(after);
        result += bodyInnerHtml(doc.toHtml());
    }

    return result;
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

    // Qt splits a multi-line syntax-highlighted code block into one <pre>
    // per source line (verified directly — there's no way to keep it as a
    // single block), each still carrying the background brush explicitly
    // set on that <pre> (also verified) — which is what lets a "line
    // belongs to a code block" check use that alone, without needing some
    // other out-of-band marker. Without treating consecutive code lines
    // specially, the same paragraphSpacing gap applied between two ordinary
    // paragraphs would separate every single line of the same code block
    // with visible space, making one intended block look like several
    // stacked ones instead of a single continuous panel. The gap between
    // block i and i+1 is controlled by block i's *bottom* margin, so this
    // has to look ahead at the *next* block to decide whether the current
    // one is still inside a run of code lines — not at the previous one.
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat fmt = block.blockFormat();
        const bool inList = block.textList() != nullptr;
        const bool isHeading = fmt.headingLevel() > 0;
        const bool isFirstBlock = (block == doc->begin());
        const bool isCodeLine = fmt.hasProperty(QTextFormat::BackgroundBrush);
        const QTextBlock nextBlock = block.next();
        const bool nextIsCodeLine = nextBlock.isValid()
            && nextBlock.blockFormat().hasProperty(QTextFormat::BackgroundBrush);

        fmt.setTopMargin(isHeading && !isFirstBlock ? headingSpacingBefore : 0);
        fmt.setBottomMargin(isCodeLine && nextIsCodeLine
                                 ? 0
                                 : (inList ? listItemSpacing : paragraphSpacing));
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
    // Off, not on — a plain http(s) link still opens in the system browser
    // (see onAnchorClicked()), but Qt's own automatic handling for
    // setOpenExternalLinks(true) intercepts external links *before*
    // anchorClicked() ever fires, which would silently swallow this
    // widget's "copycode:<index>" links (see convertMarkdownWithCodeBlocks())
    // instead of letting this widget's own handler see them.
    setOpenExternalLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &AutoHeightTextBrowser::onAnchorClicked);
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

void AutoHeightTextBrowser::setMarkdownWithHtmlBlocks(const QString &content, bool dark)
{
    ensurePolished(); // font() below must reflect the actual QSS-applied font, not the pre-stylesheet default
    const QFont ownFont = font();
    const QString sanitized = sanitizeLatexMath(content);

    m_codeBlockTexts.clear();
    const QString html = convertMarkdownWithCodeBlocks(sanitized, ownFont, dark, &m_codeBlockTexts);

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

void AutoHeightTextBrowser::onAnchorClicked(const QUrl &url)
{
    if (url.scheme() == QLatin1String("copycode")) {
        bool ok = false;
        const int index = url.path().toInt(&ok);
        if (ok && index >= 0 && index < m_codeBlockTexts.size())
            QGuiApplication::clipboard()->setText(m_codeBlockTexts.at(index));

        // TextSelectableByMouse (needed so replies can be selected/copied
        // normally) also leaves the just-clicked icon looking "selected"
        // (highlighted) afterward — clicking a Copy icon is an action, not
        // text selection, so that highlight shouldn't stick around.
        // Deferred to the next event-loop turn, not done inline here, in
        // case QTextBrowser's own click handling sets the selection *after*
        // emitting this signal rather than before.
        QTimer::singleShot(0, this, [this]() {
            QTextCursor cursor = textCursor();
            cursor.clearSelection();
            setTextCursor(cursor);
        });
        return;
    }
    // Replicates what setOpenExternalLinks(true) used to do automatically —
    // see the constructor's own comment for why that's off now.
    QDesktopServices::openUrl(url);
}

void AutoHeightTextBrowser::doSetSource(const QUrl &name, QTextDocument::ResourceType type)
{
    Q_UNUSED(name);
    Q_UNUSED(type);
    // Deliberately empty — see the declaration's own comment for why this
    // widget never wants QTextBrowser's built-in link-navigation behavior.
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
