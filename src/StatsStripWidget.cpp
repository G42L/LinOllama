#include "StatsStripWidget.h"
#include "ThemeManager.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QSettings>

StatsStripWidget::StatsStripWidget(SystemMonitor *systemMonitor, ThemeManager *themeManager, QWidget *parent)
    : QWidget(parent)
    , m_themeManager(themeManager)
{
    setMinimumWidth(140); // draggable via MainWindow's splitter, not fixed

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(14);

    auto *heading = new QLabel("System");
    heading->setStyleSheet("font-weight: 600; opacity: 0.7;");
    layout->addWidget(heading);

    m_cpuBar = makeMeter(this, "CPU", &m_cpuValueLabel);
    layout->addWidget(m_cpuBar->parentWidget());

    m_ramBar = makeMeter(this, "RAM", &m_ramValueLabel);
    layout->addWidget(m_ramBar->parentWidget());

    // GPU meters are built lazily once we know how many GPUs exist, so this
    // is just an anchor container they get appended into.
    m_gpuSection = new QWidget;
    auto *gpuSectionLayout = new QVBoxLayout(m_gpuSection);
    gpuSectionLayout->setContentsMargins(0, 0, 0, 0);
    gpuSectionLayout->setSpacing(14);
    layout->addWidget(m_gpuSection);

    layout->addStretch();

    connect(systemMonitor, &SystemMonitor::statsUpdated,
            this, &StatsStripWidget::onStatsUpdated);

    if (m_themeManager)
        connect(m_themeManager, &ThemeManager::themeChanged, this, &StatsStripWidget::applyMeterColors);
    applyMeterColors();
}

QProgressBar *StatsStripWidget::makeMeter(QWidget *, const QString &labelText, QLabel **valueLabelOut)
{
    auto *container = new QWidget;
    auto *vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(2);

    auto *row = new QWidget;
    auto *hbox = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);

    auto *nameLabel = new QLabel(labelText);
    nameLabel->setStyleSheet("font-size: 11px; opacity: 0.6;");
    nameLabel->setWordWrap(true);
    hbox->addWidget(nameLabel);
    hbox->addStretch();

    auto *valueLabel = new QLabel("--");
    valueLabel->setStyleSheet("font-size: 11px; opacity: 0.6;");
    hbox->addWidget(valueLabel);
    *valueLabelOut = valueLabel;

    vbox->addWidget(row);

    auto *bar = new QProgressBar;
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    vbox->addWidget(bar);

    return bar;
}

void StatsStripWidget::onStatsUpdated(double cpuPercent,
                                       quint64 ramUsedKB, quint64 ramTotalKB,
                                       QVector<GpuStat> gpus)
{
    m_cpuBar->setValue(static_cast<int>(cpuPercent));
    m_cpuValueLabel->setText(QString("%1%").arg(cpuPercent, 0, 'f', 0));

    const int ramPercent = ramTotalKB > 0
        ? static_cast<int>((double(ramUsedKB) / double(ramTotalKB)) * 100.0)
        : 0;
    m_ramBar->setValue(ramPercent);
    m_ramValueLabel->setText(QString("%1 / %2").arg(formatKB(ramUsedKB), formatKB(ramTotalKB)));

    // Build (or rebuild, if the GPU count changed — e.g. first poll after
    // construction) the per-GPU meter widgets.
    if (m_gpuMeters.size() != gpus.size()) {
        // Clear existing GPU widgets.
        QLayout *gpuLayout = m_gpuSection->layout();
        QLayoutItem *item;
        while ((item = gpuLayout->takeAt(0)) != nullptr) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
        m_gpuMeters.clear();

        for (int i = 0; i < gpus.size(); ++i) {
            auto *sep = new QFrame;
            sep->setFrameShape(QFrame::HLine);
            sep->setStyleSheet("color: rgba(128,128,128,60);");
            gpuLayout->addWidget(sep);

            GpuMeterWidgets meter;
            meter.titleLabel = new QLabel;
            meter.titleLabel->setStyleSheet("font-weight: 600; opacity: 0.7;");
            meter.titleLabel->setWordWrap(true);
            gpuLayout->addWidget(meter.titleLabel);

            meter.utilBar = makeMeter(m_gpuSection, "Usage", &meter.utilValueLabel);
            gpuLayout->addWidget(meter.utilBar->parentWidget());

            meter.memBar = makeMeter(m_gpuSection, "Memory", &meter.memValueLabel);
            gpuLayout->addWidget(meter.memBar->parentWidget());

            m_gpuMeters.append(meter);
        }

        if (gpus.isEmpty()) {
            auto *noneLabel = new QLabel("No GPU detected");
            noneLabel->setStyleSheet("font-size: 11px; opacity: 0.5;");
            gpuLayout->addWidget(noneLabel);
        }

        applyMeterColors(); // the bars just built above have no styling yet
    }

    for (int i = 0; i < gpus.size() && i < m_gpuMeters.size(); ++i) {
        const GpuStat &gpu = gpus[i];
        GpuMeterWidgets &meter = m_gpuMeters[i];

        meter.titleLabel->setText(gpu.name);

        if (gpu.utilPercent >= 0.0) {
            meter.utilBar->setValue(static_cast<int>(gpu.utilPercent));
            meter.utilValueLabel->setText(QString("%1%").arg(gpu.utilPercent, 0, 'f', 0));
        } else {
            meter.utilBar->setValue(0);
            meter.utilValueLabel->setText("n/a");
        }

        if (gpu.vramAvailable) {
            const int memPercent = gpu.vramTotalKB > 0
                ? static_cast<int>((double(gpu.vramUsedKB) / double(gpu.vramTotalKB)) * 100.0)
                : 0;
            meter.memBar->setValue(memPercent);
            meter.memValueLabel->setText(QString("%1 / %2").arg(formatKB(gpu.vramUsedKB), formatKB(gpu.vramTotalKB)));
        } else {
            meter.memBar->setValue(0);
            meter.memValueLabel->setText("shared w/ RAM");
        }
    }
}

QString StatsStripWidget::formatKB(quint64 kb)
{
    constexpr double MB = 1024.0;
    constexpr double GB = 1024.0 * 1024.0;
    if (kb >= static_cast<quint64>(GB))
        return QString("%1G").arg(kb / GB, 0, 'f', 1);
    return QString("%1M").arg(kb / MB, 0, 'f', 0);
}

void StatsStripWidget::styleMeterBar(QProgressBar *bar, const QString &trackColorHex, const QString &chunkColorHex)
{
    // A per-instance stylesheet (rather than an objectName + a Theme.cpp
    // selector) since the chunk color is a user preference read from
    // QSettings at apply-time, not one of the fixed light/dark tokens
    // Theme::styleSheet() substitutes app-wide.
    bar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: none; border-radius: 3px; }"
        "QProgressBar::chunk { background-color: %2; border-radius: 3px; }")
        .arg(trackColorHex, chunkColorHex));
}

void StatsStripWidget::applyMeterColors()
{
    const bool dark = m_themeManager && m_themeManager->isDarkActive();
    const QString trackColor = Theme::colorToken("progressTrack", dark);
    const QString defaultColor = Theme::currentAccentColor(dark);

    // Empty (never customized in Settings) falls back to the *effective*
    // accent — the user's own "Application" color if they've set one,
    // otherwise the theme's default — so an un-customized meter still
    // matches the rest of the app and follows a light/dark switch or a
    // custom Application color automatically, and only
    // "escapes" that once the person actually picks a color for it.
    auto colorFor = [&](const QString &settingsKey) {
        const QString stored = QSettings().value(settingsKey).toString();
        return stored.isEmpty() ? defaultColor : stored;
    };

    const QString cpuColor = colorFor("stats/cpuColor");
    const QString ramColor = colorFor("stats/ramColor");
    const QString gpuColor = colorFor("stats/gpuColor");
    const QString vramColor = colorFor("stats/vramColor");

    if (m_cpuBar)
        styleMeterBar(m_cpuBar, trackColor, cpuColor);
    if (m_ramBar)
        styleMeterBar(m_ramBar, trackColor, ramColor);
    for (GpuMeterWidgets &meter : m_gpuMeters) {
        styleMeterBar(meter.utilBar, trackColor, gpuColor);
        styleMeterBar(meter.memBar, trackColor, vramColor);
    }
}
