#pragma once

#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>

#include "SystemMonitor.h"
#include "OllamaClient.h"
#include "ConversationStore.h"
#include "ChatWidget.h"
#include "StatsStripWidget.h"
#include "ThemeManager.h"

// The main application window: a resizable conversation sidebar on the left
// (with a settings button pinned to its bottom-left), the active chat
// filling the middle, and a compact live CPU/RAM/GPU stats strip on the
// right — all three panes live in a QSplitter so the person can drag to
// resize them. Closing this window hides it rather than quitting the app —
// it's a tray app, quitting happens via the tray menu.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(SystemMonitor *systemMonitor,
               OllamaClient *ollamaClient,
               ConversationStore *store,
               ThemeManager *themeManager,
               QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onNewConversationClicked();
    void onSidebarSelectionChanged();
    void onConversationListChanged();
    void onModelsListed(const QStringList &modelNames);
    void onConversationTitleMayHaveChanged(const QString &conversationId);
    void onChatConversationCreated(const QString &conversationId);
    void onSidebarContextMenuRequested(const QPoint &pos);
    void onSettingsRequested();
    // Re-derives the window icon (black on light, white-ish on dark — see
    // Theme::loadThemedIconMultiSize) whenever the theme actually flips.
    // Needed because an already-shown top-level window doesn't reliably
    // pick up a later QApplication::setWindowIcon() change on its own —
    // main.cpp sets that default for windows not yet created, this keeps
    // *this* one in sync live.
    void updateWindowIconForTheme();

private:
    void refreshSidebar();

    // Selects the sidebar row for the given conversation id, if present.
    // Triggers onSidebarSelectionChanged(), which is what actually calls
    // ChatWidget::setActiveConversation().
    void selectSidebarRow(const QString &id);

    // Builds a one-shot popup menu offering "Delete chat" (red text, see
    // Theme.cpp's #deleteMenuItem rule) for the given conversation. Caller
    // owns the returned menu and should deleteLater() it after exec().
    QMenu *buildDeleteMenu(const QString &conversationId, const QString &title);

    // Shows a confirmation dialog (red "Delete" button) and, only if
    // confirmed, deletes the conversation — clearing the active chat first
    // if it's the one being removed, so ChatWidget never ends up pointing
    // at a conversation that no longer exists.
    void confirmAndDeleteConversation(const QString &conversationId, const QString &title);

    ConversationStore *m_store = nullptr;
    OllamaClient *m_ollamaClient = nullptr;
    ThemeManager *m_themeManager = nullptr;

    QListWidget *m_sidebarList = nullptr;
    QPushButton *m_newConversationButton = nullptr;
    QToolButton *m_settingsButton = nullptr; // bottom-left of the sidebar
    ChatWidget *m_chatWidget = nullptr;
    StatsStripWidget *m_statsStrip = nullptr;

    // Avoids onSidebarSelectionChanged() re-triggering setActiveConversation
    // (and therefore aborting an in-flight stream) while we're
    // programmatically rebuilding the list, e.g. after a rename or delete.
    bool m_updatingSidebarProgrammatically = false;

    QStringList m_availableModels;
};
