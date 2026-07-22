#include "EmojiRenderer.h"

#include <QHash>
#include <QFile>
#include <QStringList>

namespace {

constexpr uint kVariationSelector16 = 0xFE0F;
constexpr uint kZeroWidthJoiner = 0x200D;
constexpr uint kCombiningEnclosingKeycap = 0x20E3;

bool isPictographic(uint cp)
{
    return (cp >= 0x2600 && cp <= 0x27BF)
        || (cp >= 0x2300 && cp <= 0x23FF)
        || (cp >= 0x2B00 && cp <= 0x2BFF)
        || (cp >= 0x2190 && cp <= 0x21FF)
        || (cp >= 0x1F300 && cp <= 0x1FAFF);
}

bool isRegionalIndicator(uint cp)
{
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

bool isSkinToneModifier(uint cp)
{
    return cp >= 0x1F3FB && cp <= 0x1F3FF;
}

bool isKeycapBase(uint cp)
{
    return (cp >= '0' && cp <= '9') || cp == '#' || cp == '*';
}

// Noto Emoji's bundled filenames follow "emoji_u<hex>[_<hex>...].svg", each
// codepoint padded to at least 4 hex digits (e.g. "emoji_u0023_20e3.svg" for
// the keycap "#", "emoji_u1f41b.svg" for the bug). Not every sequence a
// model emits has an exact bundled file (e.g. an unexpected combination of
// variation selector/skin tone), so this tries the full matched cluster
// first, then progressively shorter prefixes — dropping a trailing
// variation selector, then trailing ZWJ-joined segments one at a time —
// before giving up and leaving the original text untouched.
QString emojiResourcePath(const QList<uint> &cluster)
{
    static QHash<QList<uint>, QString> cache;
    const auto cacheIt = cache.constFind(cluster);
    if (cacheIt != cache.constEnd())
        return cacheIt.value();

    auto pathFor = [](const QList<uint> &cps) {
        QStringList hex;
        for (uint cp : cps)
            hex << QString::number(cp, 16).rightJustified(4, QLatin1Char('0'));
        return QStringLiteral(":/emoji/emoji_u%1.svg").arg(hex.join(QLatin1Char('_')));
    };

    QString found;
    QList<uint> attempt = cluster;
    while (!attempt.isEmpty()) {
        const QString candidate = pathFor(attempt);
        if (QFile::exists(candidate)) {
            found = candidate;
            break;
        }
        if (attempt.last() == kVariationSelector16) {
            attempt.removeLast();
            continue;
        }
        const int zwjIndex = attempt.lastIndexOf(kZeroWidthJoiner);
        if (zwjIndex < 0)
            break;
        attempt = attempt.sliced(0, zwjIndex);
    }

    cache.insert(cluster, found);
    return found;
}

} // namespace

namespace EmojiRenderer {

// Scans Unicode codepoints directly rather than matching a QRegularExpression
// against the UTF-16 QString — deliberately, after an earlier regex-based
// version (matching \x{1F300}-\x{1FAFF}-style ranges) never actually matched
// real emoji at runtime despite compiling without error. Working in codepoint
// space sidesteps that class of problem entirely: no dependence on how
// PCRE2/QRegularExpression handles non-BMP ranges or surrogate pairs.
QString substituteWithImages(const QString &text, int pixelSize)
{
    const QList<uint> cps = text.toUcs4();
    const int n = cps.size();
    if (n == 0)
        return text;

    QString result;
    int i = 0;
    while (i < n) {
        QList<uint> cluster;

        // Keycap sequence: digit/#/* + optional variation selector + U+20E3.
        if (isKeycapBase(cps[i])) {
            int j = i + 1;
            if (j < n && cps[j] == kVariationSelector16)
                j++;
            if (j < n && cps[j] == kCombiningEnclosingKeycap) {
                cluster = cps.sliced(i, j - i + 1);
                i = j + 1;
            }
        }

        // Flag: a pair of regional indicator symbols.
        if (cluster.isEmpty() && i + 1 < n
            && isRegionalIndicator(cps[i]) && isRegionalIndicator(cps[i + 1])) {
            cluster = cps.sliced(i, 2);
            i += 2;
        }

        // A pictographic codepoint, optionally followed by a variation
        // selector and/or skin tone modifier, optionally chained further via
        // ZWJ into another such group (e.g. multi-person/profession emoji).
        if (cluster.isEmpty() && isPictographic(cps[i])) {
            int j = i + 1;
            if (j < n && cps[j] == kVariationSelector16)
                j++;
            if (j < n && isSkinToneModifier(cps[j]))
                j++;
            while (j + 1 < n && cps[j] == kZeroWidthJoiner && isPictographic(cps[j + 1])) {
                j += 2;
                if (j < n && cps[j] == kVariationSelector16)
                    j++;
                if (j < n && isSkinToneModifier(cps[j]))
                    j++;
            }
            cluster = cps.sliced(i, j - i);
            i = j;
        }

        if (!cluster.isEmpty()) {
            const QString path = emojiResourcePath(cluster);
            result += path.isEmpty()
                ? QString::fromUcs4(reinterpret_cast<const char32_t *>(cluster.constData()), cluster.size())
                : QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%2\" "
                                  "style=\"vertical-align:middle\">").arg(path).arg(pixelSize);
        } else {
            result += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cps[i]), 1);
            i++;
        }
    }
    return result;
}

QString escapedPlainTextWithImages(const QString &text, int pixelSize)
{
    return substituteWithImages(text.toHtmlEscaped(), pixelSize);
}

QString emojiForResourcePath(const QString &path)
{
    const int slashIndex = path.lastIndexOf(QLatin1Char('/'));
    const QString fileName = slashIndex >= 0 ? path.mid(slashIndex + 1) : path;

    static const QString prefix = QStringLiteral("emoji_u");
    static const QString suffix = QStringLiteral(".svg");
    if (!fileName.startsWith(prefix) || !fileName.endsWith(suffix))
        return QString();

    const QString hexPart = fileName.mid(prefix.length(),
        fileName.length() - prefix.length() - suffix.length());

    QList<uint> codepoints;
    for (const QString &hex : hexPart.split(QLatin1Char('_'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const uint cp = hex.toUInt(&ok, 16);
        if (!ok)
            return QString();
        codepoints << cp;
    }
    if (codepoints.isEmpty())
        return QString();

    return QString::fromUcs4(reinterpret_cast<const char32_t *>(codepoints.constData()), codepoints.size());
}

} // namespace EmojiRenderer
