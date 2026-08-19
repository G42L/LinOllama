#pragma once

#include <QStringList>
#include <QString>

namespace TextChunker {

// Splits `text` into overlapping chunks of roughly `chunkSizeChars`
// characters each — character count, not tokens, since embedding at
// ingestion time doesn't need a local tokenizer dependency and character
// count is a good-enough proxy for "small enough to embed sensibly" at the
// granularity RAG chunking actually needs. Breaks on a paragraph or
// sentence boundary near the target size where one exists, rather than
// cutting mid-word/mid-sentence, and repeats `overlapChars` characters at
// the start of each chunk after the first, so a fact split across a chunk
// boundary is still findable from whichever side's chunk gets retrieved.
// Returns an empty list for empty/whitespace-only input.
QStringList chunkText(const QString &text, int chunkSizeChars = 1500, int overlapChars = 200);

} // namespace TextChunker
