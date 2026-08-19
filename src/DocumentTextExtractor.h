#pragma once

#include <QString>

// Extracts plain text from a file for RAG ingestion, dispatching by file
// extension. Deliberately shells out (via QProcess) to existing system
// tools rather than linking a parsing library — pdftotext (poppler-utils)
// for PDF, unzip for pulling a .docx's own XML out of its zip container —
// both are small, common Linux packages already floated as the app's
// intended approach (see README's dependency table), and avoids pulling a
// PDF-parsing library's own licensing/build footprint into this binary.
namespace DocumentTextExtractor {

// Supported: .txt, .md (read directly), .pdf (via pdftotext), .docx (via
// unzip + a small internal XML walk). Anything else, or a failure partway
// through (missing pdftotext/unzip binary, corrupt file, empty extraction
// result), returns an empty string with *errorOut set to a message safe to
// show the user directly — callers should treat empty text as "skip this
// file, surface the error" rather than silently ingesting nothing.
// errorOut may be null if the caller doesn't need the reason.
QString extractText(const QString &filePath, QString *errorOut = nullptr);

// The file dialog filter string RAG ingestion's "Add document(s)…" button
// uses — kept here so the supported-extensions list has exactly one source
// of truth alongside extractText()'s own dispatch.
QString fileDialogFilter();

} // namespace DocumentTextExtractor
