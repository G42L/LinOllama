#pragma once

#include <QDialog>
#include <QVector>
#include <QPair>
#include <QString>
#include "ThemeManager.h"
#include "OllamaClient.h"

class QComboBox;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QCheckBox;
class QSlider;
class QSpinBox;

// App settings dialog: theme mode, plus an "Offload model" section mirroring
// the one in the tray menu (same underlying OllamaClient calls) — built as a
// plain form so more settings can be added as rows without restructuring
// anything. See README's "what's stubbed out" list (server env vars, model
// defaults, etc.) for the obvious next additions.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(ThemeManager *themeManager, OllamaClient *ollamaClient, QWidget *parent = nullptr);

signals:
    // Emitted live as soon as the "Send button" combo changes (the dialog
    // is modal, but ChatWidget picks this up immediately rather than
    // waiting for the dialog to close) — persistence happens in the same
    // slot that emits this, via QSettings, so it's also restored at next
    // launch. style is one of "plane" (default), "arrow", or "text".
    void sendButtonStyleChanged(const QString &style);

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

private slots:
    void onThemeComboChanged(int index);
    void onSendButtonStyleComboChanged(int index);
    void onContextLengthEnabledToggled(bool enabled);
    void onContextLengthValueChanged(int value);
    void onModelOptimizationToggled(bool enabled);
    void onLoadedModelsListed(const QVector<LoadedModelInfo> &models);
    void onModelUnloaded(const QString &model, bool success);
    void refreshLoadedModels();

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

    ThemeManager *m_themeManager = nullptr;
    OllamaClient *m_ollamaClient = nullptr;

    QComboBox *m_themeCombo = nullptr;
    QComboBox *m_sendButtonStyleCombo = nullptr;
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
};
