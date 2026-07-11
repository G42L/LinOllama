#include "SystemMonitor.h"

#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

#include <dlfcn.h>

// ---- Minimal NVML declarations -------------------------------------------
// We deliberately don't depend on the CUDA toolkit's nvml.h header being
// installed. We declare the small slice of the ABI we use and resolve the
// symbols at runtime via dlopen/dlsym, so the app builds and runs fine on
// machines with no NVIDIA driver at all — GPU stats just skip NVIDIA cards.
extern "C" {
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef nvmlReturn_t (*nvmlInit_v2_t)();
typedef nvmlReturn_t (*nvmlShutdown_t)();
typedef nvmlReturn_t (*nvmlDeviceGetHandleByPciBusId_v2_t)(const char *, nvmlDevice_t *);
typedef nvmlReturn_t (*nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, nvmlMemory_t *);
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, nvmlUtilization_t *);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char *, unsigned int);
}

namespace {
nvmlInit_v2_t p_nvmlInit_v2 = nullptr;
nvmlShutdown_t p_nvmlShutdown = nullptr;
nvmlDeviceGetHandleByPciBusId_v2_t p_nvmlDeviceGetHandleByPciBusId_v2 = nullptr;
nvmlDeviceGetMemoryInfo_t p_nvmlDeviceGetMemoryInfo = nullptr;
nvmlDeviceGetUtilizationRates_t p_nvmlDeviceGetUtilizationRates = nullptr;
nvmlDeviceGetName_t p_nvmlDeviceGetName = nullptr;
constexpr int NVML_SUCCESS = 0;
}

SystemMonitor::SystemMonitor(QObject *parent) : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &SystemMonitor::poll);
}

SystemMonitor::~SystemMonitor()
{
    shutdownNvml();
}

void SystemMonitor::start(int intervalMs)
{
    m_timer.setInterval(intervalMs);
    if (!m_timer.isActive())
        m_timer.start();
    poll();
}

void SystemMonitor::stop()
{
    m_timer.stop();
}

void SystemMonitor::poll()
{
    const double cpuPercent = readCpuPercent();

    quint64 ramUsedKB = 0, ramTotalKB = 0;
    readRamUsage(ramUsedKB, ramTotalKB);

    enumerateCardsIfNeeded();

    QVector<GpuStat> gpus;
    for (const CardInfo &card : m_cards) {
        switch (card.vendor) {
        case GpuStat::Vendor::Nvidia:
            gpus.append(readNvidiaStat(card));
            break;
        case GpuStat::Vendor::Amd:
            gpus.append(readAmdStat(card));
            break;
        case GpuStat::Vendor::Intel:
            gpus.append(readIntelStat(card));
            break;
        case GpuStat::Vendor::Unknown:
        default:
            break; // not a GPU we know how to read (e.g. a virtual display adapter) — skip silently
        }
    }

    emit statsUpdated(cpuPercent, ramUsedKB, ramTotalKB, gpus);
}

// ---- CPU --------------------------------------------------------------
double SystemMonitor::readCpuPercent()
{
    QFile file("/proc/stat");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0.0;

    const QString line = QString::fromUtf8(file.readLine());
    file.close();

    const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 5 || parts[0] != "cpu")
        return 0.0;

    quint64 user = parts[1].toULongLong();
    quint64 nice = parts[2].toULongLong();
    quint64 system = parts[3].toULongLong();
    quint64 idle = parts[4].toULongLong();
    quint64 iowait = parts.size() > 5 ? parts[5].toULongLong() : 0;
    quint64 irq = parts.size() > 6 ? parts[6].toULongLong() : 0;
    quint64 softirq = parts.size() > 7 ? parts[7].toULongLong() : 0;
    quint64 steal = parts.size() > 8 ? parts[8].toULongLong() : 0;

    const quint64 idleAll = idle + iowait;
    const quint64 nonIdle = user + nice + system + irq + softirq + steal;
    const quint64 total = idleAll + nonIdle;

    double percent = 0.0;
    if (m_havePrevCpuSample) {
        const quint64 totalDelta = total - m_prevTotal;
        const quint64 idleDelta = idleAll - m_prevIdle;
        if (totalDelta > 0)
            percent = (double(totalDelta - idleDelta) / double(totalDelta)) * 100.0;
    }

    m_prevIdle = idleAll;
    m_prevTotal = total;
    m_havePrevCpuSample = true;

    return percent;
}

// ---- RAM ----------------------------------------------------------------
void SystemMonitor::readRamUsage(quint64 &usedKB, quint64 &totalKB)
{
    usedKB = 0;
    totalKB = 0;

    QFile file("/proc/meminfo");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    quint64 memTotal = 0;
    quint64 memAvailable = 0;

    QTextStream stream(&file);
    QString line;
    while (stream.readLineInto(&line)) {
        if (line.startsWith("MemTotal:")) {
            memTotal = line.section(' ', -2, -2).toULongLong();
        } else if (line.startsWith("MemAvailable:")) {
            memAvailable = line.section(' ', -2, -2).toULongLong();
        }
        if (memTotal && memAvailable)
            break;
    }

    totalKB = memTotal;
    usedKB = (memAvailable <= memTotal) ? (memTotal - memAvailable) : 0;
}

// ---- Card enumeration -------------------------------------------------------
// /sys/class/drm lists both real GPU nodes ("card0", "card1", ...) and
// per-connector subentries ("card0-DP-1", "card0-HDMI-A-1", ...) plus
// render-only nodes ("renderD128", ...). We only want the plain "cardN"
// entries, one per physical GPU.
void SystemMonitor::enumerateCardsIfNeeded()
{
    if (m_cardsEnumerated)
        return;
    m_cardsEnumerated = true;

    QDir drmDir("/sys/class/drm");
    const QStringList entries = drmDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

    static const QRegularExpression cardPattern("^card[0-9]+$");

    for (const QString &entry : entries) {
        if (!cardPattern.match(entry).hasMatch())
            continue;

        CardInfo card;
        card.sysfsDevicePath = drmDir.filePath(entry) + "/device";

        QFile vendorFile(card.sysfsDevicePath + "/vendor");
        if (!vendorFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString vendorId = QString::fromUtf8(vendorFile.readAll()).trimmed().toLower();

        if (vendorId == "0x10de") {
            card.vendor = GpuStat::Vendor::Nvidia;
        } else if (vendorId == "0x1002") {
            card.vendor = GpuStat::Vendor::Amd;
        } else if (vendorId == "0x8086") {
            card.vendor = GpuStat::Vendor::Intel;
        } else {
            card.vendor = GpuStat::Vendor::Unknown;
        }

        if (card.vendor == GpuStat::Vendor::Nvidia) {
            // The PCI bus id is the last path component of the resolved
            // symlink target, e.g. .../0000:01:00.0 — this is what lets us
            // match this specific card to the right NVML device instead of
            // assuming "NVML device 0", which silently breaks the moment
            // there's more than one GPU vendor in the system.
            const QString resolved = QFileInfo(card.sysfsDevicePath).canonicalFilePath();
            card.pciBusId = resolved.section('/', -1);
        }

        m_cards.append(card);
    }
}

// ---- NVIDIA via NVML ------------------------------------------------------
bool SystemMonitor::initNvml()
{
    if (m_nvmlInitAttempted)
        return m_nvmlAvailable;
    m_nvmlInitAttempted = true;

    m_nvmlHandle = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!m_nvmlHandle)
        m_nvmlHandle = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);
    if (!m_nvmlHandle)
        return false;

    p_nvmlInit_v2 = reinterpret_cast<nvmlInit_v2_t>(dlsym(m_nvmlHandle, "nvmlInit_v2"));
    p_nvmlShutdown = reinterpret_cast<nvmlShutdown_t>(dlsym(m_nvmlHandle, "nvmlShutdown"));
    p_nvmlDeviceGetHandleByPciBusId_v2 = reinterpret_cast<nvmlDeviceGetHandleByPciBusId_v2_t>(
        dlsym(m_nvmlHandle, "nvmlDeviceGetHandleByPciBusId_v2"));
    p_nvmlDeviceGetMemoryInfo = reinterpret_cast<nvmlDeviceGetMemoryInfo_t>(
        dlsym(m_nvmlHandle, "nvmlDeviceGetMemoryInfo"));
    p_nvmlDeviceGetUtilizationRates = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(
        dlsym(m_nvmlHandle, "nvmlDeviceGetUtilizationRates"));
    p_nvmlDeviceGetName = reinterpret_cast<nvmlDeviceGetName_t>(
        dlsym(m_nvmlHandle, "nvmlDeviceGetName"));

    if (!p_nvmlInit_v2 || !p_nvmlShutdown || !p_nvmlDeviceGetHandleByPciBusId_v2 || !p_nvmlDeviceGetMemoryInfo) {
        dlclose(m_nvmlHandle);
        m_nvmlHandle = nullptr;
        return false;
    }

    if (p_nvmlInit_v2() != NVML_SUCCESS) {
        dlclose(m_nvmlHandle);
        m_nvmlHandle = nullptr;
        return false;
    }

    m_nvmlAvailable = true;
    return true;
}

void SystemMonitor::shutdownNvml()
{
    if (m_nvmlAvailable && p_nvmlShutdown)
        p_nvmlShutdown();
    if (m_nvmlHandle)
        dlclose(m_nvmlHandle);
    m_nvmlHandle = nullptr;
    m_nvmlAvailable = false;
}

GpuStat SystemMonitor::readNvidiaStat(const CardInfo &card)
{
    GpuStat stat;
    stat.vendor = GpuStat::Vendor::Nvidia;
    stat.name = "NVIDIA GPU";

    if (!initNvml() || card.pciBusId.isEmpty())
        return stat;

    nvmlDevice_t device = nullptr;
    if (p_nvmlDeviceGetHandleByPciBusId_v2(card.pciBusId.toUtf8().constData(), &device) != NVML_SUCCESS)
        return stat;

    if (p_nvmlDeviceGetName) {
        char nameBuf[96] = {0};
        if (p_nvmlDeviceGetName(device, nameBuf, sizeof(nameBuf)) == NVML_SUCCESS)
            stat.name = QString::fromUtf8(nameBuf);
    }

    if (p_nvmlDeviceGetUtilizationRates) {
        nvmlUtilization_t util{};
        if (p_nvmlDeviceGetUtilizationRates(device, &util) == NVML_SUCCESS)
            stat.utilPercent = static_cast<double>(util.gpu);
    }

    nvmlMemory_t mem{};
    if (p_nvmlDeviceGetMemoryInfo(device, &mem) == NVML_SUCCESS) {
        stat.vramUsedKB = mem.used / 1024;
        stat.vramTotalKB = mem.total / 1024;
        stat.vramAvailable = true;
    }

    return stat;
}

// ---- AMD via amdgpu sysfs --------------------------------------------------
GpuStat SystemMonitor::readAmdStat(const CardInfo &card)
{
    GpuStat stat;
    stat.vendor = GpuStat::Vendor::Amd;
    stat.name = "AMD GPU";

    QFile nameFile(card.sysfsDevicePath + "/product_name");
    if (nameFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString productName = QString::fromUtf8(nameFile.readAll()).trimmed();
        if (!productName.isEmpty())
            stat.name = productName;
    }

    QFile busyFile(card.sysfsDevicePath + "/gpu_busy_percent");
    if (busyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const double percent = QString::fromUtf8(busyFile.readAll()).trimmed().toDouble(&ok);
        if (ok)
            stat.utilPercent = percent;
    }

    auto readSysfsValue = [](const QString &path, quint64 &outBytes) -> bool {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        bool ok = false;
        outBytes = QString::fromUtf8(file.readAll()).trimmed().toULongLong(&ok);
        return ok;
    };

    quint64 usedBytes = 0, totalBytes = 0;
    const bool okUsed = readSysfsValue(card.sysfsDevicePath + "/mem_info_vram_used", usedBytes);
    const bool okTotal = readSysfsValue(card.sysfsDevicePath + "/mem_info_vram_total", totalBytes);
    if (okUsed && okTotal) {
        stat.vramUsedKB = usedBytes / 1024;
        stat.vramTotalKB = totalBytes / 1024;
        stat.vramAvailable = true;
    }

    return stat;
}

// ---- Intel via i915/Xe sysfs -----------------------------------------------
// Integrated Intel graphics share system RAM rather than having a separate
// VRAM pool, so vramAvailable is deliberately left false here — the UI is
// expected to say "shared w/ RAM" rather than show an empty/misleading bar.
// Utilization exposure varies significantly by kernel/driver (i915 vs the
// newer Xe driver), so this is read best-effort and left at -1 if absent.
GpuStat SystemMonitor::readIntelStat(const CardInfo &card)
{
    GpuStat stat;
    stat.vendor = GpuStat::Vendor::Intel;
    stat.name = "Intel GPU (integrated)";

    QFile busyFile(card.sysfsDevicePath + "/gpu_busy_percent");
    if (busyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        const double percent = QString::fromUtf8(busyFile.readAll()).trimmed().toDouble(&ok);
        if (ok)
            stat.utilPercent = percent;
    }

    return stat;
}
