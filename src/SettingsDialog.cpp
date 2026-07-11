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
#include <QTabWidget>
#include <QLineEdit>

SettingsDialog::SettingsDialog(ThemeManager *themeManager, OllamaClient *ollamaClient,
                                WhisperManager *whisperManager, QWidget *parent)
    : QDialog(parent)
    , m_themeManager(themeManager)
    , m_ollamaClient(ollamaClient)
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
    auto *inputsPage = new QWidget;
    auto *inputsPageLayout = new QVBoxLayout(inputsPage);
    auto *ollamaPage = new QWidget;
    auto *ollamaPageLayout = new QVBoxLayout(ollamaPage);
    auto *whisperPage = new QWidget;
    auto *whisperPageLayout = new QVBoxLayout(whisperPage);

    tabs->addTab(appearancePage, "Appearance");
    tabs->addTab(inputsPage, "Inputs");
    tabs->addTab(ollamaPage, "Ollama");
    tabs->addTab(whisperPage, "Whisper");

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

    // "Application" drives the whole app's accent (focus borders, the
    // send/stop button, progress-bar fills, etc. — see Theme::styleSheet())
    // — not just the stats panel. Each row's own reset button (rather than
    // one shared "reset all") lets any single color be reverted on its own.
    appearanceLayout->addRow("Application", makeColorPickerRow("appearance/accentColor"));

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
        nameLabel->setStyleSheet("font-size: 11px; font-weight: normal; opacity: 0.7;");
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

    appearanceLayout->addRow(statsColorsRow);

    appearancePageLayout->addWidget(appearanceGroup);
    appearancePageLayout->addStretch();

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
    contextLengthHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
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
    modelOptimizationHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
    // Real QWidget margins, not QSS "padding" — a stylesheet's padding on a
    // word-wrapped QLabel isn't reliably folded into its own heightForWidth()
    // calculation (that's what caused the clipped top/bottom lines before),
    // whereas setContentsMargins() always is, since it's native Qt layout
    // geometry rather than something QStyleSheetStyle has to intercept.
    modelOptimizationHint->setContentsMargins(0, 4, 0, 4);
    modelLayout->addWidget(modelOptimizationHint);

    ollamaPageLayout->addWidget(modelGroup);

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
    envHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
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
    meterSmoothingHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
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
    voiceAutoSendHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
    whisperLayout->addWidget(voiceAutoSendHint);

    whisperLayout->addSpacing(10); // visual break before the binary/model-location controls below

    m_whisperStatusLabel = new QLabel;
    m_whisperStatusLabel->setWordWrap(true);
    m_whisperStatusLabel->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
    whisperLayout->addWidget(m_whisperStatusLabel);

    auto *whisperPathsRow = new QHBoxLayout;
    auto *changeBinaryButton = new QPushButton("Whisper binary…");
    connect(changeBinaryButton, &QPushButton::clicked, this, &SettingsDialog::onChangeWhisperBinaryClicked);
    whisperPathsRow->addWidget(changeBinaryButton);
    auto *changeModelsDirButton = new QPushButton("Models folder…");
    connect(changeModelsDirButton, &QPushButton::clicked, this, &SettingsDialog::onChangeWhisperModelsDirClicked);
    whisperPathsRow->addWidget(changeModelsDirButton);
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
    whisperHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
    whisperLayout->addWidget(whisperHint);

    whisperPageLayout->addWidget(whisperGroup);
    whisperPageLayout->addStretch();

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
    offloadHint->setStyleSheet("font-size: 11px; opacity: 0.7; font-weight: normal;");
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
    if (!stored.isEmpty())
        return stored;

    const bool dark = m_themeManager->isDarkActive();
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
        notifyColorChanged(settingsKey);
    });
    connect(resetButton, &QPushButton::clicked, this, [this, settingsKey]() {
        QSettings().remove(settingsKey);
        notifyColorChanged(settingsKey);
    });

    m_colorSwatchButtons.append({swatchButton, settingsKey});
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
    // simplest correct behavior is just to re-fetch it.
    refreshLoadedModels();
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

    if (!m_whisperManager->isBinaryAvailable()) {
        m_whisperStatusLabel->setText(
            "whisper-cli not found — build whisper.cpp or point at an existing binary below.");
        return;
    }

    const QString modelsDir = m_whisperManager->modelsDir().isEmpty()
        ? "(not set yet — pick one, or download a model to create a default)"
        : m_whisperManager->modelsDir();
    m_whisperStatusLabel->setText(
        QString("Binary: %1\nModels folder: %2")
            .arg(m_whisperManager->binaryPath(), modelsDir));
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
