#include "TextChunker.h"

#include <QtGlobal>

namespace TextChunker {

QStringList chunkText(const QString &text, int chunkSizeChars, int overlapChars)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};

    if (chunkSizeChars <= 0)
        chunkSizeChars = 1500;
    if (overlapChars < 0 || overlapChars >= chunkSizeChars)
        overlapChars = qMin(200, chunkSizeChars / 4);

    // Checked in priority order: prefer backing up to a paragraph break,
    // then a sentence end, then any line break, then just a word boundary —
    // whichever kind actually has an occurrence within the lookback window
    // wins; there's no scoring across kinds beyond that priority order.
    static const QStringList boundaries = {"\n\n", ". ", "! ", "? ", "\n", " "};
    const int length = trimmed.length();
    const int lookbackWindow = qMin(200, chunkSizeChars / 4);

    QStringList chunks;
    int start = 0;
    while (start < length) {
        int end = qMin(start + chunkSizeChars, length);

        if (end < length) {
            const int lookbackLimit = qMax(start + 1, end - lookbackWindow);
            for (const QString &boundary : boundaries) {
                const int idx = trimmed.lastIndexOf(boundary, end - 1);
                if (idx >= lookbackLimit && idx < end) {
                    end = idx + boundary.length();
                    break;
                }
            }
        }

        const QString chunk = trimmed.mid(start, end - start).trimmed();
        if (!chunk.isEmpty())
            chunks.append(chunk);

        if (end >= length)
            break;
        // start + 1 guards against looping forever in the pathological case
        // where end - overlapChars doesn't advance past the previous start.
        start = qMax(end - overlapChars, start + 1);
    }

    return chunks;
}

} // namespace TextChunker
