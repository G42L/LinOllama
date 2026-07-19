#include "AutoHeightTextBrowser.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>
#include <QVector>
#include <QPair>
#include <QHash>

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

bool extractHtmlBlocks(const QString &content, QString *htmlOut)
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
            doc.setMarkdown(before);
            result += doc.toHtml();
        }

        result += wrapRawSvgAsImages(match.captured(1));
        lastEnd = match.capturedEnd();
    }

    const QString after = content.mid(lastEnd);
    if (!after.trimmed().isEmpty()) {
        QTextDocument doc;
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
    const QString sanitized = sanitizeLatexMath(content);

    QString html;
    if (extractHtmlBlocks(sanitized, &html))
        setHtml(html);
    else
        setMarkdown(sanitized);
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
