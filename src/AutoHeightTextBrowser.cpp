#include "AutoHeightTextBrowser.h"

#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QRegularExpression>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImage>

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
    QString html;
    if (extractHtmlBlocks(content, &html))
        setHtml(html);
    else
        setMarkdown(content);
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
