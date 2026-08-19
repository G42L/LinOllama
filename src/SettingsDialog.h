#pragma once

#include <QDialog>
#include <QVector>
#include <QPair>
#include <QString>
#include <QHash>
#include <QSet>
#include <QStringList>
#include "ThemeManager.h"
#include "OllamaClient.h"
#include "ConversationStore.h"
#include "WhisperManager.h"

class RagStore;
class RagIngestionController;

class QComboBox;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QCheckBox;
class QSlider;
class QSpinBox;
class QTableWidget;
class QProgressBar;
class QToolButton;
class QLineEdit;
class QDoubleSpinBox;

// App settings dialog: theme mode, plus an "Offload model" section mirroring
// the one in the tray menu (same underlying OllamaClient calls) — built as a
// plain form so more settings can be added as rows without restructuring
// anything. See README's "what's stubbed out" list (server env vars, model
// defaults, etc.) for the obvious next additions.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(ThemeManager *themeManager, OllamaClient *ollamaClient,
                             ConversationStore *conversationStore,
                             WhisperManager *whisperManager,
                             RagStore *ragStore, RagIngestionController *ragIngestionController,
                             QWidget *parent = nullptr);

signals:
    // Emitted live as soon as the "Send button" combo changes (the dialog
    // is modal, but ChatWidget picks this up immediately rather than
    // waiting for the dialog to close) — persistence happens in the same
    // slot that emits this, via QSettings, so it's also restored at next
    // launch. style is one of "plane" (default), "arrow", or "text".
    void sendButtonStyleChanged(const QString &style);

    // Emitted live as soon as the "Filled send button" checkbox changes —
    // same live-preview/persistence pattern as sendButtonStyleChanged just
    // above. false (default, unchecked): flat, matching the attach/tools/
    // voice buttons. true: the button's original solid accent-colored pill.
    void sendButtonFilledChanged(bool filled);

    // Emitted whenever the "Send automatically after transcription"
    // checkbox changes — ChatWidget listens live. false (default): the
    // transcribed text fills the message box for review/correction before
    // sending. true: it's sent immediately, same as the very first
    // implementation of this feature did. Persistence to QSettings
    // ("chat/voiceAutoSend") already happened by the time this fires.
    void voiceAutoSendChanged(bool enabled);

    // Emitted whenever any color (Application or a stats meter) is changed
    // or individually reset to default — StatsStripWidget listens live
    // (same pattern as sendButtonStyleChanged above). Persistence to
    // QSettings already happened by the time this fires; the Application
    // color additionally goes through m_themeManager->notifyAppearanceChanged()
    // first, since it affects the whole app's stylesheet, not just the stats panel.
    void statsColorsChanged();

    // Emitted whenever the context-length checkbox or slider/spinbox
    // changes — ChatWidget listens live to refresh the context-usage bar's
    // denominator for the currently displayed conversation. Persistence to
    // QSettings ("chat/useCustomContextLength", "chat/customContextLength")
    // already happened by the time this fires.
    void contextLengthSettingChanged();

    // Emitted whenever the "Queing model optimization" checkbox changes —
    // ChatWidget listens live and forwards it straight to ChatQueue::
    // setOptimizeModelSwaps(). Persistence to QSettings
    // ("chat/modelOptimization") already happened by the time this fires.
    void modelOptimizationChanged(bool enabled);

    // Emitted whenever the microphone combo changes — ChatWidget listens
    // live and forwards it straight to VoiceRecorder::refreshAudioInputDevice().
    // Persistence to QSettings ("voice/audioInputDeviceId") already happened
    // by the time this fires.
    void audioInputDeviceChanged();

    // Emitted whenever the meter-smoothing slider changes — ChatWidget
    // listens live and forwards it straight to VoiceRecorder::
    // refreshMeterSmoothing(). Persistence to QSettings
    // ("voice/meterSmoothingPercent") already happened by the time this fires.
    void meterSmoothingChanged();

    // Emitted whenever any Formatting-tab slider changes (paragraph/list/
    // heading spacing, or the whole-app font-size scale) — ChatWidget
    // listens live and calls refreshFormattingSettings(). Unlike most other
    // settings here, these are baked directly into each message's rendered
    // QTextDocument content at render time, so nothing already on screen
    // updates just from the QSettings value changing — the conversation
    // has to be re-rendered from scratch to pick up the new value.
    // Persistence already happened by the time this fires.
    void formattingSettingsChanged();

    // Emitted whenever the Brave Search API key field finishes editing —
    // ChatWidget listens live and forwards it straight to
    // ToolExecutor::setBraveApiKey(). Persistence to QSettings
    // ("webSearch/braveApiKey") already happened by the time this fires.
    void braveApiKeyChanged(const QString &key);

    // Emitted whenever the Knowledge Base tab's embedding-model combo
    // changes — ChatWidget listens live and forwards it straight to
    // ToolExecutor::setEmbeddingModel(). Persistence to QSettings
    // ("rag/embeddingModel") already happened by the time this fires.
    void embeddingModelChanged(const QString &model);

private slots:
    void onThemeComboChanged(int index);
    void onThemeColorPresetChanged(int index);
    void onSendButtonStyleComboChanged(int index);
    void onSendButtonFilledToggled(bool filled);
    void onContextLengthEnabledToggled(bool enabled);
    void onContextLengthValueChanged(int value);
    void onModelOptimizationToggled(bool enabled);
    void onLoadedModelsListed(const QVector<LoadedModelInfo> &models);
    void onModelUnloaded(const QString &model, bool success);
    void refreshLoadedModels();

    void onWhisperModelsChanged();
    void onWhisperDownloadProgress(const QString &modelId, qint64 received, qint64 total);
    void onWhisperDownloadFinished(const QString &modelId, bool success, const QString &error);
    void onChangeWhisperBinaryClicked();
    void onChangeWhisperModelsDirClicked();
    void onChangeWhisperServerBinaryClicked();
    void onWhisperExpandToggleClicked();
    void onVoiceAutoSendToggled(bool enabled);
    void onLiveTranscriptionToggled(bool enabled);

    void onAudioInputComboChanged(int index);
    void refreshAudioInputCombo();
    void onMeterSmoothingSliderChanged(int value);

    // Persist straight to QSettings ("ollamaServer/*") — no live signal out
    // of SettingsDialog, since these only take effect the next time
    // ServerController actually starts Ollama (see its own
    // configuredEnvironmentOverrides()/applyUserSystemdEnvironmentOverride()),
    // not live like most other settings in this dialog.
    void onOllamaModelsPathEdited();
    void onBrowseOllamaModelsPathClicked();
    void onOllamaKeepAliveEdited();
    void onOllamaFlashAttentionToggled(bool enabled);
    void onOllamaNumParallelChanged(int value);

    void onBraveApiKeyEdited();

    // Knowledge Base tab — see rebuildKbDocumentsList()/refreshEmbeddingModelCombo().
    void onEmbeddingModelComboChanged(int index);
    void onChunkSizeChanged(int value);
    void onChunkOverlapChanged(int value);
    void onAddKbDocumentsClicked();
    // Connected to RagIngestionController's signals — see that class's own
    // header for why ingestion keeps running even if this dialog is closed
    // mid-batch (these slots just stop firing once the dialog is destroyed,
    // same as any other Qt signal/slot connection whose receiver is gone).
    void onKbIngestionProgress(const QString &fileName, int chunkIndex, int chunkCount);
    void onKbIngestionFileFinished(const QString &fileName, bool success, const QString &errorMessage);
    void onKbIngestionQueueFinished();

    // Persist straight to QSettings ("chat/*") — read fresh at send time by
    // ChatWidget::streamAssistantReplyForCurrentHistory() (same pattern as
    // the existing context-length setting), so no live signal out of this
    // dialog is needed; nothing elsewhere in the UI reflects these values.
    void onGenParamsToggled(bool enabled);
    void onTemperatureChanged(double value);
    void onTopPChanged(double value);
    void onTopKChanged(int value);
    void onSeedChanged(int value);
    void onNumPredictChanged(int value);
    void onRepeatPenaltyChanged(double value);
    void onStopSequencesEdited();

    void onPullModelClicked();
    void onCancelPullClicked();
    void onModelPullProgress(const QString &model, const QString &status, qint64 completed, qint64 total);
    void onModelPullFinished(const QString &model, bool success, const QString &error);
    void onModelDeleted(const QString &model, bool success, const QString &error);
    // Reacts to OllamaClient::modelsListed — the same periodic /api/tags
    // poll (see main.cpp's reachabilityTimer) that keeps the model combo
    // elsewhere in the app current also keeps this list current, so there's
    // no separate fetch call needed here.
    void onInstalledModelsListed(const QStringList &modelNames);

    // Reacts to OllamaClient::modelMetadataFetched, fired off the
    // per-model /api/show request rebuildInstalledModelsList() kicks off
    // (see m_modelCapabilities below) — refreshes just the capability
    // icons once a model's result comes in, rather than waiting for the
    // next 3s modelsListed() poll to pick up the cache.
    void onModelMetadataFetched(const QString &model, const ModelMetadata &metadata);

    // Data tab: storage locations + backup/restore — see ConversationStore::
    // exportAll()/importAll() for the conversations bundle format, and
    // QSettings().fileName() for where the settings file itself lives.
    void onOpenConversationsFolderClicked();
    void onOpenSettingsFolderClicked();
    void onExportAllConversationsClicked();
    void onImportConversationsClicked();
    void onExportSettingsClicked();
    void onImportSettingsClicked();

    // Data tab's "Clear Data" group — all irreversible, all gated behind
    // confirmDestructive() below.
    void onClearConfigurationClicked();
    void onClearConversationsClicked();
    void onClearAllClicked();

private:
    void rebuildLoadedModelsList(const QVector<LoadedModelInfo> &models);
    void clearLoadedModelsList();

    // Builds one row — a color swatch + a small reset-to-default button —
    // bound to a QSettings key (e.g. "stats/cpuColor", or the special
    // "appearance/accentColor" for the overall Application color). Clicking
    // the swatch opens a QColorDialog; clicking reset clears just that one
    // key. Registers the swatch in m_colorSwatchButtons so any other
    // color's change can refresh it too (an unset meter color visually
    // tracks the Application color, so changing that one has to refresh
    // every other swatch as well).
    QWidget *makeColorPickerRow(const QString &settingsKey);

    // Builds one row — a label, a slider, and a spinbox kept in sync with
    // each other — for a single Formatting-tab spacing value (paragraph,
    // list item, or heading-before spacing, all in pixels). Persists
    // straight to QSettings (settingsKey) on every change; no live signal
    // out of this dialog, since AutoHeightTextBrowser::applyBlockSpacing()
    // reads the value fresh at render time — same "no live signal needed"
    // pattern as the generation-parameter fields, since it only affects
    // messages rendered from this point on, not ones already on screen.
    QWidget *makeSpacingSliderRow(const QString &labelText, const QString &settingsKey,
                                  int minVal, int maxVal, int defaultValue);

    // Shows a QMessageBox::Warning with a Cancel button (default) and a
    // red "dangerButton"-styled confirm button labeled `confirmLabel` — same
    // pattern as MainWindow::confirmAndDeleteConversation(). Returns true
    // only if the confirm button was actually clicked. Used by the Data
    // tab's "Clear Data" actions.
    bool confirmDestructive(const QString &title, const QString &text,
                             const QString &informativeText, const QString &confirmLabel);

    // Rebuilds m_whisperModelsTable's rows from scratch — the catalog is
    // fixed, so this just re-derives each row's install/selection/download
    // state (installed models, the current selection, any in-flight
    // download) rather than patching individual cells. Cheap enough to call
    // on every relevant event except raw download progress ticks, which
    // update their row's existing QProgressBar directly instead (see
    // onWhisperDownloadProgress()) to avoid rebuilding the whole table
    // several times a second while a download is running.
    void rebuildWhisperModelsTable();
    void refreshWhisperStatusLabel();
    // Swaps m_whisperExpandButton's icon between expand.svg/contract.svg for
    // the current m_whisperTableExpanded state — called once at construction
    // and again on every toggle (the dialog itself is never long-lived
    // enough to need a live theme-change hookup like ChatWidget's icons do;
    // a fresh SettingsDialog is constructed each time Settings is opened).
    void updateWhisperExpandIcon();

    WhisperManager *m_whisperManager = nullptr;
    QLabel *m_whisperStatusLabel = nullptr;
    QCheckBox *m_voiceAutoSendCheck = nullptr; // "Send automatically after transcription" — see voiceAutoSendChanged()
    // "Enable live transcription" — "voice/liveTranscriptionEnabled" in
    // QSettings, off by default even when whisper-server is installed and
    // detected. Read fresh by ChatWidget::onVoicePressed() at the moment
    // the mic button is pressed, same as e.g. useCustomContextLength — no
    // live-signal round trip needed since it only matters at that one
    // point, not continuously. Grayed out (see refreshWhisperStatusLabel())
    // whenever no whisper-server binary is available, since it wouldn't do
    // anything either way in that case.
    QCheckBox *m_liveTranscriptionCheck = nullptr;
    QTableWidget *m_whisperModelsTable = nullptr;
    QToolButton *m_whisperExpandButton = nullptr;
    // Minimal (default): Model/Speed/Accuracy/action only, with disk size,
    // memory, language, and usage blurb folded into each row's tooltip
    // instead of their own columns — full: every catalog column shown at
    // once. Toggled via m_whisperExpandButton; not persisted, since it's
    // just a display convenience rather than a real setting.
    bool m_whisperTableExpanded = false;
    // Row action-cell progress bars, keyed by model id — only present for a
    // model currently downloading, looked up directly by
    // onWhisperDownloadProgress() instead of rebuilding the table per tick.
    QHash<QString, QProgressBar *> m_whisperProgressBars;
    // The stored hex for settingsKey, or its fallback if nothing's been
    // saved: the effective accent (Theme::currentAccentColor()) for a
    // stats-meter key, or the theme's own built-in default for the
    // Application key itself (so it doesn't just reference its own value).
    QString currentColorHex(const QString &settingsKey) const;
    void refreshColorSwatch(QPushButton *button, const QString &settingsKey);
    // Persists via the caller (already done) — this just handles the
    // side effects: a full app-wide re-style if the Application color was
    // the one that changed, refreshing every swatch, and telling
    // StatsStripWidget to re-derive its meter colors.
    void notifyColorChanged(const QString &changedKey);
    // Re-selects m_themeColorCombo's current item to match whatever
    // "appearance/accentColor" actually holds right now — "Default" if
    // unset, the matching preset if it happens to equal one exactly, else
    // "Custom…" (e.g. after picking an arbitrary color via the Application
    // swatch below the combo). Called at construction and from
    // notifyColorChanged() whenever the accent color changes some other
    // way, so the combo never drifts out of sync with the swatch.
    void refreshThemeColorCombo();

    ThemeManager *m_themeManager = nullptr;
    OllamaClient *m_ollamaClient = nullptr;
    ConversationStore *m_conversationStore = nullptr; // non-owning — see the Data tab's backup/restore actions

    QComboBox *m_themeCombo = nullptr;
    // "Theme color": Default / one of 8 curated presets / Custom — a quick
    // way to set "appearance/accentColor" without opening the color picker,
    // kept in sync with the swatch below it (see refreshThemeColorCombo()).
    QComboBox *m_themeColorCombo = nullptr;
    QComboBox *m_sendButtonStyleCombo = nullptr;
    QCheckBox *m_sendButtonFilledCheck = nullptr; // "Filled send button" — see sendButtonFilledChanged()
    QVector<QPair<QPushButton *, QString>> m_colorSwatchButtons; // (swatch button, settingsKey) — see makeColorPickerRow()

    // Context length: "Use custom context length" gates a 1..262144
    // slider+spinbox pair (kept in sync with each other) that sets
    // options.num_ctx on chat requests when checked — see ChatWidget's
    // streamAssistantReplyForCurrentHistory(). Unchecked (the default)
    // omits num_ctx entirely, which is Ollama's own "pick a sensible
    // default for me" behavior — NOT "unlimited": no such mode exists, every
    // model has a hard context ceiling from its own GGUF metadata that
    // Ollama enforces regardless of what's requested.
    QCheckBox *m_useCustomContextLengthCheck = nullptr;
    QSlider *m_contextLengthSlider = nullptr;
    QSpinBox *m_contextLengthSpinBox = nullptr;

    // "Queing model optimization": off by default (see ChatQueue) — when a chat is
    // sent, it's queued strictly in the order it was submitted even if that
    // means swapping models back and forth. Checking this lets the queue
    // instead prefer running whichever queued turn matches the
    // currently-loaded model, reducing load/unload round trips at the cost
    // of not preserving strict submission order.
    QCheckBox *m_modelOptimizationCheck = nullptr;

    QVBoxLayout *m_loadedModelsLayout = nullptr; // rows get inserted/cleared here
    QLabel *m_loadedModelsStatusLabel = nullptr;  // "Loading…" / "No models loaded" placeholder

    // "System default" (empty itemData) plus one entry per
    // QMediaDevices::audioInputs() (itemData = QAudioDevice::id(), a
    // QByteArray) — see refreshAudioInputCombo(). Picking a specific device
    // pins push-to-talk recording to it via "voice/audioInputDeviceId" in
    // QSettings, regardless of what the OS considers default afterward.
    QComboBox *m_audioInputCombo = nullptr;

    // "Meter smoothing" slider (0..100, default 50) — see VoiceRecorder::
    // refreshMeterSmoothing() for how this maps to actual attack/release
    // rates. m_meterSmoothingValueLabel shows "Sharper"/"Default"/"Smoother"
    // rather than a bare number, since the raw 0-100 scale isn't meaningful
    // on its own.
    QSlider *m_meterSmoothingSlider = nullptr;
    QLabel *m_meterSmoothingValueLabel = nullptr;

    // Inputs tab's "Web Search" group — see onBraveApiKeyEdited().
    QLineEdit *m_braveApiKeyEdit = nullptr;

    // Ollama tab's "Server environment" group — see the onOllama*() slots.
    QLineEdit *m_ollamaModelsPathEdit = nullptr;
    QLineEdit *m_ollamaKeepAliveEdit = nullptr;
    QCheckBox *m_ollamaFlashAttentionCheck = nullptr;
    QSpinBox *m_ollamaNumParallelSpin = nullptr;

    // Ollama tab's "Models" group — pull new models, delete installed ones.
    QLineEdit *m_pullModelEdit = nullptr;
    QPushButton *m_pullButton = nullptr;
    QPushButton *m_cancelPullButton = nullptr;
    QProgressBar *m_pullProgressBar = nullptr;
    QLabel *m_pullStatusLabel = nullptr;
    // Empty when nothing's pulling — the one thing this UI restricts beyond
    // what OllamaClient itself allows (which supports one concurrent pull
    // per distinct model reference): only one pull at a time *from this
    // dialog*, so there's only ever one progress bar/status line to show.
    QString m_pullingModel;

    QVBoxLayout *m_installedModelsLayout = nullptr; // rows get inserted/cleared here, same pattern as m_loadedModelsLayout
    QLabel *m_installedModelsStatusLabel = nullptr;
    void rebuildInstalledModelsList(const QStringList &modelNames);
    void clearInstalledModelsList();
    QWidget *buildCapabilityIconsWidget(const QStringList &capabilities, bool dark);

    // Rebuilds m_embeddingModelCombo from m_modelCapabilities — every
    // installed model whose capabilities include "embedding", alphabetical.
    // Reuses the same cache the Ollama tab's installed-models list already
    // populates (see onModelMetadataFetched(), which calls this too) rather
    // than issuing its own separate /api/show requests. Preserves the
    // current selection if it's still in the filtered list; shows a
    // disabled placeholder item ("No embedding models found — pull one,
    // e.g. nomic-embed-text") if none qualify yet.
    void refreshEmbeddingModelCombo();
    // Same clear/rebuild-from-scratch pattern as clearInstalledModelsList()/
    // rebuildInstalledModelsList() — one row per RagStore::listDocuments()
    // entry (name, chunk count, ingested date, a "Remove" button), or a
    // single "No documents yet" placeholder label.
    void clearKbDocumentsList();
    void rebuildKbDocumentsList();

    // Capability icons (thinking/vision/tools — see buildCapabilityIconsWidget())
    // shown after each installed model's name. Keyed by model name.
    // m_modelCapabilities caches OllamaClient::modelMetadataFetched()'s
    // result so the 3s modelsListed() poll doesn't re-fetch it every time
    // it rebuilds the list; m_capabilitiesRequestedFor tracks which models
    // already have an /api/show request in flight or answered, so each
    // model is only ever queried once per session.
    QHash<QString, QStringList> m_modelCapabilities;
    QSet<QString> m_capabilitiesRequestedFor;

    // Ollama tab's "Generation parameters" group — see GenerationOptions
    // (OllamaClient.h) for how these map onto the actual /api/chat request,
    // and the onGenParamsToggled()/etc. slots for persistence. All six
    // numeric fields plus stop sequences are gated by one master checkbox
    // rather than each having its own independent enable/disable.
    QCheckBox *m_useCustomGenParamsCheck = nullptr;
    QDoubleSpinBox *m_temperatureSpin = nullptr;
    QDoubleSpinBox *m_topPSpin = nullptr;
    QSpinBox *m_topKSpin = nullptr;
    QSpinBox *m_seedSpin = nullptr;
    QSpinBox *m_numPredictSpin = nullptr;
    QDoubleSpinBox *m_repeatPenaltySpin = nullptr;
    QLineEdit *m_stopSequencesEdit = nullptr;

    // Knowledge Base tab — not owned, app-level shared instances (see
    // main.cpp), same non-owning-pointer convention as m_ollamaClient.
    RagStore *m_ragStore = nullptr;
    RagIngestionController *m_ragIngestionController = nullptr;

    QComboBox *m_embeddingModelCombo = nullptr;
    QSpinBox *m_chunkSizeSpin = nullptr;
    QSpinBox *m_chunkOverlapSpin = nullptr;

    QVBoxLayout *m_kbDocumentsLayout = nullptr; // rows inserted/cleared here, same pattern as m_installedModelsLayout
    QLabel *m_kbDocumentsStatusLabel = nullptr;

    QProgressBar *m_kbIngestionProgressBar = nullptr;
    QLabel *m_kbIngestionStatusLabel = nullptr;
};
