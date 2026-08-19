#pragma once

#include <QMainWindow>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QSplitter>
#include <QLineEdit>
#include <QTimer>

#include "SystemMonitor.h"
#include "OllamaClient.h"
#include "ConversationStore.h"
#include "ChatWidget.h"
#include "StatsStripWidget.h"
#include "ThemeManager.h"
#include "WhisperManager.h"

// The main application window: a persistent top bar (new-conversation +
// sidebar collapse/expand, both icon-only) above a resizable conversation
// sidebar on the left (with a settings button pinned to its bottom-left),
// the active chat filling the middle, and a compact live CPU/RAM/GPU stats
// strip on the right — sidebar/chat/stats live in a QSplitter so the person
// can drag to resize them, while the top bar sits outside it so
// new-conversation and the collapse toggle stay reachable even when the
// sidebar itself is fully collapsed (Settings, being inside the sidebar,
// isn't). Closing this window hides it rather than quitting the app — it's
// a tray app, quitting happens via the tray menu.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(SystemMonitor *systemMonitor,
               OllamaClient *ollamaClient,
               ConversationStore *store,
               ThemeManager *themeManager,
               WhisperManager *whisperManager,
               QWidget *parent = nullptr);

    // Deletes the active conversation if it's still completely empty (see
    // ChatWidget::discardConversationIfEmpty) — connected to
    // QApplication::aboutToQuit in main.cpp so a "New conversation" left
    // open when the app quits doesn't linger on disk.
    void discardEmptyActiveConversation();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Watches m_sidebarList for QEvent::Resize (e.g. dragging the splitter)
    // to keep every row's item sizeHint matching the list's actual width —
    // see updateSidebarItemWidths().
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onNewConversationClicked();
    void onImportConversationClicked();
    void onSidebarSelectionChanged();
    void onConversationListChanged();
    void onModelsListed(const QStringList &modelNames);
    void onConversationTitleMayHaveChanged(const QString &conversationId);
    void onChatConversationCreated(const QString &conversationId);
    void onSidebarContextMenuRequested(const QPoint &pos);
    // Restarts m_sidebarSearchDebounceTimer on every keystroke in
    // m_sidebarSearchEdit — see the timer's own comment for why filtering
    // is debounced rather than applied straight from textChanged().
    void onSidebarSearchTextChanged(const QString &text);
    // Double-clicking a sidebar row is a third entry point into the same
    // rename flow as the context menu's "Rename…" and the row's own "⋮"
    // menu — see promptRenameConversation().
    void onSidebarItemDoubleClicked(QListWidgetItem *item);
    void onSettingsRequested();
    // Re-derives the window icon (black on light, white-ish on dark — see
    // Theme::loadThemedIconMultiSize) whenever the theme actually flips.
    // Needed because an already-shown top-level window doesn't reliably
    // pick up a later QApplication::setWindowIcon() change on its own —
    // main.cpp sets that default for windows not yet created, this keeps
    // *this* one in sync live.
    void updateWindowIconForTheme();
    // Toggles the sidebar between shown and fully hidden — see
    // setSidebarCollapsed().
    void onSidebarToggleClicked();
    // Populates m_ollamaVersionLabel once fetchServerVersion() resolves —
    // blank (hidden) if the server isn't reachable or predates /api/version.
    void onServerVersionFetched(const QString &version);
    // Shows/hides m_updateAvailableButton once OllamaClient::checkForUpdate()
    // resolves — see its own comment for why this piggybacks on the version
    // fetch rather than having its own timer.
    void onUpdateCheckFinished(bool available, const QString &latestVersion);
    // Confirms with the user, then runs Ollama's own official install
    // script (curl | sh) in a terminal so the user can see progress and
    // answer any sudo password prompt — see .cpp for why a bare QProcess
    // isn't enough.
    void onUpdateOllamaClicked();

private:
    void refreshSidebar();
    // Sets every sidebar row's item sizeHint width to the list's actual
    // viewport width (instead of leaving it at whatever ConversationListItemWidget::
    // sizeHint() naturally reports, which reflects its *full, un-elided*
    // title — since QListWidget sizes each row's widget to its own item's
    // sizeHint rather than stretching it to the viewport, a long title would
    // otherwise make the row genuinely wider than the visible sidebar, and
    // with horizontal scrolling disabled, that overflow just gets silently
    // clipped by the viewport instead of ever reaching
    // ConversationListItemWidget's own eliding logic). Called after
    // rebuilding the list and on every list resize (see eventFilter()).
    void updateSidebarItemWidths();

    // True if conv should be shown under the current sidebar search filter
    // — matches on title OR any message's content (case-insensitive plain
    // substring, not a query language). Called from refreshSidebar()'s own
    // rebuild loop; an empty filter always matches everything.
    bool conversationMatchesFilter(const Conversation &conv, const QString &filter) const;

    // Applies the given collapsed state: hides/shows the whole sidebar pane
    // (remembering its expanded width so re-expanding restores it, rather
    // than snapping back to a fixed default) and swaps the toggle button's
    // icon/tooltip. New-conversation and Settings live in the top bar, not
    // the sidebar itself, so they're unaffected either way. Called by
    // onSidebarToggleClicked() and once at construction to set the initial
    // (expanded) state's icon.
    void setSidebarCollapsed(bool collapsed);
    // Re-derives the new-conversation and sidebar-toggle icons for the
    // current theme — called at construction and on ThemeManager::themeChanged,
    // same pattern as ChatWidget::reloadThemedIcons().
    void reloadSidebarIcons();

    // Selects the sidebar row for the given conversation id, if present.
    // Triggers onSidebarSelectionChanged(), which is what actually calls
    // ChatWidget::setActiveConversation().
    void selectSidebarRow(const QString &id);

    // Builds a one-shot popup menu offering "Rename…", "Export
    // conversation…", and "Delete chat" (the latter in red text, see
    // Theme.cpp's #deleteMenuItem rule) for the given conversation. Caller
    // owns the returned menu and should deleteLater() it after exec().
    QMenu *buildConversationContextMenu(const QString &conversationId, const QString &title);

    // Shared by all three rename entry points (sidebar right-click menu,
    // each row's own "⋮" menu, and double-clicking a row) — prompts with
    // the conversation's current title pre-filled, and if a non-empty,
    // actually-changed title is given, persists it via
    // ConversationStore::renameConversation(), which itself emits
    // conversationListChanged() to refresh the sidebar.
    void promptRenameConversation(const QString &conversationId, const QString &currentTitle);

    // Shows a confirmation dialog (red "Delete" button) and, only if
    // confirmed, deletes the conversation — clearing the active chat first
    // if it's the one being removed, so ChatWidget never ends up pointing
    // at a conversation that no longer exists.
    void confirmAndDeleteConversation(const QString &conversationId, const QString &title);

    // Prompts for a save location (defaulting to a sanitized version of the
    // conversation's own title as the filename) and writes the
    // conversation's own on-disk JSON shape (see Conversation::toJson()) to
    // it verbatim — the exported file is byte-for-byte the same schema this
    // app already persists internally, just copied somewhere the person
    // chose rather than the app's own conversations folder. Shows an error
    // dialog on write failure rather than failing silently.
    void exportConversation(const QString &conversationId, const QString &title);

    ConversationStore *m_store = nullptr;
    OllamaClient *m_ollamaClient = nullptr;
    ThemeManager *m_themeManager = nullptr;
    WhisperManager *m_whisperManager = nullptr;

    QWidget *m_sidebar = nullptr; // the whole left pane, shown/hidden by setSidebarCollapsed()
    // Filters m_sidebarList by title/message content — see
    // conversationMatchesFilter() and refreshSidebar(). Sits above the list,
    // inside the sidebar pane (so it collapses/hides along with it).
    QLineEdit *m_sidebarSearchEdit = nullptr;
    QString m_sidebarSearchFilter;
    // Debounces m_sidebarSearchEdit's textChanged() before it triggers a
    // full refreshSidebar() rebuild — refreshSidebar() reallocates one
    // ConversationListItemWidget per visible conversation on every call, so
    // firing it on every single keystroke of a fast typist would be wasteful
    // (each is a full widget-tree rebuild, not just a filter pass). 150ms
    // single-shot, restarted on each keystroke, same idea as any other
    // "wait for typing to pause" debounce.
    QTimer *m_sidebarSearchDebounceTimer = nullptr;
    QListWidget *m_sidebarList = nullptr;
    // All three live in the persistent top bar (not the sidebar pane
    // itself), icon-only, left to right: new conversation, sidebar
    // collapse/expand, [stretch], settings — so new-conversation and
    // settings both stay reachable even while the sidebar is collapsed.
    QToolButton *m_newConversationButton = nullptr;
    // Between new-conversation and the sidebar collapse toggle — imports a
    // conversation previously exported via a sidebar row's right-click menu
    // (see exportConversation()/onImportConversationClicked()).
    QToolButton *m_importConversationButton = nullptr;
    QToolButton *m_sidebarToggleButton = nullptr;
    QToolButton *m_settingsButton = nullptr;
    // Far right of the top bar (after the stretch) — Ollama's own server
    // version, e.g. "v0.5.4". Hidden (empty text) until fetched, and again
    // if the server isn't reachable — see onServerVersionFetched().
    QLabel *m_ollamaVersionLabel = nullptr;
    // Hidden until onUpdateCheckFinished() reports a newer Ollama release is
    // out; clicking it runs the official install script to update in place
    // (see onUpdateOllamaClicked()). Sits right next to m_ollamaVersionLabel.
    QToolButton *m_updateAvailableButton = nullptr;
    // The running server's own version string, e.g. "0.5.4" — cached here so
    // onUpdateCheckFinished() can be re-derived without another /api/version
    // round trip, and so a later reachable(true) can pass it back into
    // checkForUpdate() again.
    QString m_currentOllamaVersion;
    ChatWidget *m_chatWidget = nullptr;
    StatsStripWidget *m_statsStrip = nullptr;
    QSplitter *m_splitter = nullptr;

    // Avoids onSidebarSelectionChanged() re-triggering setActiveConversation
    // (and therefore aborting an in-flight stream) while we're
    // programmatically rebuilding the list, e.g. after a rename or delete.
    bool m_updatingSidebarProgrammatically = false;

    bool m_sidebarCollapsed = false;
    // The sidebar pane's width right before it was last collapsed, restored
    // on expand instead of snapping back to a fixed default — 220 (the
    // splitter's own initial size) until the user actually resizes it once.
    int m_expandedSidebarWidth = 220;

    QStringList m_availableModels;
};
