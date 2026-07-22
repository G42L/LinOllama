#include "SettingsDialog.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QSettings>
#include <QColorDialog>
#include <QColor>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QRadioButton>
#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QToolButton>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QSignalBlocker>
#include <QTimer>
#include <QCoreApplication>
#include <QTabWidget>
#include <limits>
#include <QScrollArea>
#include <QLineEdit>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QLinearGradient>
#include <QPolygon>
#include <QPen>
#include <QStringList>
#include <iterator>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QFile>

namespace {
// Every color the "Theme color" combo (see the Appearance tab) treats as
// part of one coordinated palette — the Application accent plus all five
// stats meters. `backup` is where makeColorPickerRow() remembers a manual
// pick for that swatch, independent of `live` (which a preset/Default
// choice from the combo can freely overwrite) — see onThemeColorPresetChanged()
// and refreshThemeColorCombo(). Index 0 (accent) is treated as special in a
// couple of places below, so keep it first if this list is ever reordered.
struct ThemedColorKey { const char *live; const char *backup; };
const QVector<ThemedColorKey> kThemedColorKeys = {
    {"appearance/accentColor", "appearance/customAccentColor"},
    {"stats/cpuColor",         "appearance/customCpuColor"},
    {"stats/ramColor",         "appearance/customRamColor"},
    {"stats/gpuColor",         "appearance/customGpuColor"},
    {"stats/vramColor",        "appearance/customVramColor"},
    {"stats/micColor",         "appearance/customMicColor"},
};

// Empty when settingsKey isn't one of kThemedColorKeys at all (e.g. some
// future unrelated color picker reusing makeColorPickerRow()).
QString themedColorBackupKey(const QString &settingsKey)
{
    for (const ThemedColorKey &k : kThemedColorKeys) {
        if (settingsKey == QLatin1String(k.live))
            return QString(QLatin1String(k.backup));
    }
    return QString();
}

// "Rainbow" is the one theme choice that isn't a single color applied
// everywhere — each of kThemedColorKeys gets its own distinct hue instead
// (a spread across the visible spectrum), parallel array indexed exactly
// like kThemedColorKeys: [accent, cpu, ram, gpu, vram, mic]. Kept as its
// own array (rather than folded into ThemedColorKey) since every *other*
// theme choice only ever needs one color, not six.
const char *kRainbowColors[] = {
    "#8b5cf6", // accent — violet
    "#e5484d", // cpu — red
    "#f2994a", // ram — orange
    "#e8b93b", // gpu — gold (a pure yellow reads poorly against light surfaces)
    "#2fa968", // vram — green
    "#3b82f6", // mic — blue
};

// Color-harmony presets: like Rainbow, each of kThemedColorKeys gets its own
// hue, but algorithmically derived from the *current* accent's hue (see
// onThemeColorPresetChanged()) via a standard hue-rotation offset list,
// rather than one fixed hardcoded table. A scheme with fewer than 6 offsets
// (every one of these) cycles — offsets[i % offsetCount] — to fill all 6
// slots, per index order [accent, cpu, ram, gpu, vram, mic].
struct HarmonyScheme {
    const char *name;
    const char *dataKey;
    int offsets[5];
    int offsetCount;
};
const HarmonyScheme kHarmonySchemes[] = {
    {"Analogous",            "__harmony_analogous__",   {0, 30, 60, -30, -60}, 5},
    {"Complementary",        "__harmony_complement__",  {0, 180},              2},
    {"Split-Complementary",  "__harmony_split__",       {0, 150, 210},         3},
    {"Triadic",              "__harmony_triadic__",     {0, 120, 240},         3},
    {"Tetradic",             "__harmony_tetradic__",    {0, 60, 180, 240},     4},
    {"Square",               "__harmony_square__",      {0, 90, 180, 270},     4},
};

// Fixed vividness for every harmony-generated color, so results stay
// consistently readable regardless of how saturated/light the seed accent
// happens to be (QColor::fromHsl's s/l are 0–255, not 0–100).
constexpr int kHarmonySaturation = 170;
constexpr int kHarmonyLightness = 140;
// Used when the current accent can't meaningfully carry a hue (e.g.
// unset/default, or the adaptive-gray "Contrast" preset, which is desaturated
// by design) — a warm coral-ish hue in the same spirit as the app's own
// default accent, so a harmony picked right after "Contrast" still produces 6
// distinct colors instead of 6 identical grays.
constexpr int kHarmonyFallbackHue = 15;

// Where onThemeColorPresetChanged() remembers which harmony was last applied
// and exactly what it produced, so refreshThemeColorCombo() can show that
// scheme's name again instead of falling back to "Custom…" — see the
// harmony-application loop's own comment for why a snapshot is needed here
// (unlike Rainbow, which just re-checks its own fixed table).
const char *kLastHarmonyKeyKey = "appearance/lastHarmonyKey";
const char *kLastHarmonyColorsKey = "appearance/lastHarmonyColorsSnapshot";

// Builds a small swatch icon from a handful of color stops (a left-to-right
// gradient) — shared by "Rainbow" and every harmony preset below instead of
// duplicating the QPainter/QLinearGradient boilerplate once per entry.
QIcon makeGradientSwatch(const QVector<QColor> &stops)
{
    QPixmap swatch(16, 16);
    QPainter painter(&swatch);
    QLinearGradient gradient(0, 0, 16, 0);
    const int n = stops.size();
    for (int i = 0; i < n; ++i)
        gradient.setColorAt(n > 1 ? double(i) / (n - 1) : 0.0, stops[i]);
    painter.fillRect(swatch.rect(), gradient);
    painter.end();
    return QIcon(swatch);
}

// "Contrast" (adaptive grayscale accent — see Theme::adaptiveAccentSentinel())
// gets its own swatch style rather than makeGradientSwatch(): a hard
// diagonal black/white split with a thin border, since a plain white square
// would disappear against the combo's own background.
QIcon makeAdaptiveSwatch()
{
    QPixmap swatch(16, 16);
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#1E1D1B"));
    painter.drawRect(swatch.rect());
    painter.setBrush(QColor("#FFFFFF"));
    const QPolygon triangle({QPoint(16, 0), QPoint(16, 16), QPoint(0, 16)});
    painter.drawPolygon(triangle);
    painter.setPen(QPen(QColor("#8A8578"), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
    painter.end();
    return QIcon(swatch);
}
}

SettingsDialog::SettingsDialog(ThemeManager *themeManager, OllamaClient *ollamaClient,
                                ConversationStore *conversationStore,
                                WhisperManager *whisperManager, QWidget *parent)
    : QDialog(parent)
    , m_themeManager(themeManager)
    , m_ollamaClient(ollamaClient)
    , m_conversationStore(conversationStore)
    , m_whisperManager(whisperManager)
{
    setWindowTitle("Settings");
    setMinimumWidth(420);
    // Comfortably tall enough for the fullest single tab (Whisper, with its
    // model table) without the dialog needing to grow when switching tabs —
    // each tab only holds a fraction of what used to be stacked in one long
    // scroll, so this is noticeably shorter than before tabs existed.
    setMinimumHeight(560);

    auto *layout = new QVBoxLayout(this);

    auto *tabs = new QTabWidget;
    layout->addWidget(tabs, /*stretch=*/1);

    auto *appearancePage = new QWidget;
    auto *appearancePageLayout = new QVBoxLayout(appearancePage);
    auto *formattingPage = new QWidget;
    auto *formattingPageLayout = new QVBoxLayout(formattingPage);
    auto *inputsPage = new QWidget;
    auto *inputsPageLayout = new QVBoxLayout(inputsPage);
    auto *ollamaPage = new QWidget;
    auto *ollamaPageLayout = new QVBoxLayout(ollamaPage);
    auto *whisperPage = new QWidget;
    auto *whisperPageLayout = new QVBoxLayout(whisperPage);
    auto *dataPage = new QWidget;
    auto *dataPageLayout = new QVBoxLayout(dataPage);

    tabs->addTab(appearancePage, "Appearance");
    tabs->addTab(formattingPage, "Formatting");
    tabs->addTab(inputsPage, "Inputs");
    // Ollama has grown into the tallest tab by far (model management,
    // generation parameters, server environment, pull/delete) — a
    // QScrollArea rather than sub-tabs keeps every section visible in one
    // place (still searchable-by-eye top to bottom) instead of hiding half
    // of it behind another layer of navigation.
    //
    // Whisper and Data get the same treatment for a different reason: at a
    // larger "appearance/fontScale", every label/button in them grows too,
    // and the dialog itself doesn't grow past setMinimumHeight() to match —
    // without a scroll area, that overflow squeezed widgets into less
    // height than their wrapped text actually needed, showing as
    // overlapping lines rather than a clipped/scrollable page.
    auto makeScrollablePage = [](QWidget *page) {
        auto *scrollArea = new QScrollArea;
        scrollArea->setWidget(page);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        // Scrolling itself (wheel, drag) still works — this only hides the
        // bar's own track/handle, matching the same sleek look already used
        // for the main chat message list (see ChatWidget's m_scrollArea).
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return scrollArea;
    };
    tabs->addTab(makeScrollablePage(ollamaPage), "Ollama");
    tabs->addTab(makeScrollablePage(whisperPage), "Whisper");
    tabs->addTab(makeScrollablePage(dataPage), "Data");

    // --- Appearance tab ----------------------------------------------------
    // Wrapped in a bordered QGroupBox (rather than a bare QLabel heading)
    // so this section is unmistakably visible as its own block, not just
    // relying on font-weight to separate it from what's below.
    auto *appearanceGroup = new QGroupBox("Appearance");
    auto *appearanceLayout = new QFormLayout(appearanceGroup);

    m_themeCombo = new QComboBox;
    m_themeCombo->addItem("Light", static_cast<int>(ThemeManager::Mode::Light));
    m_themeCombo->addItem("Dark", static_cast<int>(ThemeManager::Mode::Dark));
    m_themeCombo->addItem("Auto (match system)", static_cast<int>(ThemeManager::Mode::Auto));

    const int currentIdx = m_themeCombo->findData(static_cast<int>(m_themeManager->mode()));
    if (currentIdx >= 0)
        m_themeCombo->setCurrentIndex(currentIdx);

    connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onThemeComboChanged);
    appearanceLayout->addRow("Theme", m_themeCombo);

    // Scales every "font-size: Npx" declaration in Theme::styleSheet() (see
    // applyFontScale() there) — the whole app's text, not just the chat
    // bubbles. Applied live via notifyAppearanceChanged() the same way an
    // accent color change is, so there's no need to reopen Settings or
    // restart to see the effect.
    auto *fontScaleRow = new QWidget;
    auto *fontScaleRowLayout = new QHBoxLayout(fontScaleRow);
    fontScaleRowLayout->setContentsMargins(0, 0, 0, 0);

    // Discrete 0.25x steps (0.5x .. 2.0x) rather than a continuous range —
    // the slider's own range is just the step *index* (0..6), which is
    // what actually guarantees only these values are ever reachable by
    // dragging (a continuous range with a "single step" hint doesn't stop a
    // drag from landing in between). The spinbox can still have an
    // in-between value typed into it directly; the sync below snaps that to
    // the nearest step on the same round trip. Capped at 2.0x (was 4.0x) —
    // past that, message bubbles started visibly cropping text, a layout-
    // timing issue in AutoHeightTextBrowser's height calculation that's out
    // of scope to fully chase down right now.
    static const QVector<double> kFontScaleSteps = {
        0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0,
    };
    auto indexForFontScale = [](double scale) {
        int bestIndex = 0;
        double bestDiff = std::numeric_limits<double>::max();
        for (int i = 0; i < kFontScaleSteps.size(); ++i) {
            const double diff = qAbs(kFontScaleSteps[i] - scale);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestIndex = i;
            }
        }
        return bestIndex;
    };

    auto *fontScaleSlider = new QSlider(Qt::Horizontal);
    fontScaleSlider->setObjectName("fontScaleSlider");
    fontScaleSlider->setRange(0, kFontScaleSteps.size() - 1);
    fontScaleSlider->setTickPosition(QSlider::TicksBelow);
    fontScaleSlider->setTickInterval(1);
    fontScaleRowLayout->addWidget(fontScaleSlider, /*stretch=*/1);

    auto *fontScaleSpinBox = new QDoubleSpinBox;
    fontScaleSpinBox->setRange(0.5, 2.0);
    fontScaleSpinBox->setSingleStep(0.25);
    fontScaleSpinBox->setDecimals(2);
    fontScaleSpinBox->setSuffix("x");
    fontScaleRowLayout->addWidget(fontScaleSpinBox);

    // Same reset glyph/size/tooltip as makeColorPickerRow()'s reset button —
    // just sets the spinbox back to 1.0x, which cascades through the sync
    // connections below to reset the slider, persist, and re-render live,
    // same as picking 1.0x by hand.
    auto *fontScaleResetButton = new QPushButton(QString::fromUtf8("\xE2\x86\xBA")); // U+21BA ANTICLOCKWISE OPEN CIRCLE ARROW
    fontScaleResetButton->setFixedSize(24, 20);
    fontScaleResetButton->setCursor(Qt::PointingHandCursor);
    fontScaleResetButton->setToolTip("Reset to default (1.0x)");
    fontScaleRowLayout->addWidget(fontScaleResetButton);
    connect(fontScaleResetButton, &QPushButton::clicked, fontScaleSpinBox, [fontScaleSpinBox]() {
        fontScaleSpinBox->setValue(1.0);
    });

    const double storedFontScale = QSettings().value("appearance/fontScale", 1.0).toDouble();
    const int storedFontScaleIndex = indexForFontScale(storedFontScale);
    fontScaleSlider->setValue(storedFontScaleIndex);
    fontScaleSpinBox->setValue(kFontScaleSteps[storedFontScaleIndex]);

    // Kept in sync with each other the same way as the other slider/spinbox
    // pairs in this dialog, just via a step-index<->value lookup instead of
    // a simple int<->double conversion.
    connect(fontScaleSlider, &QSlider::valueChanged, fontScaleSpinBox, [fontScaleSpinBox](int index) {
        fontScaleSpinBox->setValue(kFontScaleSteps[index]);
    });
    connect(fontScaleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            fontScaleSlider, [fontScaleSlider, indexForFontScale](double value) {
        fontScaleSlider->setValue(indexForFontScale(value));
    });
    connect(fontScaleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        QSettings().setValue("appearance/fontScale", value);
        m_themeManager->notifyAppearanceChanged();
        emit formattingSettingsChanged();
    });

    appearanceLayout->addRow("Font size", fontScaleRow);

    m_sendButtonStyleCombo = new QComboBox;
    m_sendButtonStyleCombo->addItem("Paper plane icon", "plane");
    m_sendButtonStyleCombo->addItem(QString::fromUtf8("Arrow icon (\xE2\x86\x91)"), "arrow");
    m_sendButtonStyleCombo->addItem("Text label (\"Send\")", "text");

    const QString savedStyle = QSettings().value("chat/sendButtonStyle", "plane").toString();
    const int sendStyleIdx = m_sendButtonStyleCombo->findData(savedStyle);
    if (sendStyleIdx >= 0)
        m_sendButtonStyleCombo->setCurrentIndex(sendStyleIdx);

    connect(m_sendButtonStyleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onSendButtonStyleComboChanged);
    appearanceLayout->addRow("Send button", m_sendButtonStyleCombo);

    m_sendButtonFilledCheck = new QCheckBox("Filled send button (classic look)");
    const bool sendButtonFilled = QSettings().value("chat/sendButtonFilled", false).toBool();
    m_sendButtonFilledCheck->setChecked(sendButtonFilled);
    connect(m_sendButtonFilledCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onSendButtonFilledToggled);
    appearanceLayout->addRow(QString(), m_sendButtonFilledCheck);

    auto *colorsHeading = new QLabel("Colors");
    colorsHeading->setStyleSheet("font-weight: 600; margin-top: 6px;");
    appearanceLayout->addRow(colorsHeading);

    // Quick-pick presets for "appearance/accentColor" — curated for
    // contrast/vibrancy against both themes' neutral surfaces rather than
    // pulled from any particular brand palette. Each gets a small color
    // swatch icon in the combo itself so picking one is a visual choice,
    // not a guess from a name. "Default" clears the override (tracks
    // the active theme's own built-in accent); "Custom…" changes nothing
    // by itself — it's what the combo shows once you've picked an exact
    // color via the swatch below that doesn't happen to match a preset.
    struct ThemeColorPreset { const char *name; const char *hex; };
    static const ThemeColorPreset kThemeColorPresets[] = {
        {"Coral",   "#e8674a"},
        {"Amber",   "#e8a23b"},
        {"Emerald", "#21a366"},
        {"Teal",    "#14b8a6"},
        {"Sky",     "#3b9de8"},
        {"Indigo",  "#5b6ee8"},
        {"Violet",  "#8b5cf6"},
        {"Rose",    "#e0457b"},
    };

    m_themeColorCombo = new QComboBox;
    m_themeColorCombo->addItem("Default", QString());
    for (const ThemeColorPreset &preset : kThemeColorPresets) {
        QPixmap swatch(16, 16);
        swatch.fill(QColor(preset.hex));
        m_themeColorCombo->addItem(QIcon(swatch), preset.name, QString(preset.hex));
    }
    // "Contrast" — adaptive grayscale (near-black in Light, near-white in
    // Dark; see Theme::adaptiveAccentSentinel()), rather than a fixed hex
    // like the presets above, since a literal white accent would vanish
    // against the light theme's own near-white surfaces.
    m_themeColorCombo->addItem(makeAdaptiveSwatch(), "Contrast", Theme::adaptiveAccentSentinel());
    // Color-harmony presets: each derives its hues from the current accent
    // (see onThemeColorPresetChanged()) rather than a fixed table, so the
    // swatch here is just illustrative (a representative seed hue), not
    // exactly what applying it will produce.
    for (const HarmonyScheme &scheme : kHarmonySchemes) {
        QVector<QColor> preview;
        for (int i = 0; i < scheme.offsetCount; ++i) {
            const int hue = ((160 + scheme.offsets[i]) % 360 + 360) % 360;
            preview.append(QColor::fromHsl(hue, kHarmonySaturation, kHarmonyLightness));
        }
        m_themeColorCombo->addItem(makeGradientSwatch(preview), scheme.name, QString(scheme.dataKey));
    }
    {
        // A gradient swatch across all six kRainbowColors, rather than a
        // solid fill like the presets above — visually signals up front
        // that this one behaves differently (colors *everything*
        // differently, not everything the same).
        QVector<QColor> rainbowStops;
        for (const char *hex : kRainbowColors)
            rainbowStops.append(QColor(hex));
        m_themeColorCombo->addItem(makeGradientSwatch(rainbowStops), "Rainbow", QStringLiteral("__rainbow__"));
    }
    m_themeColorCombo->addItem(QStringLiteral("Custom…"), QStringLiteral("__custom__"));
    connect(m_themeColorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onThemeColorPresetChanged);
    appearanceLayout->addRow("Theme color", m_themeColorCombo);

    // "Application" drives the whole app's accent (focus borders, the
    // send/stop button, progress-bar fills, etc. — see Theme::styleSheet())
    // — not just the stats panel. Each row's own reset button (rather than
    // one shared "reset all") lets any single color be reverted on its own.
    // Still here alongside the quick-pick combo above for exact/arbitrary
    // colors the 8 presets don't cover.
    appearanceLayout->addRow("Application", makeColorPickerRow("appearance/accentColor"));
    refreshThemeColorCombo(); // matches the combo to whatever's actually stored, now that both it and the swatch above exist

    // The four stats-meter colors share one row (a borderless, headerless
    // 4-column strip — own small label above each swatch — rather than one
    // QFormLayout row per meter) purely to save vertical space; each
    // column's swatch/reset pair is still the same makeColorPickerRow()
    // widget used everywhere else.
    auto *statsColorsRow = new QWidget;
    auto *statsColorsLayout = new QHBoxLayout(statsColorsRow);
    statsColorsLayout->setContentsMargins(0, 0, 0, 0);
    statsColorsLayout->setSpacing(16);

    auto addStatsColorColumn = [this, statsColorsLayout](const QString &label, const QString &settingsKey) {
        auto *column = new QWidget;
        auto *columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(2);

        auto *nameLabel = new QLabel(label);
        nameLabel->setAlignment(Qt::AlignHCenter);
        nameLabel->setObjectName("settingsHintLabel");
        columnLayout->addWidget(nameLabel, 0, Qt::AlignHCenter);
        columnLayout->addWidget(makeColorPickerRow(settingsKey), 0, Qt::AlignHCenter);

        // Equal stretch factor (not a fixed sizeHint width) is what makes
        // all four columns share the row's full width evenly, rather than
        // each just claiming its own contents' width with the leftover
        // space dumped into one trailing stretch.
        statsColorsLayout->addWidget(column, /*stretch=*/1);
    };
    addStatsColorColumn("CPU", "stats/cpuColor");
    addStatsColorColumn("RAM", "stats/ramColor");
    addStatsColorColumn("GPU", "stats/gpuColor");
    addStatsColorColumn("VRAM", "stats/vramColor");
    addStatsColorColumn("Mic", "stats/micColor");

    appearanceLayout->addRow(statsColorsRow);

    appearancePageLayout->addWidget(appearanceGroup);
    appearancePageLayout->addStretch();

    // --- Formatting tab ------------------------------------------------------
    // Controls AutoHeightTextBrowser::applyBlockSpacing() — how much vertical
    // room a rendered reply's paragraphs, list items, and headings get.
    // Qt's own Markdown-to-block-format conversion collapses all of these to
    // 0, which reads as cramped for anything with more than a line or two of
    // structure, so this dialog exposes them directly rather than baking in
    // one fixed guess.
    auto *spacingGroup = new QGroupBox("Message spacing");
    auto *spacingLayout = new QVBoxLayout(spacingGroup);

    spacingLayout->addWidget(makeSpacingSliderRow(
        "Paragraph spacing", "formatting/paragraphSpacing", 0, 40, 8));
    spacingLayout->addWidget(makeSpacingSliderRow(
        "List item spacing", "formatting/listItemSpacing", 0, 40, 4));
    spacingLayout->addWidget(makeSpacingSliderRow(
        "Heading spacing (before)", "formatting/headingSpacingBefore", 0, 60, 18));

    formattingPageLayout->addWidget(spacingGroup);

    // Controls the Copy icon shown above a syntax-highlighted code block
    // (see AutoHeightTextBrowser::convertMarkdownWithCodeBlocks()) — a fixed
    // size like the spacing settings above, not scaled by the font-size
    // slider, since it's an icon rather than text.
    auto *codeBlockGroup = new QGroupBox("Code blocks");
    auto *codeBlockLayout = new QVBoxLayout(codeBlockGroup);
    codeBlockLayout->addWidget(makeSpacingSliderRow(
        "Copy icon size", "formatting/copyIconSize", 12, 32, 16));
    formattingPageLayout->addWidget(codeBlockGroup);

    formattingPageLayout->addStretch();

    // --- Ollama tab: Model ---------------------------------------------------
    auto *modelGroup = new QGroupBox("Model");
    auto *modelLayout = new QVBoxLayout(modelGroup);

    m_useCustomContextLengthCheck = new QCheckBox("Use custom context length");
    const bool customCtxEnabled = QSettings().value("chat/useCustomContextLength", false).toBool();
    m_useCustomContextLengthCheck->setChecked(customCtxEnabled);
    connect(m_useCustomContextLengthCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onContextLengthEnabledToggled);
    modelLayout->addWidget(m_useCustomContextLengthCheck);

    auto *contextLengthHint = new QLabel(
        "Unchecked, Ollama picks its own default context length (its OLLAMA_CONTEXT_LENGTH env "
        "var, or an automatic VRAM-based default). There's no \"unlimited\" option — every model "
        "has a hard maximum context length that Ollama enforces regardless of this setting.");
    contextLengthHint->setWordWrap(true);
    contextLengthHint->setObjectName("settingsHintLabel");
    modelLayout->addWidget(contextLengthHint);

    auto *contextSliderRow = new QHBoxLayout;
    m_contextLengthSlider = new QSlider(Qt::Horizontal);
    m_contextLengthSlider->setObjectName("contextLengthSlider");
    // 262144 (2^18), not a round 256000 — matches the actual context_length
    // several real models report (confirmed against a live Ollama server:
    // gemma4's GGUF metadata reports exactly 262144), so the slider's max
    // lines up with a real achievable ceiling instead of an arbitrary number.
    m_contextLengthSlider->setRange(1, 262144);
    m_contextLengthSlider->setSingleStep(1024);
    m_contextLengthSlider->setPageStep(8192);
    contextSliderRow->addWidget(m_contextLengthSlider, /*stretch=*/1);

    m_contextLengthSpinBox = new QSpinBox;
    m_contextLengthSpinBox->setRange(1, 262144);
    m_contextLengthSpinBox->setSuffix(" tokens");
    contextSliderRow->addWidget(m_contextLengthSpinBox);

    modelLayout->addLayout(contextSliderRow);

    const int storedContextLength = QSettings().value("chat/customContextLength", 8192).toInt();
    m_contextLengthSlider->setValue(storedContextLength);
    m_contextLengthSpinBox->setValue(storedContextLength);
    m_contextLengthSlider->setEnabled(customCtxEnabled);
    m_contextLengthSpinBox->setEnabled(customCtxEnabled);

    // Kept in sync with each other (Qt no-ops setValue() when the value
    // hasn't actually changed, so this can't loop) — persistence/emit only
    // needs to happen once, off the spinbox's own valueChanged.
    connect(m_contextLengthSlider, &QSlider::valueChanged, m_contextLengthSpinBox, &QSpinBox::setValue);
    connect(m_contextLengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            m_contextLengthSlider, &QSlider::setValue);
    connect(m_contextLengthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::onContextLengthValueChanged);

    m_modelOptimizationCheck = new QCheckBox("Queing model optimization");
    const bool modelOptimizationEnabled = QSettings().value("chat/modelOptimization", false).toBool();
    m_modelOptimizationCheck->setChecked(modelOptimizationEnabled);
    connect(m_modelOptimizationCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onModelOptimizationToggled);
    modelLayout->addWidget(m_modelOptimizationCheck);

    auto *modelOptimizationHint = new QLabel(
        "Ollama only generates for one model at a time, so switching between chats that use "
        "different models queues them up rather than sending everything at once. Off (default), "
        "chats are processed strictly in the order you send them, even if that means reloading a "
        "model repeatedly. On, chats waiting on the model that's already loaded may jump ahead of "
        "others to reduce how often models get swapped.");
    modelOptimizationHint->setWordWrap(true);
    modelOptimizationHint->setObjectName("settingsHintLabel");
    // Real QWidget margins, not QSS "padding" — a stylesheet's padding on a
    // word-wrapped QLabel isn't reliably folded into its own heightForWidth()
    // calculation (that's what caused the clipped top/bottom lines before),
    // whereas setContentsMargins() always is, since it's native Qt layout
    // geometry rather than something QStyleSheetStyle has to intercept.
    modelOptimizationHint->setContentsMargins(0, 4, 0, 4);
    modelLayout->addWidget(modelOptimizationHint);

    ollamaPageLayout->addWidget(modelGroup);

    // --- Ollama tab: generation parameters ----------------------------------
    auto *genParamsGroup = new QGroupBox("Generation parameters");
    auto *genParamsLayout = new QVBoxLayout(genParamsGroup);

    m_useCustomGenParamsCheck = new QCheckBox("Use custom generation parameters");
    const bool useCustomGenParams = QSettings().value("chat/useCustomGenParams", false).toBool();
    m_useCustomGenParamsCheck->setChecked(useCustomGenParams);
    connect(m_useCustomGenParamsCheck, &QCheckBox::toggled, this, &SettingsDialog::onGenParamsToggled);
    genParamsLayout->addWidget(m_useCustomGenParamsCheck);

    auto *genParamsHint = new QLabel(
        "Unchecked, Ollama uses its own built-in defaults (shown below) exactly as if none of this "
        "existed. The fields below are pre-filled with those same defaults, so checking this and "
        "leaving everything untouched changes nothing — only fields you actually edit affect anything.");
    genParamsHint->setWordWrap(true);
    genParamsHint->setObjectName("settingsHintLabel");
    genParamsLayout->addWidget(genParamsHint);

    auto *genParamsForm = new QFormLayout;

    m_temperatureSpin = new QDoubleSpinBox;
    m_temperatureSpin->setRange(0.0, 2.0);
    m_temperatureSpin->setSingleStep(0.05);
    m_temperatureSpin->setDecimals(2);
    m_temperatureSpin->setValue(QSettings().value("chat/temperature", 0.8).toDouble());
    connect(m_temperatureSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SettingsDialog::onTemperatureChanged);
    genParamsForm->addRow("Temperature", m_temperatureSpin);

    m_topPSpin = new QDoubleSpinBox;
    m_topPSpin->setRange(0.0, 1.0);
    m_topPSpin->setSingleStep(0.05);
    m_topPSpin->setDecimals(2);
    m_topPSpin->setValue(QSettings().value("chat/topP", 0.9).toDouble());
    connect(m_topPSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SettingsDialog::onTopPChanged);
    genParamsForm->addRow("Top P", m_topPSpin);

    m_topKSpin = new QSpinBox;
    m_topKSpin->setRange(0, 200);
    m_topKSpin->setValue(QSettings().value("chat/topK", 40).toInt());
    connect(m_topKSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onTopKChanged);
    genParamsForm->addRow("Top K", m_topKSpin);

    m_seedSpin = new QSpinBox;
    m_seedSpin->setRange(0, 2147483647);
    m_seedSpin->setSpecialValueText("Random"); // at 0, the minimum — see GenerationOptions' own comment on the convention
    m_seedSpin->setValue(QSettings().value("chat/seed", 0).toInt());
    connect(m_seedSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSeedChanged);
    genParamsForm->addRow("Seed", m_seedSpin);

    m_numPredictSpin = new QSpinBox;
    m_numPredictSpin->setRange(-1, 128000);
    m_numPredictSpin->setSpecialValueText("No limit"); // at -1, the minimum
    m_numPredictSpin->setSuffix(" tokens");
    m_numPredictSpin->setValue(QSettings().value("chat/numPredict", -1).toInt());
    connect(m_numPredictSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::onNumPredictChanged);
    genParamsForm->addRow("Max tokens", m_numPredictSpin);

    m_repeatPenaltySpin = new QDoubleSpinBox;
    m_repeatPenaltySpin->setRange(0.0, 2.0);
    m_repeatPenaltySpin->setSingleStep(0.05);
    m_repeatPenaltySpin->setDecimals(2);
    m_repeatPenaltySpin->setValue(QSettings().value("chat/repeatPenalty", 1.1).toDouble());
    connect(m_repeatPenaltySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SettingsDialog::onRepeatPenaltyChanged);
    genParamsForm->addRow("Repeat penalty", m_repeatPenaltySpin);

    m_stopSequencesEdit = new QLineEdit(QSettings().value("chat/stopSequences").toString());
    m_stopSequencesEdit->setPlaceholderText("Comma-separated, e.g. ###, </s>");
    connect(m_stopSequencesEdit, &QLineEdit::editingFinished, this, &SettingsDialog::onStopSequencesEdited);
    genParamsForm->addRow("Stop sequences", m_stopSequencesEdit);

    genParamsLayout->addLayout(genParamsForm);

    const auto setGenParamsRowEnabled = [this]() {
        const bool enabled = m_useCustomGenParamsCheck->isChecked();
        m_temperatureSpin->setEnabled(enabled);
        m_topPSpin->setEnabled(enabled);
        m_topKSpin->setEnabled(enabled);
        m_seedSpin->setEnabled(enabled);
        m_numPredictSpin->setEnabled(enabled);
        m_repeatPenaltySpin->setEnabled(enabled);
        m_stopSequencesEdit->setEnabled(enabled);
    };
    setGenParamsRowEnabled();
    connect(m_useCustomGenParamsCheck, &QCheckBox::toggled, this, [setGenParamsRowEnabled]() { setGenParamsRowEnabled(); });

    ollamaPageLayout->addWidget(genParamsGroup);

    // --- Ollama tab: server environment -------------------------------------
    auto *envGroup = new QGroupBox("Server environment");
    auto *envLayout = new QFormLayout(envGroup);

    auto *envHint = new QLabel(
        "Applied the next time Ollama's server is (re)started via the tray menu's Start/Stop — not "
        "for an already-running server. Takes effect for a systemd user service (a drop-in override "
        "is written and reloaded automatically) or a plain process this app starts directly. A "
        "systemd system service isn't modified automatically here; set these in its own unit file "
        "instead. Leave a field at its default to omit that variable entirely, same as never having "
        "set it.");
    envHint->setWordWrap(true);
    envHint->setObjectName("settingsHintLabel");
    envLayout->addRow(envHint);

    auto *modelsPathRow = new QHBoxLayout;
    m_ollamaModelsPathEdit = new QLineEdit(QSettings().value("ollamaServer/modelsPath").toString());
    m_ollamaModelsPathEdit->setPlaceholderText("~/.ollama/models (default)");
    connect(m_ollamaModelsPathEdit, &QLineEdit::editingFinished,
            this, &SettingsDialog::onOllamaModelsPathEdited);
    modelsPathRow->addWidget(m_ollamaModelsPathEdit, /*stretch=*/1);
    auto *modelsPathBrowseButton = new QPushButton("Browse…");
    connect(modelsPathBrowseButton, &QPushButton::clicked,
            this, &SettingsDialog::onBrowseOllamaModelsPathClicked);
    modelsPathRow->addWidget(modelsPathBrowseButton);
    envLayout->addRow("OLLAMA_MODELS", modelsPathRow);

    m_ollamaKeepAliveEdit = new QLineEdit(QSettings().value("ollamaServer/keepAlive").toString());
    m_ollamaKeepAliveEdit->setPlaceholderText("5m (default)");
    connect(m_ollamaKeepAliveEdit, &QLineEdit::editingFinished,
            this, &SettingsDialog::onOllamaKeepAliveEdited);
    envLayout->addRow("OLLAMA_KEEP_ALIVE", m_ollamaKeepAliveEdit);

    m_ollamaFlashAttentionCheck = new QCheckBox("Enabled");
    m_ollamaFlashAttentionCheck->setChecked(QSettings().value("ollamaServer/flashAttention", false).toBool());
    connect(m_ollamaFlashAttentionCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onOllamaFlashAttentionToggled);
    envLayout->addRow("OLLAMA_FLASH_ATTENTION", m_ollamaFlashAttentionCheck);

    m_ollamaNumParallelSpin = new QSpinBox;
    m_ollamaNumParallelSpin->setRange(0, 64);
    m_ollamaNumParallelSpin->setSpecialValueText("Auto (default)"); // 0 == unset, let Ollama decide
    m_ollamaNumParallelSpin->setValue(QSettings().value("ollamaServer/numParallel", 0).toInt());
    connect(m_ollamaNumParallelSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::onOllamaNumParallelChanged);
    envLayout->addRow("OLLAMA_NUM_PARALLEL", m_ollamaNumParallelSpin);

    ollamaPageLayout->addWidget(envGroup);

    // --- Ollama tab: Models (pull new, delete installed) -------------------
    auto *modelsGroup = new QGroupBox("Models");
    auto *modelsGroupLayout = new QVBoxLayout(modelsGroup);

    auto *pullRow = new QHBoxLayout;
    m_pullModelEdit = new QLineEdit;
    m_pullModelEdit->setPlaceholderText("e.g. llama3.2, llama3.2:3b, qwen2.5-coder");
    connect(m_pullModelEdit, &QLineEdit::returnPressed, this, &SettingsDialog::onPullModelClicked);
    pullRow->addWidget(m_pullModelEdit, /*stretch=*/1);
    m_pullButton = new QPushButton("Pull");
    connect(m_pullButton, &QPushButton::clicked, this, &SettingsDialog::onPullModelClicked);
    pullRow->addWidget(m_pullButton);
    m_cancelPullButton = new QPushButton("Cancel");
    m_cancelPullButton->setVisible(false);
    connect(m_cancelPullButton, &QPushButton::clicked, this, &SettingsDialog::onCancelPullClicked);
    pullRow->addWidget(m_cancelPullButton);
    modelsGroupLayout->addLayout(pullRow);

    m_pullStatusLabel = new QLabel;
    m_pullStatusLabel->setWordWrap(true);
    m_pullStatusLabel->setObjectName("settingsHintLabel");
    m_pullStatusLabel->setVisible(false);
    modelsGroupLayout->addWidget(m_pullStatusLabel);

    m_pullProgressBar = new QProgressBar;
    m_pullProgressBar->setRange(0, 100);
    m_pullProgressBar->setTextVisible(false);
    m_pullProgressBar->setFixedHeight(6);
    m_pullProgressBar->setVisible(false);
    modelsGroupLayout->addWidget(m_pullProgressBar);

    auto *installedHeading = new QLabel("Installed");
    installedHeading->setStyleSheet("font-weight: 600; margin-top: 6px;");
    modelsGroupLayout->addWidget(installedHeading);

    m_installedModelsLayout = new QVBoxLayout;
    m_installedModelsLayout->setSpacing(4);
    modelsGroupLayout->addLayout(m_installedModelsLayout);

    m_installedModelsStatusLabel = new QLabel("Loading…");
    m_installedModelsStatusLabel->setStyleSheet("opacity: 0.6; font-weight: normal;");
    m_installedModelsLayout->addWidget(m_installedModelsStatusLabel);

    ollamaPageLayout->addWidget(modelsGroup);

    connect(m_ollamaClient, &OllamaClient::modelsListed, this, &SettingsDialog::onInstalledModelsListed);
    connect(m_ollamaClient, &OllamaClient::modelPullProgress, this, &SettingsDialog::onModelPullProgress);
    connect(m_ollamaClient, &OllamaClient::modelPullFinished, this, &SettingsDialog::onModelPullFinished);
    connect(m_ollamaClient, &OllamaClient::modelDeleted, this, &SettingsDialog::onModelDeleted);
    m_ollamaClient->refreshStatus(); // forces an immediate /api/tags rather than waiting for main.cpp's periodic poll

    ollamaPageLayout->addStretch();

    // --- Inputs tab: Microphone ----------------------------------------------
    auto *micGroup = new QGroupBox("Microphone");
    auto *micGroupLayout = new QVBoxLayout(micGroup);

    auto *micDeviceRow = new QHBoxLayout;
    micDeviceRow->addWidget(new QLabel("Audio input"));
    m_audioInputCombo = new QComboBox;
    micDeviceRow->addWidget(m_audioInputCombo, /*stretch=*/1);

    auto *micRefreshButton = new QPushButton("Refresh");
    connect(micRefreshButton, &QPushButton::clicked, this, &SettingsDialog::refreshAudioInputCombo);
    micDeviceRow->addWidget(micRefreshButton);
    micGroupLayout->addLayout(micDeviceRow);

    refreshAudioInputCombo();
    connect(m_audioInputCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onAudioInputComboChanged);

    auto *meterSmoothingRow = new QHBoxLayout;
    meterSmoothingRow->addWidget(new QLabel("Meter smoothing"));
    m_meterSmoothingSlider = new QSlider(Qt::Horizontal);
    m_meterSmoothingSlider->setObjectName("meterSmoothingSlider"); // picks up Theme.cpp's accent-colored handle/fill, same as the context-length slider
    m_meterSmoothingSlider->setRange(0, 100);
    m_meterSmoothingSlider->setValue(QSettings().value("voice/meterSmoothingPercent", 50).toInt());
    meterSmoothingRow->addWidget(m_meterSmoothingSlider, /*stretch=*/1);
    m_meterSmoothingValueLabel = new QLabel;
    m_meterSmoothingValueLabel->setMinimumWidth(56); // stops the row from jittering width as the text changes
    meterSmoothingRow->addWidget(m_meterSmoothingValueLabel);
    micGroupLayout->addLayout(meterSmoothingRow);
    connect(m_meterSmoothingSlider, &QSlider::valueChanged,
            this, &SettingsDialog::onMeterSmoothingSliderChanged);
    // Sets the initial label text (re-persisting the just-loaded value is
    // harmless, and nothing's connected to meterSmoothingChanged() yet at
    // this point in construction anyway).
    onMeterSmoothingSliderChanged(m_meterSmoothingSlider->value());

    auto *meterSmoothingHint = new QLabel(
        "How quickly the live \"Mic\" meter (in the system stats strip) responds to changes in "
        "volume — lower reacts instantly but can look jumpy; higher eases between levels more "
        "gradually, avoiding sharp jumps at the cost of a slight lag. Only affects the meter's "
        "display, not the actual recording.");
    meterSmoothingHint->setWordWrap(true);
    meterSmoothingHint->setObjectName("settingsHintLabel");
    micGroupLayout->addWidget(meterSmoothingHint);

    inputsPageLayout->addWidget(micGroup);
    inputsPageLayout->addStretch();

    // --- Whisper tab: Voice transcription -----------------------------------
    auto *whisperGroup = new QGroupBox("Voice transcription (Whisper)");
    auto *whisperLayout = new QVBoxLayout(whisperGroup);

    m_voiceAutoSendCheck = new QCheckBox("Send automatically after transcription");
    const bool voiceAutoSend = QSettings().value("chat/voiceAutoSend", false).toBool();
    m_voiceAutoSendCheck->setChecked(voiceAutoSend);
    connect(m_voiceAutoSendCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onVoiceAutoSendToggled);
    whisperLayout->addWidget(m_voiceAutoSendCheck);

    auto *voiceAutoSendHint = new QLabel(
        "Off (default): the transcribed text fills the message box so you can review or correct it "
        "before sending. On: it's sent to Ollama immediately, with no chance to fix a "
        "misheard word first.");
    voiceAutoSendHint->setWordWrap(true);
    voiceAutoSendHint->setObjectName("settingsHintLabel");
    whisperLayout->addWidget(voiceAutoSendHint);

    m_liveTranscriptionCheck = new QCheckBox("Enable live transcription (while still talking)");
    const bool liveTranscriptionEnabled = QSettings().value("voice/liveTranscriptionEnabled", false).toBool();
    m_liveTranscriptionCheck->setChecked(liveTranscriptionEnabled);
    connect(m_liveTranscriptionCheck, &QCheckBox::toggled,
            this, &SettingsDialog::onLiveTranscriptionToggled);
    whisperLayout->addWidget(m_liveTranscriptionCheck);

    auto *liveTranscriptionHint = new QLabel(
        "Off (default): the mic button transcribes once, on release, same as above. On: it streams "
        "text into the message box in short chunks as you talk, via a separate whisper-server "
        "process — requires that binary to be built alongside whisper-cli (see the status line "
        "below); has no effect without it.");
    liveTranscriptionHint->setWordWrap(true);
    liveTranscriptionHint->setObjectName("settingsHintLabel");
    whisperLayout->addWidget(liveTranscriptionHint);

    whisperLayout->addSpacing(10); // visual break before the binary/model-location controls below

    m_whisperStatusLabel = new QLabel;
    m_whisperStatusLabel->setWordWrap(true);
    m_whisperStatusLabel->setObjectName("settingsHintLabel");
    whisperLayout->addWidget(m_whisperStatusLabel);

    auto *whisperPathsRow = new QHBoxLayout;
    auto *changeBinaryButton = new QPushButton("Whisper binary…");
    connect(changeBinaryButton, &QPushButton::clicked, this, &SettingsDialog::onChangeWhisperBinaryClicked);
    whisperPathsRow->addWidget(changeBinaryButton);
    auto *changeModelsDirButton = new QPushButton("Models folder…");
    connect(changeModelsDirButton, &QPushButton::clicked, this, &SettingsDialog::onChangeWhisperModelsDirClicked);
    whisperPathsRow->addWidget(changeModelsDirButton);
    auto *changeServerBinaryButton = new QPushButton("Live server binary…");
    changeServerBinaryButton->setToolTip(
        "whisper-server — a separate binary from the same whisper.cpp checkout (built from its "
        "examples/server), used only for live in-progress transcription while you're still "
        "talking. Push-to-talk transcription on release works without it.");
    connect(changeServerBinaryButton, &QPushButton::clicked,
            this, &SettingsDialog::onChangeWhisperServerBinaryClicked);
    whisperPathsRow->addWidget(changeServerBinaryButton);
    whisperPathsRow->addStretch(1);
    whisperLayout->addLayout(whisperPathsRow);

    auto *whisperTableHeaderRow = new QHBoxLayout;
    auto *whisperTableHeading = new QLabel("Models");
    whisperTableHeading->setStyleSheet("font-weight: 600;");
    whisperTableHeaderRow->addWidget(whisperTableHeading);
    whisperTableHeaderRow->addStretch(1);
    m_whisperExpandButton = new QToolButton;
    m_whisperExpandButton->setObjectName("whisperExpandButton");
    m_whisperExpandButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_whisperExpandButton->setIconSize(QSize(14, 14));
    m_whisperExpandButton->setAutoRaise(true);
    m_whisperExpandButton->setCursor(Qt::PointingHandCursor);
    connect(m_whisperExpandButton, &QToolButton::clicked, this, &SettingsDialog::onWhisperExpandToggleClicked);
    whisperTableHeaderRow->addWidget(m_whisperExpandButton);
    whisperLayout->addLayout(whisperTableHeaderRow);

    m_whisperModelsTable = new QTableWidget(0, 0);
    m_whisperModelsTable->verticalHeader()->setVisible(false);
    m_whisperModelsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_whisperModelsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_whisperModelsTable->setShowGrid(false);
    // No vertical scrollbar — the fixed height below already caps visible
    // rows, with the rest reachable by scrolling the whole Settings dialog
    // like everything else in it. Horizontal is "as needed" rather than off:
    // the minimal view's columns always fit, but the expanded view's extra
    // Disk/Memory/Language columns can genuinely overflow a narrow dialog
    // (see rebuildWhisperModelsTable()'s resize-mode handling), and clipping
    // those silently would be worse than a scrollbar appearing.
    m_whisperModelsTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_whisperModelsTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Tall enough to show ~4-5 rows without the dialog itself having to grow
    // to fit all ten catalog entries — the rest scroll internally.
    m_whisperModelsTable->setMinimumHeight(160);
    m_whisperModelsTable->setMaximumHeight(220);
    whisperLayout->addWidget(m_whisperModelsTable);

    auto *whisperHint = new QLabel(
        "Recording is transcribed locally via whisper.cpp — nothing is sent anywhere. Pick which "
        "downloaded model to use with the radio button in the last column; \".en\" models are "
        "English-only but a little faster/more accurate for that language alone. Hover a row for "
        "its full details.");
    whisperHint->setWordWrap(true);
    whisperHint->setObjectName("settingsHintLabel");
    whisperLayout->addWidget(whisperHint);

    whisperPageLayout->addWidget(whisperGroup);
    whisperPageLayout->addStretch();

    // --- Data tab: storage locations + backup/restore -----------------------
    auto *storageGroup = new QGroupBox("Storage locations");
    auto *storageLayout = new QFormLayout(storageGroup);

    auto *storageHint = new QLabel(
        "Every conversation lives as its own JSON file under the folder below; app settings "
        "(theme, server environment, generation defaults, etc.) live in the settings file below "
        "that. Nothing here is normally worth touching by hand — this is just so the files are easy "
        "to find, e.g. to back them up yourself outside the app.");
    storageHint->setWordWrap(true);
    storageHint->setObjectName("settingsHintLabel");
    storageLayout->addRow(storageHint);

    auto *conversationsPathRow = new QHBoxLayout;
    auto *conversationsPathLabel = new QLabel(m_conversationStore->conversationsDirPath());
    conversationsPathLabel->setWordWrap(true);
    conversationsPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    conversationsPathRow->addWidget(conversationsPathLabel, /*stretch=*/1);
    auto *openConversationsFolderButton = new QPushButton("Open Folder");
    connect(openConversationsFolderButton, &QPushButton::clicked,
            this, &SettingsDialog::onOpenConversationsFolderClicked);
    conversationsPathRow->addWidget(openConversationsFolderButton);
    storageLayout->addRow("Conversations", conversationsPathRow);

    auto *settingsPathRow = new QHBoxLayout;
    auto *settingsPathLabel = new QLabel(QSettings().fileName());
    settingsPathLabel->setWordWrap(true);
    settingsPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    settingsPathRow->addWidget(settingsPathLabel, /*stretch=*/1);
    auto *openSettingsFolderButton = new QPushButton("Open Folder");
    connect(openSettingsFolderButton, &QPushButton::clicked,
            this, &SettingsDialog::onOpenSettingsFolderClicked);
    settingsPathRow->addWidget(openSettingsFolderButton);
    storageLayout->addRow("Settings", settingsPathRow);

    dataPageLayout->addWidget(storageGroup);

    auto *backupGroup = new QGroupBox("Backup and Restore");
    auto *backupLayout = new QVBoxLayout(backupGroup);

    auto *conversationsBackupRow = new QHBoxLayout;
    auto *exportConversationsButton = new QPushButton("Export all conversations…");
    connect(exportConversationsButton, &QPushButton::clicked,
            this, &SettingsDialog::onExportAllConversationsClicked);
    conversationsBackupRow->addWidget(exportConversationsButton);
    auto *importConversationsButton = new QPushButton("Import conversations…");
    connect(importConversationsButton, &QPushButton::clicked,
            this, &SettingsDialog::onImportConversationsClicked);
    conversationsBackupRow->addWidget(importConversationsButton);
    backupLayout->addLayout(conversationsBackupRow);

    auto *conversationsBackupHint = new QLabel(
        "Export bundles every conversation into one JSON file. Import always adds conversations as "
        "new entries (fresh ids) — it never overwrites or removes anything already here, so "
        "importing the same backup twice just duplicates it.");
    conversationsBackupHint->setWordWrap(true);
    conversationsBackupHint->setObjectName("settingsHintLabel");
    backupLayout->addWidget(conversationsBackupHint);

    auto *settingsBackupRow = new QHBoxLayout;
    auto *exportSettingsButton = new QPushButton("Export settings…");
    connect(exportSettingsButton, &QPushButton::clicked,
            this, &SettingsDialog::onExportSettingsClicked);
    settingsBackupRow->addWidget(exportSettingsButton);
    auto *importSettingsButton = new QPushButton("Import settings…");
    connect(importSettingsButton, &QPushButton::clicked,
            this, &SettingsDialog::onImportSettingsClicked);
    settingsBackupRow->addWidget(importSettingsButton);
    backupLayout->addLayout(settingsBackupRow);

    auto *settingsBackupHint = new QLabel(
        "Import replaces every current setting with whatever's in the chosen file — restart "
        "LinOllama afterward so everything picks it up.");
    settingsBackupHint->setWordWrap(true);
    settingsBackupHint->setObjectName("settingsHintLabel");
    backupLayout->addWidget(settingsBackupHint);

    dataPageLayout->addWidget(backupGroup);

    auto *clearGroup = new QGroupBox("Clear Data");
    auto *clearLayout = new QVBoxLayout(clearGroup);

    auto *clearHint = new QLabel(
        "Irreversible — consider exporting a backup above first.");
    clearHint->setWordWrap(true);
    clearHint->setObjectName("settingsHintLabel");
    clearLayout->addWidget(clearHint);

    auto *clearRow = new QHBoxLayout;
    auto *clearConfigButton = new QPushButton("Clear Configuration");
    clearConfigButton->setObjectName("dangerButton");
    connect(clearConfigButton, &QPushButton::clicked,
            this, &SettingsDialog::onClearConfigurationClicked);
    clearRow->addWidget(clearConfigButton);
    auto *clearConversationsButton = new QPushButton("Clear Conversations");
    clearConversationsButton->setObjectName("dangerButton");
    connect(clearConversationsButton, &QPushButton::clicked,
            this, &SettingsDialog::onClearConversationsClicked);
    clearRow->addWidget(clearConversationsButton);
    auto *clearAllButton = new QPushButton("Clear All");
    clearAllButton->setObjectName("dangerButton");
    connect(clearAllButton, &QPushButton::clicked,
            this, &SettingsDialog::onClearAllClicked);
    clearRow->addWidget(clearAllButton);
    clearLayout->addLayout(clearRow);

    dataPageLayout->addWidget(clearGroup);
    dataPageLayout->addStretch();

    // --- Offload model — deliberately OUTSIDE the tab widget ---------------
    // Kept visible regardless of which tab is selected, rather than tucked
    // into any one of them — unlike everything else here, this is an action
    // someone might reach for in the middle of using the app (freeing VRAM
    // right before loading a big model, etc.), not a one-time preference.
    auto *offloadGroup = new QGroupBox("Offload model");
    auto *offloadLayout = new QVBoxLayout(offloadGroup);

    auto *offloadHeaderRow = new QHBoxLayout;
    auto *offloadHint = new QLabel(
        "Frees a model's memory/VRAM immediately instead of waiting for its normal idle timeout.");
    offloadHint->setWordWrap(true);
    offloadHint->setObjectName("settingsHintLabel");
    offloadHeaderRow->addWidget(offloadHint, /*stretch=*/1);
    auto *refreshButton = new QPushButton("Refresh");
    connect(refreshButton, &QPushButton::clicked, this, &SettingsDialog::refreshLoadedModels);
    offloadHeaderRow->addWidget(refreshButton);
    offloadLayout->addLayout(offloadHeaderRow);

    m_loadedModelsLayout = new QVBoxLayout;
    m_loadedModelsLayout->setSpacing(4);
    offloadLayout->addLayout(m_loadedModelsLayout);

    m_loadedModelsStatusLabel = new QLabel("Loading…");
    m_loadedModelsStatusLabel->setStyleSheet("opacity: 0.6; font-weight: normal;");
    m_loadedModelsLayout->addWidget(m_loadedModelsStatusLabel);

    layout->addWidget(offloadGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);

    connect(m_ollamaClient, &OllamaClient::loadedModelsListed,
            this, &SettingsDialog::onLoadedModelsListed);
    connect(m_ollamaClient, &OllamaClient::modelUnloaded,
            this, &SettingsDialog::onModelUnloaded);

    refreshLoadedModels();

    if (m_whisperManager) {
        connect(m_whisperManager, &WhisperManager::modelsChanged,
                this, &SettingsDialog::onWhisperModelsChanged);
        connect(m_whisperManager, &WhisperManager::downloadProgress,
                this, &SettingsDialog::onWhisperDownloadProgress);
        connect(m_whisperManager, &WhisperManager::downloadFinished,
                this, &SettingsDialog::onWhisperDownloadFinished);
    }
    refreshWhisperStatusLabel();
    updateWhisperExpandIcon();
    rebuildWhisperModelsTable();

    // Word-wrapped QLabels (the hint text under each checkbox/slider) can
    // report a stale, too-small heightForWidth() on the very first layout
    // pass — before the dialog has actually been resized to its real
    // width — which is what read as their last line being clipped. Forcing
    // one more layout pass now, after every widget/width is already known,
    // fixes that for good rather than needing a hand-tuned margin per label.
    layout->activate();
    adjustSize();
}

void SettingsDialog::onThemeComboChanged(int index)
{
    const auto mode = static_cast<ThemeManager::Mode>(m_themeCombo->itemData(index).toInt());
    m_themeManager->setMode(mode);
}

void SettingsDialog::onThemeColorPresetChanged(int index)
{
    const QString data = m_themeColorCombo->itemData(index).toString();

    for (const HarmonyScheme &scheme : kHarmonySchemes) {
        if (data != QLatin1String(scheme.dataKey))
            continue;

        // Seed hue comes from whatever accent is in effect right now — if it
        // can't meaningfully carry a hue (unset/default, or the desaturated
        // adaptive-gray "Contrast" preset), fall back to a fixed hue so this
        // still produces 6 distinct colors instead of 6 identical grays.
        const bool dark = m_themeManager->isDarkActive();
        const QColor seed(Theme::currentAccentColor(dark));
        const int seedHue = seed.hslSaturationF() > 0.15 ? seed.hslHue() : kHarmonyFallbackHue;

        // Harmonies (unlike Rainbow) have no fixed color table to re-detect
        // against later — they're derived from whatever the seed hue was at
        // pick time. So refreshThemeColorCombo() can't recompute "is this
        // still Triadic?" from the hues alone; instead we snapshot exactly
        // what got written here and compare that snapshot verbatim next
        // time, which self-invalidates the moment any one of the 6 colors is
        // hand-edited afterward (see kLastHarmonyColorsKey's own comment).
        QStringList appliedColors;
        for (int i = 0; i < kThemedColorKeys.size(); ++i) {
            const int offset = scheme.offsets[i % scheme.offsetCount];
            const int hue = ((seedHue + offset) % 360 + 360) % 360;
            const QString hex = QColor::fromHsl(hue, kHarmonySaturation, kHarmonyLightness).name();
            QSettings().setValue(kThemedColorKeys[i].live, hex);
            appliedColors << hex;
        }
        QSettings().setValue(kLastHarmonyKeyKey, QString(scheme.dataKey));
        QSettings().setValue(kLastHarmonyColorsKey, appliedColors.join(QLatin1Char('|')));
        notifyColorChanged("appearance/accentColor");
        return;
    }

    if (data == QLatin1String("__rainbow__")) {
        // Unlike every other choice here, this sets each of the six colors
        // to its own distinct value rather than one color applied
        // everywhere — see kRainbowColors' own comment. Each swatch's
        // individual backup (see makeColorPickerRow()) is untouched, same
        // as picking any other theme color, so "Custom…" afterward still
        // restores whatever was manually set before this.
        for (int i = 0; i < kThemedColorKeys.size(); ++i)
            QSettings().setValue(kThemedColorKeys[i].live, QString(kRainbowColors[i]));
        notifyColorChanged("appearance/accentColor");
        return;
    }

    if (data == QLatin1String("__custom__")) {
        // Restores whatever was last manually picked for each swatch this
        // combo governs (accent + every stats meter — see makeColorPickerRow()
        // and kThemedColorKeys), not just the accent. Any swatch with no
        // backup yet just gets cleared instead, tracking the accent like
        // it would by default. First time ever (nothing backed up at all),
        // there's nothing to restore, so this opens the picker for the
        // Application color directly instead of silently doing nothing —
        // the rest fall back to tracking it automatically either way.
        bool anyBackup = false;
        for (const ThemedColorKey &k : kThemedColorKeys) {
            if (!QSettings().value(k.backup).toString().isEmpty()) {
                anyBackup = true;
                break;
            }
        }

        if (!anyBackup) {
            const QColor picked = QColorDialog::getColor(
                QColor(currentColorHex("appearance/accentColor")), this, "Choose Color");
            if (!picked.isValid()) {
                refreshThemeColorCombo(); // the combo already flipped to "Custom…"; undo that since nothing was actually picked
                return;
            }
            QSettings().setValue("appearance/accentColor", picked.name());
            QSettings().setValue("appearance/customAccentColor", picked.name());
        } else {
            for (const ThemedColorKey &k : kThemedColorKeys) {
                const QString backup = QSettings().value(k.backup).toString();
                if (!backup.isEmpty())
                    QSettings().setValue(k.live, backup);
                else
                    QSettings().remove(k.live);
            }
        }
        notifyColorChanged("appearance/accentColor");
        return;
    }

    if (data.isEmpty())
        QSettings().remove("appearance/accentColor");
    else
        QSettings().setValue("appearance/accentColor", data);

    // Any individually-customized stats-meter color is cleared here too —
    // without this, a meter customized earlier would keep showing its old
    // color instead of picking up this theme choice, when the whole point
    // is that any theme color applies everywhere at once. Each one's own
    // saved backup (see makeColorPickerRow()) is untouched, so selecting
    // "Custom…" afterward still restores it.
    for (int i = 1; i < kThemedColorKeys.size(); ++i) // starts at 1: index 0 is the accent itself, handled above
        QSettings().remove(kThemedColorKeys[i].live);

    notifyColorChanged("appearance/accentColor");
}

void SettingsDialog::refreshThemeColorCombo()
{
    QSettings settings;

    // Rainbow assigns a different color to each of the six slots, so it
    // needs its own exact-match check first — the "clean preset" logic
    // below assumes a single shared color, which doesn't apply to it.
    bool isRainbow = true;
    for (int i = 0; i < kThemedColorKeys.size(); ++i) {
        if (settings.value(kThemedColorKeys[i].live).toString().toLower()
                != QString::fromLatin1(kRainbowColors[i])) { // kRainbowColors entries are already lowercase
            isRainbow = false;
            break;
        }
    }

    // A stored harmony "survives" only if every one of the 6 colors still
    // exactly matches what was written when it was applied — the moment any
    // one of them is hand-edited afterward, this naturally stops matching
    // and falls through to "Custom…" below, same as it would for Rainbow.
    bool isStoredHarmony = false;
    const QString lastHarmonyKey = settings.value(kLastHarmonyKeyKey).toString();
    if (!lastHarmonyKey.isEmpty()) {
        QStringList currentColors;
        for (int i = 0; i < kThemedColorKeys.size(); ++i)
            currentColors << settings.value(kThemedColorKeys[i].live).toString().toLower();
        isStoredHarmony = currentColors.join(QLatin1Char('|')) == settings.value(kLastHarmonyColorsKey).toString();
    }

    int matchIndex = m_themeColorCombo->count() - 1; // falls back to "Custom…", the last entry

    if (isRainbow) {
        const int rainbowIndex = m_themeColorCombo->findData(QStringLiteral("__rainbow__"));
        if (rainbowIndex >= 0)
            matchIndex = rainbowIndex;
    } else if (isStoredHarmony) {
        const int harmonyIndex = m_themeColorCombo->findData(lastHarmonyKey);
        if (harmonyIndex >= 0)
            matchIndex = harmonyIndex;
    } else {
        const QString accent = settings.value("appearance/accentColor").toString().toLower();

        // A "clean" preset/Default state means every *other* themed color
        // is unset (tracking the accent automatically) — if even one stats
        // meter has its own override, this is a custom palette regardless
        // of what the accent itself happens to be set to.
        bool othersUnset = true;
        for (int i = 1; i < kThemedColorKeys.size(); ++i) {
            if (!settings.value(kThemedColorKeys[i].live).toString().isEmpty()) {
                othersUnset = false;
                break;
            }
        }

        if (othersUnset) {
            if (accent.isEmpty()) {
                matchIndex = 0; // "Default"
            } else {
                for (int i = 1; i < m_themeColorCombo->count() - 1; ++i) {
                    if (m_themeColorCombo->itemData(i).toString().toLower() == accent) {
                        matchIndex = i;
                        break;
                    }
                }
            }
        }
    }

    const QSignalBlocker blocker(m_themeColorCombo);
    m_themeColorCombo->setCurrentIndex(matchIndex);
}

void SettingsDialog::onSendButtonStyleComboChanged(int index)
{
    const QString style = m_sendButtonStyleCombo->itemData(index).toString();
    QSettings().setValue("chat/sendButtonStyle", style);
    emit sendButtonStyleChanged(style);
}

void SettingsDialog::onSendButtonFilledToggled(bool filled)
{
    QSettings().setValue("chat/sendButtonFilled", filled);
    emit sendButtonFilledChanged(filled);
}

void SettingsDialog::onVoiceAutoSendToggled(bool enabled)
{
    QSettings().setValue("chat/voiceAutoSend", enabled);
    emit voiceAutoSendChanged(enabled);
}

void SettingsDialog::onLiveTranscriptionToggled(bool enabled)
{
    // No live-signal round trip to ChatWidget needed — see
    // m_liveTranscriptionCheck's own comment: it's read fresh from
    // QSettings the next time the mic button is pressed, not cached
    // anywhere in the meantime.
    QSettings().setValue("voice/liveTranscriptionEnabled", enabled);
}

void SettingsDialog::onContextLengthEnabledToggled(bool enabled)
{
    QSettings().setValue("chat/useCustomContextLength", enabled);
    m_contextLengthSlider->setEnabled(enabled);
    m_contextLengthSpinBox->setEnabled(enabled);
    emit contextLengthSettingChanged();
}

void SettingsDialog::onContextLengthValueChanged(int value)
{
    QSettings().setValue("chat/customContextLength", value);
    emit contextLengthSettingChanged();
}

void SettingsDialog::onModelOptimizationToggled(bool enabled)
{
    QSettings().setValue("chat/modelOptimization", enabled);
    emit modelOptimizationChanged(enabled);
}

QString SettingsDialog::currentColorHex(const QString &settingsKey) const
{
    const QString stored = QSettings().value(settingsKey).toString();
    const bool dark = m_themeManager->isDarkActive();

    // The adaptive-gray "Contrast" sentinel (see Theme::adaptiveAccentSentinel())
    // isn't a real hex value — resolve it to the color it actually renders as,
    // otherwise this swatch would show an invalid "background-color:
    // __adaptive_gray__" instead of the near-black/near-white it means.
    if (stored == Theme::adaptiveAccentSentinel())
        return Theme::currentAccentColor(dark);

    if (!stored.isEmpty())
        return stored;

    // The Application picker itself falls back to the theme's hardcoded
    // default (not currentAccentColor(), which would just reference its own
    // still-unset value) — every other color falls back to the *effective*
    // accent, so its swatch matches what StatsStripWidget actually renders.
    if (settingsKey == QLatin1String("appearance/accentColor"))
        return Theme::colorToken("accent", dark);
    return Theme::currentAccentColor(dark);
}

void SettingsDialog::refreshColorSwatch(QPushButton *button, const QString &settingsKey)
{
    button->setStyleSheet(QString(
        "background-color: %1; border: 1px solid palette(mid); border-radius: 4px;")
        .arg(currentColorHex(settingsKey)));
}

void SettingsDialog::notifyColorChanged(const QString &changedKey)
{
    // Affects the whole app's stylesheet (borders, buttons, progress fills
    // — see Theme::styleSheet()), not just the stats panel, so this needs
    // the full re-apply-and-notify path rather than just statsColorsChanged().
    if (changedKey == QLatin1String("appearance/accentColor"))
        m_themeManager->notifyAppearanceChanged();

    // A change to *any* of the six colors the "Theme color" combo governs
    // (not just the accent) can flip it to/from "Custom…" — e.g. manually
    // editing just the GPU meter's color while a preset is otherwise active.
    if (!themedColorBackupKey(changedKey).isEmpty())
        refreshThemeColorCombo();

    // Any swatch still on "default" visually tracks whichever color just
    // changed (the effective accent), so all of them need refreshing, not
    // just the one that was actually edited.
    for (const auto &entry : m_colorSwatchButtons)
        refreshColorSwatch(entry.first, entry.second);

    emit statsColorsChanged();
}

QWidget *SettingsDialog::makeColorPickerRow(const QString &settingsKey)
{
    auto *row = new QWidget;
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    auto *swatchButton = new QPushButton;
    swatchButton->setFixedSize(32, 20);
    swatchButton->setCursor(Qt::PointingHandCursor);
    swatchButton->setToolTip("Click to choose a color");
    refreshColorSwatch(swatchButton, settingsKey);
    rowLayout->addWidget(swatchButton);

    auto *resetButton = new QPushButton(QString::fromUtf8("\xE2\x86\xBA")); // U+21BA ANTICLOCKWISE OPEN CIRCLE ARROW — plain glyph, not an emoji
    resetButton->setFixedSize(24, 20);
    resetButton->setCursor(Qt::PointingHandCursor);
    resetButton->setToolTip("Reset to default");
    rowLayout->addWidget(resetButton);
    rowLayout->addStretch(1);

    connect(swatchButton, &QPushButton::clicked, this, [this, swatchButton, settingsKey]() {
        const QColor picked = QColorDialog::getColor(
            QColor(currentColorHex(settingsKey)), this, "Choose Color");
        if (!picked.isValid())
            return;
        QSettings().setValue(settingsKey, picked.name());
        // Remembered separately from the active color itself, so picking a
        // theme color afterward (see onThemeColorPresetChanged()) doesn't
        // lose it — selecting "Custom…" again later restores exactly this,
        // for every swatch this backs (accent + every stats meter), not
        // just the one just edited.
        const QString backupKey = themedColorBackupKey(settingsKey);
        if (!backupKey.isEmpty())
            QSettings().setValue(backupKey, picked.name());
        notifyColorChanged(settingsKey);
    });
    connect(resetButton, &QPushButton::clicked, this, [this, settingsKey]() {
        QSettings().remove(settingsKey);
        notifyColorChanged(settingsKey);
    });

    m_colorSwatchButtons.append({swatchButton, settingsKey});
    return row;
}

QWidget *SettingsDialog::makeSpacingSliderRow(const QString &labelText, const QString &settingsKey,
                                              int minVal, int maxVal, int defaultValue)
{
    auto *row = new QWidget;
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    auto *label = new QLabel(labelText);
    rowLayout->addWidget(label, /*stretch=*/1);

    auto *slider = new QSlider(Qt::Horizontal);
    // Gives each of these sliders its own themed groove/handle (see
    // Theme.cpp's "Settings sliders" rule, which styles every objectName
    // ending in "Slider" the same way the accent-colored progress bars are)
    // instead of the plain OS-default look the context-length/meter-
    // smoothing sliders had before that rule existed.
    slider->setObjectName(settingsKey.section(QLatin1Char('/'), -1) + "Slider");
    slider->setRange(minVal, maxVal);
    rowLayout->addWidget(slider, /*stretch=*/2);

    auto *spinBox = new QSpinBox;
    spinBox->setRange(minVal, maxVal);
    spinBox->setSuffix(" px");
    rowLayout->addWidget(spinBox);

    const int storedValue = QSettings().value(settingsKey, defaultValue).toInt();
    slider->setValue(storedValue);
    spinBox->setValue(storedValue);

    // Kept in sync with each other the same way as the context-length
    // slider/spinbox pair — persistence only needs to happen once, off the
    // spinbox's own valueChanged, since Qt no-ops setValue() when the value
    // hasn't actually changed (so this can't loop).
    connect(slider, &QSlider::valueChanged, spinBox, &QSpinBox::setValue);
    connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    connect(spinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, settingsKey](int value) {
        QSettings().setValue(settingsKey, value);
        emit formattingSettingsChanged();
    });

    return row;
}

void SettingsDialog::refreshLoadedModels()
{
    clearLoadedModelsList();
    m_loadedModelsStatusLabel = new QLabel("Loading…");
    m_loadedModelsStatusLabel->setStyleSheet("opacity: 0.6; font-weight: normal;");
    m_loadedModelsLayout->addWidget(m_loadedModelsStatusLabel);

    m_ollamaClient->fetchLoadedModels();
}

void SettingsDialog::onLoadedModelsListed(const QVector<LoadedModelInfo> &models)
{
    rebuildLoadedModelsList(models);
}

void SettingsDialog::onModelUnloaded(const QString &model, bool success)
{
    Q_UNUSED(model);
    Q_UNUSED(success);
    // Whatever just changed, the loaded-model list is now stale either way —
    // simplest correct behavior is just to re-fetch it. Ollama's /api/ps can
    // still report the model as loaded for a brief moment after the unload
    // request's HTTP response comes back, so an immediate re-fetch here
    // would just redraw the stale state; give the server a moment to
    // actually evict it first.
    QTimer::singleShot(500, this, &SettingsDialog::refreshLoadedModels);
}

void SettingsDialog::clearLoadedModelsList()
{
    QLayoutItem *item;
    while ((item = m_loadedModelsLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_loadedModelsStatusLabel = nullptr;

    // refreshLoadedModels()/rebuildLoadedModelsList() add the new row(s) or
    // status label immediately after calling this — deleteLater() above
    // only *schedules* destruction, so without this flush, a previous row's
    // "Offload" button (still alive, just no longer in the layout) would
    // keep showing up alongside the new content until the event loop
    // eventually got around to the deferred deletion — the same "ghost
    // widget" bug ChatWidget::clearMessages() had for chat bubbles.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void SettingsDialog::rebuildLoadedModelsList(const QVector<LoadedModelInfo> &models)
{
    clearLoadedModelsList();

    if (models.isEmpty()) {
        m_loadedModelsStatusLabel = new QLabel("No models currently loaded.");
        m_loadedModelsStatusLabel->setStyleSheet("opacity: 0.6; font-weight: normal;");
        m_loadedModelsLayout->addWidget(m_loadedModelsStatusLabel);
        return;
    }

    for (const LoadedModelInfo &info : models) {
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        const QString sizeText = info.sizeVramBytes > 0
            ? QString(" — %1 GB VRAM").arg(info.sizeVramBytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1)
            : QString();
        auto *nameLabel = new QLabel(info.name + sizeText);
        nameLabel->setStyleSheet("font-weight: normal;");
        rowLayout->addWidget(nameLabel, /*stretch=*/1);

        auto *offloadButton = new QPushButton("Offload");
        connect(offloadButton, &QPushButton::clicked, this, [this, name = info.name]() {
            m_ollamaClient->unloadModel(name);
        });
        rowLayout->addWidget(offloadButton);

        m_loadedModelsLayout->addWidget(row);
    }
}

void SettingsDialog::refreshWhisperStatusLabel()
{
    if (!m_whisperManager)
        return;

    // The checkbox itself stays checkable regardless — its setting persists
    // even while temporarily grayed out, so it's still there, already
    // ticked, the moment a server binary does become available (no need to
    // remember to come back and re-enable it).
    const bool serverAvailable = m_whisperManager->isServerBinaryAvailable();
    m_liveTranscriptionCheck->setEnabled(serverAvailable);

    if (!m_whisperManager->isBinaryAvailable()) {
        m_whisperStatusLabel->setText(
            "whisper-cli not found — build whisper.cpp or point at an existing binary below.");
        return;
    }

    const QString modelsDir = m_whisperManager->modelsDir().isEmpty()
        ? "(not set yet — pick one, or download a model to create a default)"
        : m_whisperManager->modelsDir();
    const QString liveStatus = serverAvailable
        ? QString("found (%1)").arg(m_whisperManager->serverBinaryPath())
        : "not found (see \"Enable live transcription\" above)";
    m_whisperStatusLabel->setText(
        QString("Binary: %1\nModels folder: %2\nLive server (whisper-server): %3")
            .arg(m_whisperManager->binaryPath(), modelsDir, liveStatus));
}

void SettingsDialog::rebuildWhisperModelsTable()
{
    if (!m_whisperManager)
        return;

    m_whisperModelsTable->setRowCount(0);
    m_whisperProgressBars.clear();

    const QStringList installed = m_whisperManager->installedModels();
    const QString selected = m_whisperManager->selectedModel();
    const QVector<WhisperModelInfo> catalog = WhisperManager::catalog();

    // Minimal (default) view keeps just the columns someone picking a model
    // actually decides on; disk size/memory/language/usage — everything
    // else in the catalog — move into each row's tooltip instead. Expanded
    // shows every column at once, tooltip included, for anyone who wants it
    // all visible without hovering.
    const QStringList headers = m_whisperTableExpanded
        ? QStringList{"Model", "Disk", "Memory", "Language", "Speed", "Accuracy", ""}
        : QStringList{"Model", "Speed", "Accuracy", ""};
    m_whisperModelsTable->setColumnCount(headers.size());
    m_whisperModelsTable->setHorizontalHeaderLabels(headers);
    const int actionColumn = headers.size() - 1;
    if (m_whisperTableExpanded) {
        // Every column sized to its own contents, with none stretched to
        // soak up leftover width — the point is to let the row's true total
        // width exceed the viewport when it doesn't fit, so the horizontal
        // scrollbar set up above actually has something to do instead of
        // Stretch quietly shrinking column 0 to make everything fit anyway.
        for (int column = 0; column < headers.size(); ++column)
            m_whisperModelsTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    } else {
        // Minimal view's few columns comfortably fit any reasonable dialog
        // width, so column 0 stretching to fill it looks better than a
        // sliver of empty space after Accuracy/action.
        m_whisperModelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_whisperModelsTable->horizontalHeader()->setSectionResizeMode(actionColumn, QHeaderView::ResizeToContents);
    }

    auto *selectionGroup = new QButtonGroup(m_whisperModelsTable);

    for (const WhisperModelInfo &info : catalog) {
        const int row = m_whisperModelsTable->rowCount();
        m_whisperModelsTable->insertRow(row);

        const QString tooltip = QString(
            "Disk: %1\nMemory: ~%2\nLanguage: %3\nUsage: %4")
            .arg(info.diskSize, info.memEstimate, info.language, info.usage);

        auto setPlainCell = [this, row, &tooltip](int column, const QString &text, bool center = true) {
            auto *item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (center)
                item->setTextAlignment(Qt::AlignCenter);
            item->setToolTip(tooltip);
            m_whisperModelsTable->setItem(row, column, item);
        };

        int column = 0;
        setPlainCell(column++, info.id, false);
        if (m_whisperTableExpanded) {
            setPlainCell(column++, info.diskSize);
            setPlainCell(column++, info.memEstimate);
            setPlainCell(column++, info.language);
        }
        setPlainCell(column++, info.speed);
        setPlainCell(column++, info.accuracy);

        const bool isInstalled = installed.contains(info.id);
        const bool isDownloading = m_whisperManager->isDownloading(info.id);

        auto *actionCell = new QWidget;
        actionCell->setToolTip(tooltip);
        auto *actionLayout = new QHBoxLayout(actionCell);
        actionLayout->setContentsMargins(4, 0, 4, 0);

        if (isDownloading) {
            auto *progress = new QProgressBar;
            progress->setRange(0, 0); // indeterminate until the first progress signal gives a real total
            progress->setFixedWidth(80);
            actionLayout->addWidget(progress);
            m_whisperProgressBars.insert(info.id, progress);
        } else if (isInstalled) {
            auto *radio = new QRadioButton("Use");
            radio->setChecked(info.id == selected);
            selectionGroup->addButton(radio);
            connect(radio, &QRadioButton::toggled, this, [this, id = info.id](bool checked) {
                if (checked)
                    m_whisperManager->setSelectedModel(id);
            });
            actionLayout->addWidget(radio);
        } else {
            auto *downloadButton = new QPushButton("Download");
            connect(downloadButton, &QPushButton::clicked, this, [this, id = info.id]() {
                m_whisperManager->downloadModel(id);
                rebuildWhisperModelsTable();
            });
            actionLayout->addWidget(downloadButton);
        }

        m_whisperModelsTable->setCellWidget(row, actionColumn, actionCell);
    }
}

void SettingsDialog::updateWhisperExpandIcon()
{
    const bool dark = m_themeManager && m_themeManager->isDarkActive();
    m_whisperExpandButton->setIcon(Theme::loadThemedIcon(
        m_whisperTableExpanded ? ":/icons/contract.svg" : ":/icons/expand.svg", dark, 14, "secondaryText"));
    m_whisperExpandButton->setToolTip(m_whisperTableExpanded ? "Show fewer columns" : "Show all columns");
}

void SettingsDialog::onWhisperExpandToggleClicked()
{
    m_whisperTableExpanded = !m_whisperTableExpanded;
    updateWhisperExpandIcon();
    rebuildWhisperModelsTable();
}

void SettingsDialog::onWhisperModelsChanged()
{
    refreshWhisperStatusLabel();
    rebuildWhisperModelsTable();
}

void SettingsDialog::onWhisperDownloadProgress(const QString &modelId, qint64 received, qint64 total)
{
    QProgressBar *bar = m_whisperProgressBars.value(modelId);
    if (!bar)
        return;
    if (total > 0) {
        bar->setRange(0, 100);
        bar->setValue(static_cast<int>((received * 100) / total));
    }
}

void SettingsDialog::onWhisperDownloadFinished(const QString &modelId, bool success, const QString &error)
{
    if (!success) {
        m_whisperStatusLabel->setText("Download of \"" + modelId + "\" failed: " + error);
        rebuildWhisperModelsTable();
    }
    // On success, modelsChanged() (emitted by WhisperManager right after
    // this) already triggers onWhisperModelsChanged(), which rebuilds the
    // table — no separate rebuild needed here to avoid doing it twice.
}

void SettingsDialog::onChangeWhisperBinaryClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Select whisper-cli binary", QDir::homePath());
    if (path.isEmpty())
        return;
    m_whisperManager->setBinaryPath(path);
    refreshWhisperStatusLabel();
    rebuildWhisperModelsTable();
}

void SettingsDialog::onChangeWhisperModelsDirClicked()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, "Select Whisper models folder", QDir::homePath());
    if (path.isEmpty())
        return;
    m_whisperManager->setModelsDir(path);
    refreshWhisperStatusLabel();
    rebuildWhisperModelsTable();
}

void SettingsDialog::onChangeWhisperServerBinaryClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Select whisper-server binary", QDir::homePath());
    if (path.isEmpty())
        return;
    m_whisperManager->setServerBinaryPath(path);
    refreshWhisperStatusLabel();
}

void SettingsDialog::refreshAudioInputCombo()
{
    const QByteArray savedId = QSettings().value("voice/audioInputDeviceId").toByteArray();

    // Blocked while repopulating — clearing and re-adding items fires
    // currentIndexChanged() for every step otherwise, which would each
    // persist a spurious selection before the real (saved) one is restored
    // below.
    const QSignalBlocker blocker(m_audioInputCombo);

    m_audioInputCombo->clear();
    m_audioInputCombo->addItem("System default", QByteArray());

    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    int matchIndex = 0;
    for (const QAudioDevice &device : devices) {
        m_audioInputCombo->addItem(device.description(), device.id());
        if (!savedId.isEmpty() && device.id() == savedId)
            matchIndex = m_audioInputCombo->count() - 1;
    }

    m_audioInputCombo->setCurrentIndex(matchIndex);
}

void SettingsDialog::onAudioInputComboChanged(int index)
{
    const QByteArray deviceId = m_audioInputCombo->itemData(index).toByteArray();
    if (deviceId.isEmpty())
        QSettings().remove("voice/audioInputDeviceId"); // "System default"
    else
        QSettings().setValue("voice/audioInputDeviceId", deviceId);
    emit audioInputDeviceChanged();
}

void SettingsDialog::onMeterSmoothingSliderChanged(int value)
{
    QSettings().setValue("voice/meterSmoothingPercent", value);

    QString text;
    if (value < 40)
        text = "Sharper";
    else if (value > 60)
        text = "Smoother";
    else
        text = "Default";
    m_meterSmoothingValueLabel->setText(text);

    emit meterSmoothingChanged();
}

void SettingsDialog::onOllamaModelsPathEdited()
{
    const QString path = m_ollamaModelsPathEdit->text().trimmed();
    if (path.isEmpty())
        QSettings().remove("ollamaServer/modelsPath");
    else
        QSettings().setValue("ollamaServer/modelsPath", path);
}

void SettingsDialog::onBrowseOllamaModelsPathClicked()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, "Select Ollama models folder", QDir::homePath());
    if (path.isEmpty())
        return;
    m_ollamaModelsPathEdit->setText(path);
    QSettings().setValue("ollamaServer/modelsPath", path);
}

void SettingsDialog::onOllamaKeepAliveEdited()
{
    const QString value = m_ollamaKeepAliveEdit->text().trimmed();
    if (value.isEmpty())
        QSettings().remove("ollamaServer/keepAlive");
    else
        QSettings().setValue("ollamaServer/keepAlive", value);
}

void SettingsDialog::onOllamaFlashAttentionToggled(bool enabled)
{
    QSettings().setValue("ollamaServer/flashAttention", enabled);
}

void SettingsDialog::onOllamaNumParallelChanged(int value)
{
    // 0 (the spin box's "Auto (default)" special value) means "unset" —
    // stored as 0 either way, ServerController::configuredEnvironmentOverrides()
    // is what actually treats <= 0 as "omit this variable."
    QSettings().setValue("ollamaServer/numParallel", value);
}

void SettingsDialog::onPullModelClicked()
{
    const QString model = m_pullModelEdit->text().trimmed();
    if (model.isEmpty() || !m_pullingModel.isEmpty())
        return; // already pulling something from this dialog — see m_pullingModel's own comment

    m_pullingModel = model;
    m_pullButton->setEnabled(false);
    m_pullModelEdit->setEnabled(false);
    m_cancelPullButton->setVisible(true);
    m_pullStatusLabel->setVisible(true);
    m_pullStatusLabel->setText(QString("Pulling \"%1\"…").arg(model));
    m_pullProgressBar->setVisible(true);
    m_pullProgressBar->setRange(0, 0); // indeterminate until the first progress line gives a real total

    m_ollamaClient->pullModel(model);
}

void SettingsDialog::onCancelPullClicked()
{
    if (m_pullingModel.isEmpty())
        return;
    m_ollamaClient->cancelPull(m_pullingModel);
    // No UI reset here — cancelPull() still ends the stream, so
    // onModelPullFinished() (with a "cancelled" flavor of error, or none at
    // all) fires either way and is the single place that resets the UI,
    // rather than duplicating that reset in two places.
}

void SettingsDialog::onModelPullProgress(const QString &model, const QString &status, qint64 completed, qint64 total)
{
    if (model != m_pullingModel)
        return; // a pull this dialog isn't tracking (shouldn't normally happen, but cheap to guard)

    if (total > 0) {
        m_pullProgressBar->setRange(0, 100);
        m_pullProgressBar->setValue(static_cast<int>((completed * 100) / total));
        const double completedMB = completed / (1024.0 * 1024.0);
        const double totalMB = total / (1024.0 * 1024.0);
        m_pullStatusLabel->setText(QString("%1 — %2 / %3 MB")
            .arg(status).arg(completedMB, 0, 'f', 1).arg(totalMB, 0, 'f', 1));
    } else {
        m_pullProgressBar->setRange(0, 0); // between layers, or a non-download status line ("verifying...", "writing manifest") — nothing byte-based to show
        m_pullStatusLabel->setText(status);
    }
}

void SettingsDialog::onModelPullFinished(const QString &model, bool success, const QString &error)
{
    if (model != m_pullingModel)
        return;

    m_pullStatusLabel->setText(success
        ? QString("\"%1\" pulled successfully.").arg(model)
        : QString("Pulling \"%1\" failed: %2").arg(model, error.isEmpty() ? "cancelled" : error));
    m_pullProgressBar->setVisible(false);
    m_cancelPullButton->setVisible(false);
    m_pullButton->setEnabled(true);
    m_pullModelEdit->setEnabled(true);
    if (success)
        m_pullModelEdit->clear();
    m_pullingModel.clear();

    if (success)
        m_ollamaClient->refreshStatus(); // picks up the newly-installed model in the list below
}

void SettingsDialog::onModelDeleted(const QString &model, bool success, const QString &error)
{
    if (!success) {
        // Reuses the pull section's status label rather than adding a
        // dedicated one — it's already a generic "something happened with
        // models" status line, just below the pull controls.
        m_pullStatusLabel->setVisible(true);
        m_pullStatusLabel->setText(QString("Deleting \"%1\" failed: %2").arg(model, error));
    }
    m_ollamaClient->refreshStatus(); // re-fetches the list either way — a successful delete needs the removal reflected, a failed one is just a no-op refresh
}

void SettingsDialog::onInstalledModelsListed(const QStringList &modelNames)
{
    rebuildInstalledModelsList(modelNames);
}

void SettingsDialog::clearInstalledModelsList()
{
    QLayoutItem *item;
    while ((item = m_installedModelsLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_installedModelsStatusLabel = nullptr;

    // Same reasoning as clearLoadedModelsList() — flush immediately so a
    // previous row doesn't linger as a "ghost" widget alongside the new one.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void SettingsDialog::rebuildInstalledModelsList(const QStringList &modelNames)
{
    clearInstalledModelsList();

    if (modelNames.isEmpty()) {
        m_installedModelsStatusLabel = new QLabel("No models installed.");
        m_installedModelsStatusLabel->setStyleSheet("opacity: 0.6; font-weight: normal;");
        m_installedModelsLayout->addWidget(m_installedModelsStatusLabel);
        return;
    }

    for (const QString &name : modelNames) {
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        auto *nameLabel = new QLabel(name);
        nameLabel->setStyleSheet("font-weight: normal;");
        rowLayout->addWidget(nameLabel, /*stretch=*/1);

        auto *deleteButton = new QPushButton("Delete");
        deleteButton->setObjectName("dangerButton"); // red styling, same as the conversation-delete confirmation elsewhere
        connect(deleteButton, &QPushButton::clicked, this, [this, name]() {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle("Delete model");
            box.setText(QString("Delete \"%1\"?").arg(name));
            box.setInformativeText("This removes it from disk. You'll need to pull it again to use it.");
            QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
            QPushButton *confirmButton = box.addButton("Delete", QMessageBox::DestructiveRole);
            confirmButton->setObjectName("dangerButton");
            box.setDefaultButton(cancelButton);
            box.exec();
            if (box.clickedButton() != confirmButton)
                return;
            m_ollamaClient->deleteModel(name);
        });
        rowLayout->addWidget(deleteButton);

        m_installedModelsLayout->addWidget(row);
    }
}

void SettingsDialog::onGenParamsToggled(bool enabled)
{
    QSettings().setValue("chat/useCustomGenParams", enabled);
}

void SettingsDialog::onTemperatureChanged(double value)
{
    QSettings().setValue("chat/temperature", value);
}

void SettingsDialog::onTopPChanged(double value)
{
    QSettings().setValue("chat/topP", value);
}

void SettingsDialog::onTopKChanged(int value)
{
    QSettings().setValue("chat/topK", value);
}

void SettingsDialog::onSeedChanged(int value)
{
    QSettings().setValue("chat/seed", value);
}

void SettingsDialog::onNumPredictChanged(int value)
{
    QSettings().setValue("chat/numPredict", value);
}

void SettingsDialog::onRepeatPenaltyChanged(double value)
{
    QSettings().setValue("chat/repeatPenalty", value);
}

void SettingsDialog::onStopSequencesEdited()
{
    QSettings().setValue("chat/stopSequences", m_stopSequencesEdit->text().trimmed());
}

void SettingsDialog::onOpenConversationsFolderClicked()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_conversationStore->conversationsDirPath()));
}

void SettingsDialog::onOpenSettingsFolderClicked()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QSettings().fileName()).absolutePath()));
}

void SettingsDialog::onExportAllConversationsClicked()
{
    const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/LinOllama-conversations.json";
    const QString path = QFileDialog::getSaveFileName(
        this, "Export all conversations", defaultPath, "JSON files (*.json)");
    if (path.isEmpty())
        return;

    if (!m_conversationStore->exportAll(path))
        QMessageBox::warning(this, "Export failed", "Couldn't write to \"" + path + "\".");
}

void SettingsDialog::onImportConversationsClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import conversations", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "JSON files (*.json);;All files (*)");
    if (path.isEmpty())
        return;

    const int count = m_conversationStore->importAll(path);
    if (count < 0) {
        QMessageBox::warning(this, "Import failed",
            "\"" + path + "\" doesn't look like a LinOllama conversations export.");
    } else {
        QMessageBox::information(this, "Import conversations",
            QString("Imported %1 conversation(s).").arg(count));
    }
}

void SettingsDialog::onExportSettingsClicked()
{
    const QString sourcePath = QSettings().fileName();
    const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/LinOllama-settings.conf";
    const QString path = QFileDialog::getSaveFileName(
        this, "Export settings", defaultPath, "Settings files (*.conf);;All files (*)");
    if (path.isEmpty())
        return;

    QFile::remove(path); // QFile::copy() refuses to overwrite an existing file
    if (!QFile::copy(sourcePath, path))
        QMessageBox::warning(this, "Export failed", "Couldn't write to \"" + path + "\".");
}

void SettingsDialog::onImportSettingsClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import settings", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "Settings files (*.conf);;All files (*)");
    if (path.isEmpty())
        return;

    if (QMessageBox::question(this, "Import settings",
            "This replaces every current setting with the ones from \"" + path + "\". "
            "Restart LinOllama afterward for everything to take effect. Continue?")
        != QMessageBox::Yes)
        return;

    const QString destPath = QSettings().fileName();
    QFile::remove(destPath);
    if (!QFile::copy(path, destPath)) {
        QMessageBox::warning(this, "Import failed", "Couldn't read \"" + path + "\".");
        return;
    }

    QMessageBox::information(this, "Import settings",
        "Settings imported. Restart LinOllama for all changes to take effect.");
}

bool SettingsDialog::confirmDestructive(const QString &title, const QString &text,
                                         const QString &informativeText, const QString &confirmLabel)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(text);
    box.setInformativeText(informativeText);

    QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
    QPushButton *confirmButton = box.addButton(confirmLabel, QMessageBox::DestructiveRole);
    confirmButton->setObjectName("dangerButton"); // red styling comes from the app stylesheet
    box.setDefaultButton(cancelButton);

    box.exec();
    return box.clickedButton() == confirmButton;
}

void SettingsDialog::onClearConfigurationClicked()
{
    if (!confirmDestructive("Clear configuration",
            "Reset all settings to their defaults?",
            "This can't be undone. Conversations aren't affected. Restart LinOllama afterward for "
            "everything to take effect.",
            "Clear Configuration"))
        return;

    QSettings settings;
    settings.clear();
    settings.sync();
}

void SettingsDialog::onClearConversationsClicked()
{
    if (!confirmDestructive("Clear conversations",
            "Delete every conversation?",
            "This can't be undone. Settings aren't affected.",
            "Clear Conversations"))
        return;

    m_conversationStore->clearAll();
}

void SettingsDialog::onClearAllClicked()
{
    if (!confirmDestructive("Clear all data",
            "Delete every conversation and reset all settings to their defaults?",
            "This can't be undone. Restart LinOllama afterward for everything to take effect.",
            "Clear All"))
        return;

    m_conversationStore->clearAll();
    QSettings settings;
    settings.clear();
    settings.sync();
}
