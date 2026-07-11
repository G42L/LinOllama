#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QTimer>
#include <QIcon>

#include "TrayApplication.h"
#include "MainWindow.h"
#include "SystemMonitor.h"
#include "OllamaClient.h"
#include "ServerController.h"
#include "ConversationStore.h"
#include "ThemeManager.h"
#include "WhisperManager.h"
#include "Theme.h"

int main(int argc, char *argv[])
{
    // Required by Qt WebEngine (used for MapEmbedWidget's live Google Maps
    // view) before QApplication exists — otherwise the first QWebEngineView
    // created can fail to render or crash. Harmless for the rest of the app
    // even on the vast majority of runs that never touch a map block.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

    QApplication app(argc, argv);

    // Needed before any QSettings use (ThemeManager persists the theme
    // choice there) — otherwise Qt falls back to per-call defaults instead
    // of a stable settings file/registry key.
    QCoreApplication::setOrganizationName("ollama-tray");
    QCoreApplication::setApplicationName("ollama-tray");

    // A tray-only app must not quit when its window is closed (it hides
    // instead, see MainWindow::closeEvent) — only the tray menu's Quit does.
    QApplication::setQuitOnLastWindowClosed(false);

    // Fusion is a fully cross-platform QStyle that doesn't fight with custom
    // QSS the way some native styles do — using it as the base is what
    // makes Theme::styleSheet() render consistently across Linux desktops,
    // not just on whichever one this was developed on.
    QApplication::setStyle("Fusion");

    // Applies the correct light/dark stylesheet immediately based on the
    // saved preference (or the OS, in Auto mode) — must exist before
    // MainWindow is constructed so the very first paint is already themed,
    // and before the window icon below, which needs to know dark-vs-light.
    ThemeManager themeManager;

    // Default icon for every window (title bar, taskbar/dock) that doesn't
    // set its own — every QDialog (Settings, etc.) picks this up
    // automatically. MainWindow sets its own copy too (see its constructor),
    // since an already-shown top-level window doesn't reliably pick up a
    // later QApplication::setWindowIcon() change on its own. TrayApplication
    // sets the tray icon itself separately, since QSystemTrayIcon doesn't
    // follow QApplication::windowIcon() either. Black on light, white-ish
    // (the theme's own "text" token) on dark — re-applied live whenever the
    // theme flips, see the connect() below.
    auto applyAppIcon = [&]() {
        QApplication::setWindowIcon(Theme::loadThemedIconMultiSize(
            ":/icons/ollama.svg", themeManager.isDarkActive(), {16, 24, 32, 48, 64, 128}, "text"));
    };
    applyAppIcon();
    QObject::connect(&themeManager, &ThemeManager::themeChanged, &app, applyAppIcon);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "Ollama GUI",
            "No system tray detected.\n\n"
            "On GNOME, install and enable the AppIndicator/KStatusNotifierItem "
            "extension — GNOME doesn't support tray icons out of the box.");
        return 1;
    }

    // Shared services — single instance each, so polling and network calls
    // aren't duplicated between the tray icon and the main window.
    SystemMonitor systemMonitor;
    OllamaClient ollamaClient;
    ServerController serverController;
    ConversationStore conversationStore;
    WhisperManager whisperManager;

    MainWindow mainWindow(&systemMonitor, &ollamaClient, &conversationStore, &themeManager, &whisperManager);
    TrayApplication trayApp(&systemMonitor, &ollamaClient, &serverController, &mainWindow, &themeManager);
    mainWindow.show();

    systemMonitor.start(1500);

    QTimer reachabilityTimer;
    QObject::connect(&reachabilityTimer, &QTimer::timeout,
                      &ollamaClient, &OllamaClient::refreshStatus);
    reachabilityTimer.start(3000);
    ollamaClient.refreshStatus();

    return app.exec();
}
