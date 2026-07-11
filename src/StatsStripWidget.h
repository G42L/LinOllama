#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QVector>
#include "SystemMonitor.h"

class ThemeManager;

// A narrow vertical strip of live meters: CPU%, RAM used/total, and one
// utilization + memory meter per detected GPU. GPU meters are built
// dynamically since the number of GPUs varies by machine (e.g. an NVIDIA
// discrete card alongside Intel integrated graphics shows two). Lives in
// MainWindow's resizable QSplitter, so its width is draggable rather than
// fixed — see MainWindow.cpp.
class StatsStripWidget : public QWidget
{
    Q_OBJECT

public:
    // Does not take ownership; systemMonitor/themeManager's lifetimes are
    // managed by main(). themeManager is only used to know light-vs-dark
    // for the default (unset) meter color and to re-apply colors live on a
    // theme switch — see applyMeterColors().
    explicit StatsStripWidget(SystemMonitor *systemMonitor, ThemeManager *themeManager, QWidget *parent = nullptr);

public slots:
    // Re-derives each meter's color from QSettings ("stats/cpuColor" etc.,
    // set via SettingsDialog's color pickers) — an unset/empty value falls
    // back to the current theme's accent color, so a meter nobody has
    // customized still tracks the app's own color and a light/dark switch.
    // Called at construction, whenever GPU meters are rebuilt (new bar
    // instances have no styling yet), on ThemeManager::themeChanged, and
    // from MainWindow when SettingsDialog reports a color change.
    void applyMeterColors();

private slots:
    void onStatsUpdated(double cpuPercent,
                         quint64 ramUsedKB, quint64 ramTotalKB,
                         QVector<GpuStat> gpus);

private:
    static QString formatKB(quint64 kb);
    QProgressBar *makeMeter(QWidget *parentLayoutOwner, const QString &labelText, QLabel **valueLabelOut);
    static void styleMeterBar(QProgressBar *bar, const QString &trackColorHex, const QString &chunkColorHex);

    // Per-GPU widgets, keyed by index into the last-seen GPU list. Rebuilt
    // only when the number of GPUs changes (essentially: once, at first
    // poll) rather than destroyed/recreated every tick.
    struct GpuMeterWidgets
    {
        QLabel *titleLabel = nullptr;
        QLabel *utilValueLabel = nullptr;
        QProgressBar *utilBar = nullptr;
        QLabel *memValueLabel = nullptr;
        QProgressBar *memBar = nullptr;
    };

    QLabel *m_cpuValueLabel = nullptr;
    QProgressBar *m_cpuBar = nullptr;

    QLabel *m_ramValueLabel = nullptr;
    QProgressBar *m_ramBar = nullptr;

    QWidget *m_gpuSection = nullptr; // container the GPU meters get added to
    QVector<GpuMeterWidgets> m_gpuMeters;

    ThemeManager *m_themeManager = nullptr;
};
