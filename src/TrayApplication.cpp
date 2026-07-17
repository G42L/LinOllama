#include "TrayApplication.h"
#include "MainWindow.h"
#include "ThemeManager.h"
#include "ConversationStore.h"
#include "Theme.h"

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QTimer>
#include <QDebug>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>

namespace {
// Icon-theme lookups (QIcon::fromTheme) are unreliable across desktop
// environments — COSMIC in particular doesn't always have a theme indexed
// the way Qt expects, and QSystemTrayIcon::show() with a null QIcon just
// renders nothing, with no error anywhere. Draw a guaranteed-valid fallback
// so the tray icon is never null, and only prefer the theme icon when it
// actually resolved to something.
QIcon fallbackIcon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#2E86AB"));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(4, 4, 56, 56);
    painter.setPen(QPen(Qt::white, 4));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, "O");
    return QIcon(pixmap);
}
}

TrayApplication::TrayApplication(SystemMonitor *systemMonitor,
                                  OllamaClient *ollamaClient,
                                  ServerController *serverController,
                                  ConversationStore *conversationStore,
                                  MainWindow *mainWindow,
                                  ThemeManager *themeManager,
                                  QObject *parent)
    : QObject(parent)
    , m_systemMonitor(systemMonitor)
    , m_ollamaClient(ollamaClient)
    , m_serverController(serverController)
    , m_conversationStore(conversationStore)
    , m_mainWindow(mainWindow)
    , m_themeManager(themeManager)
{
    buildMenu();

    updateTrayIconForTheme();
    connect(m_themeManager, &ThemeManager::themeChanged, this, &TrayApplication::updateTrayIconForTheme);

    m_trayIcon.setToolTip("Ollama GUI — starting up...");
    m_trayIcon.setContextMenu(&m_menu);
    connect(&m_trayIcon, &QSystemTrayIcon::activated,
            this, &TrayApplication::onTrayActivated);
    m_trayIcon.show();

    connect(m_systemMonitor, &SystemMonitor::statsUpdated,
            this, &TrayApplication::onStatsUpdated);

    connect(m_ollamaClient, &OllamaClient::reachable,
            this, &TrayApplication::onOllamaReachable);

    connect(m_serverController, &ServerController::modeChanged,
            this, &TrayApplication::onServerModeChanged);
    connect(m_serverController, &ServerController::actionSucceeded,
            this, &TrayApplication::onServerActionSucceeded);
    connect(m_serverController, &ServerController::actionFailed,
            this, &TrayApplication::onServerActionFailed);
    m_serverController->detectMode();

    connect(m_ollamaClient, &OllamaClient::loadedModelsListed,
            this, &TrayApplication::onLoadedModelsListed);
    connect(m_ollamaClient, &OllamaClient::modelUnloaded,
            this, &TrayApplication::onModelUnloaded);
}

void TrayApplication::updateTrayIconForTheme()
{
    // A handful of sizes rendered fresh from the vector source (not one
    // pixmap scaled up) — desktop environments request different pixel
    // sizes for the tray depending on panel size/DPI.
    QIcon icon = Theme::loadThemedIconMultiSize(
        ":/icons/ollama.svg", m_themeManager->isDarkActive(), {16, 22, 24, 32, 48, 64}, "text");
    if (icon.isNull()) {
        qDebug() << "Bundled ollama.svg icon failed to load, using drawn fallback";
        icon = fallbackIcon();
    }
    m_trayIcon.setIcon(icon);
}

void TrayApplication::buildMenu()
{
    m_statusAction = m_menu.addAction("Ollama: checking...");
    m_statusAction->setEnabled(false);

    m_menu.addSeparator();

    m_toggleAction = m_menu.addAction("Start server", this, &TrayApplication::toggleServer);

    m_offloadMenu = m_menu.addMenu("Offload model");
    connect(m_offloadMenu, &QMenu::aboutToShow, this, &TrayApplication::onOffloadMenuAboutToShow);
    // Some native tray backends (system tray icons are typically rendered
    // by the desktop environment itself over DBusMenu/StatusNotifierItem,
    // not painted by Qt) serialize the whole menu tree once when the
    // top-level tray menu opens, and don't reliably forward a *nested*
    // submenu's own lazy aboutToShow back to the app afterward — which
    // left "Offload model" stuck empty/on "Loading…" on some desktops even
    // though the same fetch works fine from Settings (which fetches
    // eagerly on open, with no dependency on a hover event at all). Also
    // triggering the fetch off the top-level menu's own aboutToShow — which
    // every backend has to support, since that's how the menu gets shown
    // at all — means the submenu already has fresh data by the time it's
    // opened, regardless of whether its own aboutToShow round-trips.
    connect(&m_menu, &QMenu::aboutToShow, this, &TrayApplication::onOffloadMenuAboutToShow);

    m_backupMenu = m_menu.addMenu("Backup and restore");
    m_backupMenu->addAction("Export all conversations…", this, &TrayApplication::onExportAllConversationsClicked);
    m_backupMenu->addAction("Import conversations…", this, &TrayApplication::onImportConversationsClicked);
    m_backupMenu->addSeparator();
    m_backupMenu->addAction("Export settings…", this, &TrayApplication::onExportSettingsClicked);
    m_backupMenu->addAction("Import settings…", this, &TrayApplication::onImportSettingsClicked);

    m_openAction = m_menu.addAction("Open Ollama GUI", this, [this]() {
        if (!m_mainWindow)
            return;
        m_mainWindow->show();
        m_mainWindow->raise();
        m_mainWindow->activateWindow();
    });

    m_menu.addSeparator();
    m_quitAction = m_menu.addAction("Quit", qApp, &QApplication::quit);
}

void TrayApplication::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    // Left-click (Trigger) opens the main window; right-click already opens
    // the context menu via setContextMenu(), handled natively.
    if (reason == QSystemTrayIcon::Trigger && m_openAction) {
        m_openAction->trigger();
    }
}

void TrayApplication::onStatsUpdated(double cpuPercent,
                                      quint64 ramUsedKB, quint64 ramTotalKB,
                                      QVector<GpuStat> gpus)
{
    QString tooltip;
    tooltip += QString("CPU: %1%\n").arg(cpuPercent, 0, 'f', 0);
    tooltip += QString("RAM: %1 / %2").arg(formatKB(ramUsedKB), formatKB(ramTotalKB));

    for (const GpuStat &gpu : gpus) {
        tooltip += QString("\n%1: ").arg(gpu.name);
        tooltip += (gpu.utilPercent >= 0.0)
            ? QString("%1%").arg(gpu.utilPercent, 0, 'f', 0)
            : "usage n/a";
        if (gpu.vramAvailable)
            tooltip += QString(" (%1 / %2)").arg(formatKB(gpu.vramUsedKB), formatKB(gpu.vramTotalKB));
    }

    m_trayIcon.setToolTip(tooltip);
}

void TrayApplication::onOllamaReachable(bool isReachable)
{
    m_isReachable = isReachable;
    m_statusAction->setText(isReachable
        ? QString("Ollama: running (%1)").arg(modeLabel(m_currentMode))
        : "Ollama: not running");
    m_toggleAction->setText(isReachable ? "Stop server" : "Start server");

    // Reachability changing is a good moment to re-detect run mode too —
    // e.g. it just came up, so figure out whether that was us, systemd, or
    // someone in a terminal.
    m_serverController->detectMode();
}

void TrayApplication::toggleServer()
{
    // Disable immediately to prevent double-clicks firing overlapping
    // start/stop commands while the first one is still in flight.
    m_toggleAction->setEnabled(false);

    if (m_isReachable)
        m_serverController->stop();
    else
        m_serverController->start();
}

void TrayApplication::onServerModeChanged(ServerController::RunMode mode)
{
    m_currentMode = mode;
    if (m_isReachable)
        m_statusAction->setText(QString("Ollama: running (%1)").arg(modeLabel(mode)));
}

void TrayApplication::onServerActionSucceeded(QString description)
{
    qDebug() << "Server action succeeded:" << description;
    m_toggleAction->setEnabled(true);
    // Give the server/systemd a moment to actually bind the port before
    // checking reachability — an immediate check right after "start"
    // returns would often still read as unreachable.
    QTimer::singleShot(800, m_ollamaClient, &OllamaClient::refreshStatus);
}

void TrayApplication::onServerActionFailed(QString reason)
{
    qDebug() << "Server action failed:" << reason;
    m_toggleAction->setEnabled(true);
    m_trayIcon.showMessage("Ollama QUI", reason, QSystemTrayIcon::Warning, 4000);
}

void TrayApplication::onOffloadMenuAboutToShow()
{
    m_offloadMenu->clear();
    QAction *loading = m_offloadMenu->addAction("Loading…");
    loading->setEnabled(false);
    m_ollamaClient->fetchLoadedModels();
}

void TrayApplication::onLoadedModelsListed(const QVector<LoadedModelInfo> &models)
{
    m_offloadMenu->clear();

    if (models.isEmpty()) {
        QAction *none = m_offloadMenu->addAction("No models currently loaded");
        none->setEnabled(false);
        return;
    }

    for (const LoadedModelInfo &info : models) {
        const QString label = info.sizeVramBytes > 0
            ? QString("%1 (%2)").arg(info.name, formatKB(info.sizeVramBytes / 1024))
            : info.name;
        QAction *action = m_offloadMenu->addAction(label);
        connect(action, &QAction::triggered, this, [this, name = info.name]() {
            m_ollamaClient->unloadModel(name);
        });
    }
}

void TrayApplication::onModelUnloaded(const QString &model, bool success)
{
    m_trayIcon.showMessage("Ollama GUI",
        success ? QString("Offloaded model: %1").arg(model)
                : QString("Failed to offload model: %1").arg(model),
        success ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning, 3000);
}

QString TrayApplication::modeLabel(ServerController::RunMode mode)
{
    switch (mode) {
    case ServerController::RunMode::SystemdUser:   return "systemd user service";
    case ServerController::RunMode::SystemdSystem: return "systemd system service";
    case ServerController::RunMode::Loose:         return "plain process";
    case ServerController::RunMode::NotRunning:    return "not running";
    case ServerController::RunMode::Unknown:
    default:                                       return "unknown";
    }
}

void TrayApplication::onExportAllConversationsClicked()
{
    const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/LinOllama-conversations.json";
    const QString path = QFileDialog::getSaveFileName(
        m_mainWindow, "Export all conversations", defaultPath, "JSON files (*.json)");
    if (path.isEmpty())
        return;

    if (!m_conversationStore->exportAll(path)) {
        QMessageBox::warning(m_mainWindow, "Export failed", "Couldn't write to \"" + path + "\".");
    } else {
        m_trayIcon.showMessage("LinOllama", "Exported all conversations to \"" + path + "\".",
                                QSystemTrayIcon::Information, 3000);
    }
}

void TrayApplication::onImportConversationsClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        m_mainWindow, "Import conversations", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "JSON files (*.json);;All files (*)");
    if (path.isEmpty())
        return;

    const int count = m_conversationStore->importAll(path);
    if (count < 0) {
        QMessageBox::warning(m_mainWindow, "Import failed",
            "\"" + path + "\" doesn't look like a LinOllama conversations export.");
    } else {
        m_trayIcon.showMessage("LinOllama", QString("Imported %1 conversation(s).").arg(count),
                                QSystemTrayIcon::Information, 3000);
    }
}

void TrayApplication::onExportSettingsClicked()
{
    const QString sourcePath = QSettings().fileName();
    const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/LinOllama-settings.conf";
    const QString path = QFileDialog::getSaveFileName(
        m_mainWindow, "Export settings", defaultPath, "Settings files (*.conf);;All files (*)");
    if (path.isEmpty())
        return;

    QFile::remove(path); // QFile::copy() refuses to overwrite an existing file
    if (!QFile::copy(sourcePath, path))
        QMessageBox::warning(m_mainWindow, "Export failed", "Couldn't write to \"" + path + "\".");
}

void TrayApplication::onImportSettingsClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        m_mainWindow, "Import settings", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "Settings files (*.conf);;All files (*)");
    if (path.isEmpty())
        return;

    if (QMessageBox::question(m_mainWindow, "Import settings",
            "This replaces every current setting with the ones from \"" + path + "\". "
            "Restart LinOllama afterward for everything to take effect. Continue?")
        != QMessageBox::Yes)
        return;

    const QString destPath = QSettings().fileName();
    QFile::remove(destPath);
    if (!QFile::copy(path, destPath)) {
        QMessageBox::warning(m_mainWindow, "Import failed", "Couldn't read \"" + path + "\".");
        return;
    }

    QMessageBox::information(m_mainWindow, "Import settings",
        "Settings imported. Restart LinOllama for all changes to take effect.");
}

QString TrayApplication::formatKB(quint64 kb)
{
    // Simple binary-prefix formatter; good enough for a tooltip, worth
    // swapping for QLocale::formattedDataSize if/when Qt6 baseline allows.
    constexpr double MB = 1024.0;
    constexpr double GB = 1024.0 * 1024.0;

    if (kb >= static_cast<quint64>(GB))
        return QString("%1 GB").arg(kb / GB, 0, 'f', 1);
    return QString("%1 MB").arg(kb / MB, 0, 'f', 0);
}
