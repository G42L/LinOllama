#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <QVector>
#include <cstdint>

// One physical GPU's stats for a single poll. Multiple can exist at once —
// e.g. an NVIDIA discrete GPU alongside Intel integrated graphics — and are
// reported as a list rather than picking just one, since guessing which
// GPU the person cares about is exactly the bug this replaced.
struct GpuStat
{
    enum class Vendor { Nvidia, Amd, Intel, Unknown };

    Vendor vendor = Vendor::Unknown;
    QString name;             // e.g. "NVIDIA GeForce RTX 5070 Ti", or a generic label when a proper name isn't available
    double utilPercent = -1.0; // -1 when the kernel/driver doesn't expose it
    quint64 vramUsedKB = 0;
    quint64 vramTotalKB = 0;
    bool vramAvailable = false; // false for integrated GPUs (shared with system RAM) or when unreadable
};

// SystemMonitor polls host CPU, RAM, and all detected GPUs on a timer and
// emits a single aggregated signal that UI widgets can bind to.
//
// - CPU: read from /proc/stat (system-wide) — no deps.
// - RAM: read from /proc/meminfo — no deps.
// - GPUs: enumerated from every /sys/class/drm/cardN (not the render-node or
//   connector subdirectories), vendor identified per-card via PCI vendor ID.
//   NVIDIA cards are matched to their NVML device by PCI bus ID — not just
//   "NVML device 0" — so on a mixed NVIDIA+Intel system the label and the
//   numbers are guaranteed to describe the same physical GPU. AMD/Intel read
//   straight from that card's own sysfs files, no library needed.
class SystemMonitor : public QObject
{
    Q_OBJECT

public:
    explicit SystemMonitor(QObject *parent = nullptr);
    ~SystemMonitor() override;

    // Starts polling at the given interval (milliseconds). Typical: 1000-2000ms.
    void start(int intervalMs = 1500);
    void stop();

signals:
    // Memory values are in kilobytes; percentages are 0-100.
    void statsUpdated(double cpuPercent,
                       quint64 ramUsedKB,
                       quint64 ramTotalKB,
                       QVector<GpuStat> gpus);

private slots:
    void poll();

private:
    double readCpuPercent();
    void readRamUsage(quint64 &usedKB, quint64 &totalKB);

    // Enumerates /sys/class/drm/cardN entries once and caches which sysfs
    // path + vendor + (for NVIDIA) PCI bus id each one has. Re-enumerating
    // every poll would mean extra syscalls for something that essentially
    // never changes at runtime (no hot-plug GPUs on a desktop).
    struct CardInfo
    {
        QString sysfsDevicePath; // e.g. /sys/class/drm/card0/device
        GpuStat::Vendor vendor = GpuStat::Vendor::Unknown;
        QString pciBusId;        // e.g. "0000:01:00.0" — only populated for NVIDIA cards
    };
    void enumerateCardsIfNeeded();
    QVector<CardInfo> m_cards;
    bool m_cardsEnumerated = false;

    GpuStat readNvidiaStat(const CardInfo &card);
    GpuStat readAmdStat(const CardInfo &card);
    GpuStat readIntelStat(const CardInfo &card);

    bool initNvml();
    void shutdownNvml();

    QTimer m_timer;

    // Previous /proc/stat sample, used to compute the CPU delta between polls.
    quint64 m_prevIdle = 0;
    quint64 m_prevTotal = 0;
    bool m_havePrevCpuSample = false;

    // NVML dynamic loading state.
    void *m_nvmlHandle = nullptr;
    bool m_nvmlInitAttempted = false;
    bool m_nvmlAvailable = false;
};
