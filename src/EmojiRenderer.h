#pragma once

#include <QString>

// Qt's own colour-emoji rendering can't be relied on: it depends on whatever
// emoji font Fontconfig/FreeType resolves on the host — present, absent,
// missing specific glyphs, or (as observed) the host's Qt/FreeType build
// simply not rasterizing color bitmap/COLR glyphs at all for QTextDocument-
// or QLabel-based widgets, giving a tofu box regardless of what font is
// installed or fallback-registered. These functions instead detect emoji
// characters and substitute them with <img> tags pointing at the bundled
// Noto Emoji SVG set (src/emoji/, registered via resources_emoji.qrc), so
// rendering is identical on every platform regardless of the host's fonts.
//
// Shared by every widget that displays free-form model/user text: chat
// bubbles (AutoHeightTextBrowser), the "Thinking" section, and tool call
// arguments/results (both plain QLabels).
namespace EmojiRenderer {

// Replaces every recognized emoji character/sequence in `text` with an
// <img src=":/emoji/..."> tag sized `pixelSize` square. Leaves anything with
// no bundled SVG (an unrecognized or unusual sequence) untouched as plain
// text. Safe to call on text that's about to be fed to a QTextDocument-based
// rich text renderer (QTextBrowser::setHtml/setMarkdown, or a QLabel in
// RichText mode) — not safe to call on text that's about to stay plain,
// since the <img> tag itself would show up as literal text.
//
// `pixelSize` should be computed from the destination widget's actual font
// (see AutoHeightTextBrowser.cpp/ThinkingSectionWidget.cpp for the pattern:
// ensurePolished() then QFontMetrics(font()).height()) — Qt's rich-text CSS
// doesn't resolve "em"/"ex" units on <img> (verified: silently resolves to
// 0), so there's no way to size the image relative to text purely in CSS;
// the caller has to know the pixel size up front.
QString substituteWithImages(const QString &text, int pixelSize);

// For content that must render literally (a user's own typed message, model
// reasoning/tool text) rather than as Markdown: HTML-escapes `text` so no
// markup in it is interpreted, substitutes recognized emoji for images, and
// wraps the result in a `white-space:pre-wrap` block so line breaks/spacing
// still look like plain text. Feed the result to setHtml() (QTextBrowser) or
// setText() with Qt::RichText (QLabel) — never to a plain-text setter.
QString escapedPlainTextWithImages(const QString &text, int pixelSize);

// Reverses substituteWithImages()'s filename convention: given an <img>'s
// resource path (e.g. ":/emoji/emoji_u1f41b.svg"), reconstructs the original
// emoji character(s) it replaced. Used when building clipboard data for a
// selection that includes one of these images — copying an <img> alone
// would otherwise paste as the Unicode object replacement character (￼)
// rather than the emoji itself, since the image has no text behind it.
// Returns an empty string if `path` doesn't look like one of ours.
QString emojiForResourcePath(const QString &path);

} // namespace EmojiRenderer
