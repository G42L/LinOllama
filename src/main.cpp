#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QTimer>
#include <QIcon>
#include <QWebEnginePage>
#include <QWebEngineProfile>

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

    // Chromium's own GPU process can fail outright on some setups even when
    // a real GPU is present — seen as repeated "eglCreateContext failed
    // with error EGL_BAD_CONTEXT" / "SharedImageStub: unable to create
    // context" in the log, most likely a Wayland/EGL platform mismatch
    // inside Chromium itself rather than anything this app controls. None
    // of what actually gets rendered here (basic HTML/CSS, 2D <canvas>
    // charts, map tiles) needs GPU acceleration, so disabling it outright
    // avoids the failure entirely instead of hoping Chromium's software
    // fallback recovers cleanly on its own. Must be set before
    // QApplication — Chromium reads its command-line flags when the
    // WebEngine subsystem first initializes.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");

    QApplication app(argc, argv);

    // Pre-warms QtWebEngine's Chromium subprocess right at launch rather
    // than letting it happen lazily on whatever MapEmbedWidget/
    // HtmlEmbedWidget gets constructed first. The very first QWebEnginePage
    // in a process spins up a whole separate Chromium subprocess (plus,
    // apparently on this Wayland/EGL setup, some GPU/compositor
    // initialization even with GPU disabled — see QTWEBENGINE_CHROMIUM_FLAGS
    // above), and doing that for the first time deep inside a mouse click's
    // own event handling (selecting a chat that turned out to contain a
    // ```html reply) was observed to visibly disrupt the whole window (a
    // brief flicker) and corrupt Qt's own mouse press/release tracking on
    // the sidebar, leaving it stuck auto-scrolling on mouse *move* until
    // another click reset it.
    //
    // Deliberately a bare QWebEnginePage, NOT a QWebEngineView — the actual
    // Chromium subprocess/renderer spin-up lives in QWebEnginePage; the
    // view is just the QWidget that displays its output. An earlier version
    // of this used a QWebEngineView moved off-screen via move(-10000,-10000),
    // which still creates a real, mapped top-level native window — Wayland
    // (unlike X11) doesn't let a client position its own window at all, so
    // that "off-screen" window could land anywhere the compositor chose,
    // which is what was actually causing a visible flicker/relocation of
    // the whole app on launch. A page with no view attached never creates
    // any native window in the first place, sidestepping that entirely.
    {
        auto *warmupProfile = new QWebEngineProfile(); // off-the-record; separate from HtmlEmbedWidget's own shared one, but the Chromium subprocess/GPU init this is actually paying for is shared process-wide either way
        auto *warmupPage = new QWebEnginePage(warmupProfile);
        QObject::connect(warmupPage, &QWebEnginePage::loadFinished, warmupPage,
                          [warmupPage, warmupProfile](bool) {
            warmupPage->deleteLater();
            warmupProfile->deleteLater();
        });
        warmupPage->setHtml(QStringLiteral("<html></html>"));
    }

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
