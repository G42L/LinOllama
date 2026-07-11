#include "Theme.h"

#include <QHash>
#include <QFile>
#include <QSvgRenderer>
#include <QPixmap>
#include <QPainter>
#include <QSettings>
#include <QColor>

namespace {

// One QSS template shared by both variants, written against named color
// tokens ({{token}}) rather than literal hex values. Light and dark just
// supply different token tables below — this is what guarantees a selector
// added for one theme automatically exists in the other too.
QString applyTokens(QString qss, const QHash<QString, QString> &tokens)
{
    for (auto it = tokens.constBegin(); it != tokens.constEnd(); ++it)
        qss.replace(QStringLiteral("{{%1}}").arg(it.key()), it.value());
    return qss;
}

QString templateQss()
{
    return QStringLiteral(R"(
QWidget {
    background-color: {{bg}};
    color: {{text}};
    font-size: 13px;
}

QMainWindow {
    background-color: {{bg}};
}

/* Explicit on purpose: without this, a tooltip shown for a widget that has
   its own direct setStyleSheet() call (e.g. Settings' color swatches) can
   inherit that widget's own background instead of a normal tooltip look —
   this rule always wins regardless of what the hovered widget is showing. */
QToolTip {
    background-color: {{surface}};
    color: {{text}};
    border: 1px solid {{border}};
    padding: 4px 6px;
    border-radius: 4px;
}

/* --- Window top bar ----------------------------------------------------
   Sits above the sidebar/chat/stats splitter — holds the sidebar
   collapse/expand toggle, which has to stay reachable even once the
   sidebar itself is fully hidden (see MainWindow::setSidebarCollapsed()). */
#windowTopBar {
    background-color: {{bg}};
}

/* --- Sidebar ---------------------------------------------------------- */
#sidebar {
    background-color: {{sidebarBg}};
    border-right: 1px solid {{border}};
}

QListWidget {
    background-color: transparent;
    border: none;
    outline: 0;
}

QListWidget::item {
    border-radius: 6px;
    margin: 1px 4px;
}

QListWidget::item:selected {
    background-color: {{itemSelectedBg}};
}

QListWidget::item:hover:!selected {
    background-color: {{itemHoverBg}};
}

#conversationTitle {
    background-color: transparent;
    color: {{text}};
}

#conversationMenuButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 15px;
    border: none;
    border-radius: 4px;
    padding: 0px 4px;
}

#conversationMenuButton:hover {
    background-color: {{menuButtonHoverBg}};
}

#settingsButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 16px;
    border: none;
    border-radius: 6px;
    padding: 4px 8px;
}

#settingsButton:hover {
    background-color: {{menuButtonHoverBg}};
}

#settingsHint {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 11px;
}

/* --- Generic buttons (dialog Cancel/Close, etc.) ----------------------- */
QPushButton {
    background-color: {{surface}};
    border: 1px solid {{border}};
    border-radius: 8px;
    padding: 6px 14px;
}

QPushButton:hover {
    background-color: {{menuHoverBg}};
}

/* --- Group boxes (Settings dialog sections) ------------------------------ */
QGroupBox {
    border: 1px solid {{border}};
    border-radius: 8px;
    margin-top: 10px;
    padding: 12px 8px 8px 8px;
    font-weight: 600;
}

QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 8px;
    padding: 0 4px;
}

/* --- Combo boxes (model picker, settings dropdowns) --------------------- */
QComboBox {
    background-color: {{surface}};
    border: 1px solid {{border}};
    border-radius: 8px;
    padding: 4px 8px;
}

QComboBox:hover {
    border: 1px solid {{accent}};
}

QComboBox QAbstractItemView {
    background-color: {{surface}};
    border: 1px solid {{border}};
    outline: 0;
    selection-background-color: {{menuHoverBg}};
    color: {{text}};
}

/* --- Chat area ---------------------------------------------------------- */
#messageScrollArea, #messagesContainer {
    background-color: {{surface}};
    border: none;
}

/* Each message's row (see ChatWidget::appendMessageBubble) is otherwise a
   plain, objectName-less QWidget, which would fall back to the generic
   app-wide {{bg}} rule above — a different shade than #messagesContainer's
   own {{surface}} fill. That mismatch showed up as a visible square patch
   behind every bubble's rounded corners (worst on the assistant bubble,
   which has no fill of its own to hide it behind). Transparent lets the
   container's own surface color show through with no seam, in both themes. */
#messageRow {
    background-color: transparent;
}

/* Homepage-style empty state: plain app background (not a filled panel —
   that read as "one big box taking the whole height"), with just a
   centered title/subtitle above the docked input card. */
#emptyStateTitle {
    background-color: transparent;
    color: {{text}};
    font-size: 22px;
    font-weight: 700;
}

#emptyStateSubtitle {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 13px;
}

#userBubble {
    background-color: {{userBubbleBg}};
    border-radius: 14px;
}

#assistantBubble {
    background-color: {{assistantBubbleBg}};
    border: none;
}

#errorBubble {
    background-color: {{errorBg}};
    border: 1px solid {{errorBorder}};
    border-radius: 14px;
}

#bubbleText {
    background-color: transparent;
    border: none;
}

#errorBubble #bubbleText {
    color: {{errorText}};
}

/* --- Collapsible "thinking" trace (reasoning models) --------------------- */
#thinkingHeader {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 12px;
    font-style: italic;
    border: none;
    padding: 2px 0px;
}

#thinkingHeader:hover {
    color: {{text}};
}

#thinkingBody {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 12px;
    border-left: 2px solid {{border}};
    padding-left: 8px;
}

/* --- Context usage strip ------------------------------------------------ */
#contextUsageBar {
    background-color: {{bg}};
    border-top: 1px solid {{hairline}};
}

#contextUsageLabel {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 11px;
}

#jumpToButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 11px;
    border: none;
    border-radius: 6px;
    padding: 2px 6px;
}

#jumpToButton:hover {
    background-color: {{menuButtonHoverBg}};
    color: {{text}};
}

QProgressBar#contextUsageProgress {
    background-color: {{progressTrack}};
    border: none;
    border-radius: 3px;
}

QProgressBar#contextUsageProgress::chunk {
    background-color: {{accent}};
    border-radius: 3px;
}

QProgressBar#contextUsageProgress[nearLimit="true"]::chunk {
    background-color: {{danger}};
}

/* Settings' context-length and meter-smoothing sliders — filled portion and
   handle both use {{accent}}, so they follow a custom "Application" color
   the same way the progress bars above do. */
QSlider#contextLengthSlider::groove:horizontal,
QSlider#meterSmoothingSlider::groove:horizontal {
    background-color: {{progressTrack}};
    height: 6px;
    border-radius: 3px;
}

QSlider#contextLengthSlider::sub-page:horizontal,
QSlider#meterSmoothingSlider::sub-page:horizontal {
    background-color: {{accent}};
    height: 6px;
    border-radius: 3px;
}

QSlider#contextLengthSlider::handle:horizontal,
QSlider#meterSmoothingSlider::handle:horizontal {
    background-color: {{accent}};
    width: 14px;
    height: 14px;
    margin: -4px 0;
    border-radius: 7px;
}

/* --- Input bar ------------------------------------------------------------ */
#inputBar {
    background-color: {{bg}};
}

#attachButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 16px;
    font-weight: 600;
    border: none;
    border-radius: 8px;
    padding: 4px 10px;
}

#attachButton:hover {
    background-color: {{menuButtonHoverBg}};
    color: {{text}};
}

#toolsButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 12px;
    border: none;
    border-radius: 8px;
    /* Extra right padding makes room for the menu-indicator arrow below so
       it doesn't overlap "Tools"'s own text. */
    padding: 4px 18px 4px 10px;
}

#toolsButton:hover {
    background-color: {{menuButtonHoverBg}};
    color: {{text}};
}

/* Fusion's default QToolButton::menu-indicator (the little dropdown arrow
   Qt draws automatically once a QMenu is set via setMenu()) anchors to the
   bottom-right corner by default — sensible for a toolbar button whose
   main content is a big icon with the arrow tucked in a corner below it,
   but this button is just a single line of text, so that read as the arrow
   sitting at the bottom instead of centered next to "Tools". Explicitly
   centering it vertically on the right fixes that. */
#toolsButton::menu-indicator {
    subcontrol-origin: padding;
    subcontrol-position: right center;
}

/* At least one tool (web search / no-thinking) is active — see
   ChatWidget::updateToolsButtonAppearance(). Stays visible with the menu
   closed, same [property="true"] pattern as #inputCard[focused="true"]. */
#toolsButton[active="true"] {
    color: {{accent}};
    font-weight: 600;
}

#voiceButton {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 13px;
    border: none;
    border-radius: 8px;
    padding: 4px 10px;
}

#voiceButton:hover {
    background-color: {{menuButtonHoverBg}};
    color: {{text}};
}

/* Held down and actively recording — see onVoicePressed()/onVoiceReleased(). */
#voiceButton[recording="true"] {
    color: {{danger}};
    background-color: {{errorBg}};
}

#attachmentChip {
    background-color: {{surface}};
    border: 1px solid {{border}};
    border-radius: 10px;
}

#attachmentChipLabel {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 11px;
}

#attachmentChipRemove {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 12px;
    font-weight: 600;
    border: none;
    border-radius: 4px;
    padding: 0px 2px;
}

#attachmentChipRemove:hover {
    background-color: {{errorBg}};
    color: {{danger}};
}

#attachmentsSummaryLabel {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 11px;
    font-weight: 600;
}

/* --- Message timestamp + Edit/Retry (user bubbles only) ------------------- */
/* Matches #messageScrollArea/#messagesContainer's own fill, so this row
   blends into the chat pane instead of showing as a mismatched patch — a
   plain QWidget with no objectName would otherwise fall back to the
   generic app-wide {{bg}} rule below, which is a different color. */
#messageActionsRow {
    background-color: {{surface}};
}

#messageTimestampLabel {
    background-color: transparent;
    color: {{secondaryText}};
    font-size: 10px;
}

/* Icon-only (see edit.svg/retry.svg) — the icon itself is pre-colored via
   Theme::loadThemedIcon() at creation time, this just sizes/pads the button
   around it. */
#messageActionButton {
    background-color: transparent;
    border: none;
    border-radius: 4px;
    padding: 2px 4px;
}

#messageActionButton:hover {
    background-color: {{menuButtonHoverBg}};
}

/* Same fill as the user bubble it's replacing the text of, so it reads as
   part of the same prompt rather than a separate surface/card. */
#messageEditBox {
    background-color: {{userBubbleBg}};
    color: {{text}};
    border: 1px solid {{border}};
    border-radius: 8px;
    padding: 6px 8px;
}

#messageEditBox:focus {
    border: 1px solid {{accent}};
}

QPushButton#editSaveButton {
    background-color: {{accent}};
    color: #FFFFFF;
    border: none;
}

QPushButton#editSaveButton:hover {
    background-color: {{accentHover}};
}

/* The rounded card wrapping the textarea + toolbar (attach/model/send) as
   one unit — see ChatWidget's constructor. The border here is what used to
   live directly on #messageInput; it moved out to the card once the
   textarea stopped being the only thing in the input area. */
#inputCard {
    background-color: {{surface}};
    border: 1px solid {{border}};
    border-radius: 18px;
}

/* QSS has no :focus-within, so ChatWidget::eventFilter() toggles this
   property when the textarea gains/loses focus, on behalf of the card. */
#inputCard[focused="true"] {
    border: 1px solid {{accent}};
}

/* The row holding attach/tools/model/voice/send (see ChatWidget's
   constructor) is otherwise a plain, objectName-less QWidget, which would
   fall back to the generic app-wide {{bg}} rule below — a different shade
   than #inputCard's own {{surface}} fill just above. That mismatch showed
   up as a visibly different-colored band behind the whole toolbar row, on
   top of each individual button's own background — same root cause as
   #messageRow's fix elsewhere in this file. Transparent lets the card's
   own surface color show through with no seam. */
#inputToolRow {
    background-color: transparent;
}

#messageInput {
    background-color: transparent;
    border: none;
    padding: 2px 2px 0px 2px;
}

/* Flattened to read as a toolbar item sitting next to the attach/send
   buttons, rather than a bordered form control — the generic QComboBox
   rule above still applies everywhere else (e.g. Settings dialog). */
#modelCombo {
    background-color: transparent;
    border: none;
    color: {{secondaryText}};
    padding: 4px 6px;
}

#modelCombo:hover {
    background-color: {{menuButtonHoverBg}};
    border-radius: 8px;
}

/* Flat, like the attach/tools/voice buttons it sits alongside — no filled
   pill behind it. {{accent}} text/icon color is what keeps it visually
   distinct as the primary action instead of the background. */
QPushButton#sendButton {
    background-color: transparent;
    color: {{accent}};
    border: none;
    border-radius: 8px;
    padding: 4px 10px;
    font-weight: 600;
}

QPushButton#sendButton:hover {
    background-color: {{menuButtonHoverBg}};
}

QPushButton#sendButton:disabled {
    background-color: transparent;
    color: {{secondaryText}};
}

/* "Thick arrow" mode (see SettingsDialog's "Send button" combo): a bigger,
   bolder glyph. Only what differs from the base rule above needs restating. */
QPushButton#sendButton[arrowStyle="true"] {
    font-size: 18px;
    font-weight: 700;
}

/* Paper-plane icon mode (default): square-ish padding since there's no text
   alongside the icon to balance against. */
QPushButton#sendButton[planeStyle="true"] {
    padding: 4px 8px;
}

/* "Filled send button" in Settings (off by default) — the button's original
   look, a solid accent-colored pill, for anyone who preferred that over the
   flat style above. These come after the flat rules above so they win on
   the properties they both set, same [property="value"] override pattern
   used throughout this file (e.g. #inputCard[focused="true"]). */
QPushButton#sendButton[filled="true"] {
    background-color: {{accent}};
    color: #FFFFFF;
    border-radius: 10px;
    padding: 8px 18px;
}

QPushButton#sendButton[filled="true"]:hover {
    background-color: {{accentHover}};
}

QPushButton#sendButton[filled="true"]:disabled {
    background-color: {{accentDisabledBg}};
    color: #FFFFFF;
}

QPushButton#sendButton[filled="true"][arrowStyle="true"] {
    padding: 6px 16px;
}

QPushButton#sendButton[filled="true"][planeStyle="true"] {
    padding: 8px 10px;
}

/* --- Menus ---------------------------------------------------------------- */
QMenu {
    background-color: {{surface}};
    border: 1px solid {{border}};
    border-radius: 8px;
    padding: 4px;
}

QMenu::item {
    border-radius: 4px;
    padding: 6px 12px;
}

QMenu::item:selected {
    background-color: {{menuHoverBg}};
}

#deleteMenuItem {
    color: {{danger}};
    font-weight: 500;
    background-color: transparent;
    padding: 4px 8px;
}

/* --- Dialogs / danger button ----------------------------------------------- */
QPushButton#dangerButton {
    background-color: {{danger}};
    color: #FFFFFF;
    border: none;
    border-radius: 6px;
    padding: 6px 16px;
    font-weight: 600;
}

QPushButton#dangerButton:hover {
    background-color: {{dangerHover}};
}
)");
}

} // namespace

namespace {

// Pulled out of styleSheet() so loadThemedIcon() can look up a single token
// (e.g. "secondaryText") without duplicating either table — both light and
// dark stay defined in exactly one place.
const QHash<QString, QString> &tokenTable(bool dark)
{
    static const QHash<QString, QString> lightTokens{
        {"bg", "#FAF9F5"},
        {"text", "#262523"},
        {"secondaryText", "#857F72"},
        {"sidebarBg", "#EFEBE1"},
        {"border", "#E1DCCE"},
        {"itemSelectedBg", "#DED5C2"},
        {"itemHoverBg", "#E5DFD1"},
        {"menuButtonHoverBg", "#D6CCB6"},
        {"hairline", "#EEEAE0"},
        {"surface", "#FFFFFF"},
        {"userBubbleBg", "#F0E9DA"},
        {"assistantBubbleBg", "transparent"},
        {"errorBg", "#FBEAEA"},
        {"errorBorder", "#E9B8B4"},
        {"errorText", "#B3261E"},
        {"progressTrack", "#EEEAE0"},
        {"accent", "#CC7A5C"},
        {"accentHover", "#B8684B"},
        {"accentDisabledBg", "#E1D3C9"},
        {"menuHoverBg", "#F3F0E8"},
        {"danger", "#D93025"},
        {"dangerHover", "#B7291D"},
        // Same value in both themes on purpose: for icons/text sitting on
        // a filled accent-colored surface (e.g. the send button's icon),
        // which needs to stay high-contrast regardless of light/dark mode
        // — kept as a real token rather than a hardcoded hex so it's still
        // themeable in one place if that ever needs to change.
        {"onAccent", "#FFFFFF"},
    };

    static const QHash<QString, QString> darkTokens{
        {"bg", "#262524"},
        {"text", "#ECE8E0"},
        {"secondaryText", "#A39C8E"},
        {"sidebarBg", "#1E1D1B"},
        {"border", "#3A3733"},
        {"itemSelectedBg", "#3A3733"},
        {"itemHoverBg", "#332F2B"},
        {"menuButtonHoverBg", "#423D37"},
        {"hairline", "#3A3733"},
        {"surface", "#2C2A27"},
        {"userBubbleBg", "#3A3733"},
        {"assistantBubbleBg", "transparent"},
        {"errorBg", "#3A2422"},
        {"errorBorder", "#5C3330"},
        {"errorText", "#FF8A80"},
        {"progressTrack", "#3A3733"},
        {"accent", "#D97757"},
        {"accentHover", "#C2683F"},
        {"accentDisabledBg", "#4A4038"},
        {"menuHoverBg", "#332F2B"},
        {"danger", "#E5534B"},
        {"dangerHover", "#C23D35"},
        {"onAccent", "#FFFFFF"},
    };

    return dark ? darkTokens : lightTokens;
}

} // namespace

QString Theme::styleSheet(bool dark)
{
    QHash<QString, QString> tokens = tokenTable(dark); // copy — the custom-accent substitution below is per-call, not baked into the shared table

    const QString customAccent = QSettings().value("appearance/accentColor").toString();
    if (!customAccent.isEmpty()) {
        const QColor accent(customAccent);
        tokens["accent"] = accent.name();
        tokens["accentHover"] = (dark ? accent.lighter(115) : accent.darker(115)).name();

        // Blended toward the theme's own surface color (rather than just
        // adjusted lightness) so the muted "disabled" look stays plausible
        // regardless of the picked hue.
        const QColor surface(tokens.value("surface"));
        auto blend = [](const QColor &a, const QColor &b, double t) {
            return QColor(int(a.red() * (1 - t) + b.red() * t),
                          int(a.green() * (1 - t) + b.green() * t),
                          int(a.blue() * (1 - t) + b.blue() * t));
        };
        tokens["accentDisabledBg"] = blend(accent, surface, 0.6).name();
    }

    return applyTokens(templateQss(), tokens);
}

QString Theme::colorToken(const QString &tokenName, bool dark)
{
    // "accent" specifically has to go through currentAccentColor() rather
    // than the raw table — styleSheet() already substitutes {{accent}} with
    // the custom "appearance/accentColor" override (if any) when building
    // the app's QSS, but loadThemedIcon() calls this directly to recolor
    // SVGs, bypassing that substitution entirely. Without this, any icon
    // colored via the "accent" token (the send button's plane icon,
    // web-search/thinking toggle icons, etc.) would silently ignore a
    // custom Application color and always show the theme's built-in default.
    if (tokenName == QLatin1String("accent"))
        return currentAccentColor(dark);
    return tokenTable(dark).value(tokenName);
}

QString Theme::currentAccentColor(bool dark)
{
    const QString stored = QSettings().value("appearance/accentColor").toString();
    return stored.isEmpty() ? tokenTable(dark).value("accent") : stored;
}

QIcon Theme::loadThemedIcon(const QString &resourcePath, bool dark, int sizePx, const QString &colorTokenName)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon();

    QString svg = QString::fromUtf8(file.readAll());
    svg.replace(QStringLiteral("{{iconColor}}"), colorToken(colorTokenName, dark));

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid())
        return QIcon();

    QPixmap pixmap(sizePx, sizePx);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return QIcon(pixmap);
}

QIcon Theme::loadThemedIconMultiSize(const QString &resourcePath, bool dark, const QVector<int> &sizesPx,
                                      const QString &colorTokenName)
{
    QIcon icon;
    for (int sizePx : sizesPx) {
        const QIcon single = loadThemedIcon(resourcePath, dark, sizePx, colorTokenName);
        if (!single.isNull())
            icon.addPixmap(single.pixmap(sizePx, sizePx));
    }
    return icon;
}
