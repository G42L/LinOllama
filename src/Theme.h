#pragma once

#include <QString>
#include <QIcon>
#include <QVector>

// Central app-wide stylesheet. Aims for a warm, rounded "AI desktop app"
// feel (soft accent color, rounded corners on inputs/buttons/bubbles)
// rather than the platform's default Qt widget look, in both a light and a
// dark variant — loosely in the spirit of Claude Desktop's own UI.
//
// Widgets opt in by objectName; see the #name selectors in Theme.cpp and
// the matching setObjectName() calls in ChatWidget, MainWindow,
// StatsStripWidget, SettingsDialog, and ConversationListItemWidget.
namespace Theme
{
// dark=false returns the light variant, dark=true the dark variant. Both
// share one QSS template internally (see Theme.cpp) so they can't drift out
// of sync with each other — a selector added to one is automatically in the
// other too, just pulling different color tokens. If the user has picked a
// custom "Application" color in Settings (QSettings key
// "appearance/accentColor"), this substitutes it for the theme's own
// accent/accentHover/accentDisabledBg tokens too — see currentAccentColor().
QString styleSheet(bool dark);

// Looks up a single color token (e.g. "secondaryText", "accent") from the
// same table styleSheet() draws from, for code that needs a raw color
// rather than a QSS rule — currently just loadThemedIcon(). Note: this
// returns the theme's built-in "accent", not the user's custom Application
// color override — use currentAccentColor() for the effective one.
QString colorToken(const QString &tokenName, bool dark);

// The accent color actually in effect right now: the user's custom
// "Application" color from Settings if they've set one (QSettings key
// "appearance/accentColor"), otherwise the theme's own default accent.
// Anything that wants to default to "the app's accent color" (e.g. an
// unset stats-meter color) should read this rather than
// colorToken("accent", dark), so it follows a custom accent too.
QString currentAccentColor(bool dark);

// Loads an SVG icon resource (e.g. ":/icons/microphone.svg") that contains
// a literal "{{iconColor}}" placeholder in its stroke/fill, substitutes in
// the given theme's color token, and rasterizes it at sizePx — Qt's SVG
// icon engine doesn't follow QSS, so icons need to be recolored explicitly
// like this rather than just styled. Returns a null QIcon if the resource
// can't be read (e.g. the Qt Svg module isn't linked/available) rather than
// asserting — callers should treat a null icon the same as "no icon".
QIcon loadThemedIcon(const QString &resourcePath, bool dark, int sizePx = 16,
                      const QString &colorTokenName = QStringLiteral("secondaryText"));

// Same recoloring as loadThemedIcon(), but rasterized fresh at each size in
// sizesPx and combined into one QIcon — for window/tray icons, where the OS
// requests several different pixel sizes (tray, taskbar, alt-tab, etc.) and
// a single small pixmap stretched up to fill a big one would look blurry.
QIcon loadThemedIconMultiSize(const QString &resourcePath, bool dark, const QVector<int> &sizesPx,
                               const QString &colorTokenName = QStringLiteral("secondaryText"));
}
