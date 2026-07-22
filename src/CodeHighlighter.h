#pragma once

#include <QString>

// Turns a fenced code block's raw text into syntax-highlighted HTML (a
// sequence of HTML-escaped text interspersed with <span style="color:...">
// runs for recognized keywords/strings/comments/numbers), for splicing into
// a <pre> element by AutoHeightTextBrowser — the same "pull this block out
// and build real HTML for it" pattern already used there for ```html blocks
// (see wrapRawSvgAsImages()), just covering plain source code too instead of
// leaving it to Qt's own Markdown-to-HTML conversion, which discards the
// language tag entirely and renders every line of a fenced block as its own
// separate, uncolored <pre> (verified directly against Qt).
//
// This is a lightweight, single-pass tokenizer per language family — not a
// full grammar-aware parser — so it won't catch every edge case a real
// syntax highlighter (e.g. a Tree-sitter grammar) would, but it correctly
// avoids the most common failure mode (a keyword-looking word inside a
// string or comment getting colored anyway) by scanning strings/comments as
// opaque runs it skips over, never re-entering them to look for keywords.
namespace CodeHighlighter {

// `language` is whatever followed the opening ``` (e.g. "python", "js",
// "sh") — matched case-insensitively against a small alias table covering
// common variants. Returns HTML-escaped, span-wrapped markup with real
// embedded newlines (Qt's HTML parser splits a <pre>'s lines into separate
// blocks on its own — confirmed directly — so this doesn't need to). An
// unrecognized language still renders correctly, just without color: the
// text comes back HTML-escaped with no <span> wrapping at all.
QString highlightToHtml(const QString &code, const QString &language, bool dark);

// Whether `language` (case-insensitive, matched through the same alias
// table as highlightToHtml()) is one this module actually colors — used by
// AutoHeightTextBrowser only to decide whether a fenced block needs the
// "no language tag" plain-monospace treatment vs full highlighting; both
// paths still call highlightToHtml() either way; either way still escapes
// its output safely.
bool isSupportedLanguage(const QString &language);

} // namespace CodeHighlighter
