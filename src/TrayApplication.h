#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>

#include "SystemMonitor.h"
#include "OllamaClient.h"
#include "ServerController.h"

class MainWindow;
class ThemeManager;

// Owns the tray icon, its context menu, the live CPU/RAM/GPU tooltip, and
// server start/stop (delegated to ServerController). SystemMonitor,
// OllamaClient, and ServerController are shared with MainWindow rather than
// owned here, so polling/network calls aren't duplicated between the two.
class TrayApplication : public QObject
{
    Q_OBJECT

public:
    TrayApplication(SystemMonitor *systemMonitor,
                     OllamaClient *ollamaClient,
                     ServerController *serverController,
                     MainWindow *mainWindow,
                     ThemeManager *themeManager,
                     QObject *parent = nullptr);

private slots:
    void onStatsUpdated(double cpuPercent,
                         quint64 ramUsedKB, quint64 ramTotalKB,
                         QVector<GpuStat> gpus);
    void onOllamaReachable(bool isReachable);
    void toggleServer();
    void onServerModeChanged(ServerController::RunMode mode);
    void onServerActionSucceeded(QString description);
    void onServerActionFailed(QString reason);
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);

    // Offload-model submenu: rebuilt each time it's about to be shown
    // (fetchLoadedModels() is async, so it briefly shows "Loading…" first)
    // and again whenever a fetch actually completes, since Settings' own
    // "Offload model" section can trigger the same fetch independently.
    void onOffloadMenuAboutToShow();
    void onLoadedModelsListed(const QVector<LoadedModelInfo> &models);
    void onModelUnloaded(const QString &model, bool success);

private:
    void buildMenu();
    static QString formatKB(quint64 kb);
    static QString modeLabel(ServerController::RunMode mode);
    // Recolors the tray icon for the currently active theme — black on
    // light, white-ish (the theme's own "text" token) on dark — called at
    // construction and again whenever ThemeManager::themeChanged fires.
    void updateTrayIconForTheme();

    QSystemTrayIcon m_trayIcon;
    QMenu m_menu;

    QAction *m_statusAction = nullptr;   // "Ollama: running (systemd user) / not running" (disabled, label-only)
    QAction *m_toggleAction = nullptr;   // "Start server" / "Stop server"
    QAction *m_openAction = nullptr;     // shows MainWindow
    QAction *m_quitAction = nullptr;
    QMenu *m_offloadMenu = nullptr;      // "Offload model" submenu, populated from GET /api/ps

    // Non-owning — lifetime managed by main().
    SystemMonitor *m_systemMonitor = nullptr;
    OllamaClient *m_ollamaClient = nullptr;
    ServerController *m_serverController = nullptr;
    MainWindow *m_mainWindow = nullptr;
    ThemeManager *m_themeManager = nullptr;

    bool m_isReachable = false;
    ServerController::RunMode m_currentMode = ServerController::RunMode::Unknown;
};
