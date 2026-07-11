#include "MainWindow.h"
#include "ConversationListItemWidget.h"
#include "SettingsDialog.h"
#include "Theme.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QCloseEvent>
#include <QLabel>
#include <QFrame>
#include <QMessageBox>
#include <QWidgetAction>

MainWindow::MainWindow(SystemMonitor *systemMonitor,
                        OllamaClient *ollamaClient,
                        ConversationStore *store,
                        ThemeManager *themeManager,
                        WhisperManager *whisperManager,
                        QWidget *parent)
    : QMainWindow(parent)
    , m_store(store)
    , m_ollamaClient(ollamaClient)
    , m_themeManager(themeManager)
    , m_whisperManager(whisperManager)
{
    setWindowTitle("Ollama GUI");
    resize(1000, 650);

    updateWindowIconForTheme();
    connect(m_themeManager, &ThemeManager::themeChanged, this, &MainWindow::updateWindowIconForTheme);

    // --- Window-level top bar --------------------------------------------
    // Lives *above* the sidebar/chat/stats splitter (not inside the sidebar
    // pane) — new-conversation and the sidebar collapse/expand toggle stay
    // reachable here even once the sidebar itself is fully hidden, see
    // setSidebarCollapsed(). Settings lives in the sidebar itself instead
    // (see below), so it's only reachable while the sidebar is expanded.
    auto *topBar = new QWidget;
    topBar->setObjectName("windowTopBar");
    auto *topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(10, 6, 10, 6);
    topBarLayout->setSpacing(4);

    m_newConversationButton = new QToolButton;
    m_newConversationButton->setObjectName("newConversationButton");
    m_newConversationButton->setToolTip("New conversation");
    m_newConversationButton->setIconSize(QSize(16, 16));
    m_newConversationButton->setAutoRaise(true);
    m_newConversationButton->setCursor(Qt::PointingHandCursor);
    connect(m_newConversationButton, &QToolButton::clicked,
            this, &MainWindow::onNewConversationClicked);
    topBarLayout->addWidget(m_newConversationButton);

    m_sidebarToggleButton = new QToolButton;
    m_sidebarToggleButton->setObjectName("sidebarToggleButton");
    m_sidebarToggleButton->setIconSize(QSize(16, 16));
    m_sidebarToggleButton->setAutoRaise(true);
    m_sidebarToggleButton->setCursor(Qt::PointingHandCursor);
    connect(m_sidebarToggleButton, &QToolButton::clicked, this, &MainWindow::onSidebarToggleClicked);
    topBarLayout->addWidget(m_sidebarToggleButton);

    topBarLayout->addStretch(1);

    // --- Sidebar --------------------------------------------------------
    m_sidebar = new QWidget;
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setMinimumWidth(160); // draggable via the splitter below, not fixed
    auto *sidebarLayout = new QVBoxLayout(m_sidebar);
    sidebarLayout->setContentsMargins(8, 8, 8, 8);
    sidebarLayout->setSpacing(6);

    m_sidebarList = new QListWidget;
    m_sidebarList->setFrameShape(QFrame::NoFrame);
    // Long titles are elided (see ConversationListItemWidget), not
    // scrolled — this is belt-and-suspenders in case anything else ever
    // overflows a row's width.
    m_sidebarList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebarList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sidebarList, &QListWidget::currentItemChanged,
            this, &MainWindow::onSidebarSelectionChanged);
    connect(m_sidebarList, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onSidebarContextMenuRequested);
    m_sidebarList->installEventFilter(this); // keeps row widths in sync on resize — see updateSidebarItemWidths()
    sidebarLayout->addWidget(m_sidebarList, /*stretch=*/1);

    // Bottom-left of the sidebar, below the conversation list.
    m_settingsButton = new QToolButton;
    m_settingsButton->setObjectName("settingsButton");
    m_settingsButton->setText(QString::fromUtf8("\xE2\x9A\x99")); // "⚙" gear
    m_settingsButton->setToolTip("Settings");
    m_settingsButton->setAutoRaise(true);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    connect(m_settingsButton, &QToolButton::clicked, this, &MainWindow::onSettingsRequested);
    sidebarLayout->addWidget(m_settingsButton, 0, Qt::AlignLeft);

    // --- Chat -------------------------------------------------------------
    m_chatWidget = new ChatWidget(ollamaClient, store, themeManager, whisperManager);
    connect(m_chatWidget, &ChatWidget::conversationTitleMayHaveChanged,
            this, &MainWindow::onConversationTitleMayHaveChanged);
    connect(m_chatWidget, &ChatWidget::conversationCreated,
            this, &MainWindow::onChatConversationCreated);

    // --- Stats strip --------------------------------------------------------
    m_statsStrip = new StatsStripWidget(systemMonitor, themeManager);
    connect(m_chatWidget, &ChatWidget::audioLevelChanged,
            m_statsStrip, &StatsStripWidget::setMicLevel);

    // All three panes live in one splitter so every boundary between them
    // is drag-resizable, sidebar included. setChildrenCollapsible(false)
    // stops a drag from accidentally hiding a pane entirely — that read as
    // a bug the first few times, not a feature.
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_chatWidget);
    m_splitter->addWidget(m_statsStrip);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1); // chat is the only pane that should grab extra space
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({220, 600, 180});
    m_splitter->setChildrenCollapsible(false);

    auto *central = new QWidget;
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(topBar);
    centralLayout->addWidget(m_splitter, /*stretch=*/1);
    setCentralWidget(central);

    reloadSidebarIcons();
    connect(m_themeManager, &ThemeManager::themeChanged, this, &MainWindow::reloadSidebarIcons);

    connect(m_store, &ConversationStore::conversationListChanged,
            this, &MainWindow::onConversationListChanged);
    connect(m_ollamaClient, &OllamaClient::modelsListed,
            this, &MainWindow::onModelsListed);

    m_store->loadAll();
    refreshSidebar();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Tray app convention: closing the window just hides it, doesn't quit
    // the process. Quitting is a deliberate action from the tray menu.
    hide();
    event->ignore();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_sidebarList && event->type() == QEvent::Resize)
        updateSidebarItemWidths();
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onNewConversationClicked()
{
    const QString defaultModel = m_availableModels.isEmpty() ? QString() : m_availableModels.first();
    const QString id = m_store->createConversation(defaultModel);
    refreshSidebar();
    selectSidebarRow(id);
}

void MainWindow::onChatConversationCreated(const QString &conversationId)
{
    refreshSidebar();
    selectSidebarRow(conversationId);
}

void MainWindow::selectSidebarRow(const QString &id)
{
    for (int i = 0; i < m_sidebarList->count(); ++i) {
        if (m_sidebarList->item(i)->data(Qt::UserRole).toString() == id) {
            m_sidebarList->setCurrentRow(i);
            break;
        }
    }
}

void MainWindow::onSidebarSelectionChanged()
{
    if (m_updatingSidebarProgrammatically)
        return;

    QListWidgetItem *item = m_sidebarList->currentItem();
    const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    m_chatWidget->setActiveConversation(id);
}

void MainWindow::onConversationListChanged()
{
    refreshSidebar();
}

void MainWindow::onConversationTitleMayHaveChanged(const QString &conversationId)
{
    // ConversationStore doesn't broadcast on every message append (see its
    // header comment — avoids sidebar churn mid-stream), so ChatWidget tells
    // us directly the one time it matters: right after the first user
    // message gave a "New conversation" a real title.
    Q_UNUSED(conversationId);
    refreshSidebar();
}

void MainWindow::onModelsListed(const QStringList &modelNames)
{
    m_availableModels = modelNames;
    m_chatWidget->setAvailableModels(modelNames);
}

void MainWindow::onSettingsRequested()
{
    SettingsDialog dialog(m_themeManager, m_ollamaClient, m_whisperManager, this);
    connect(&dialog, &SettingsDialog::sendButtonStyleChanged,
            m_chatWidget, &ChatWidget::setSendButtonStyle);
    connect(&dialog, &SettingsDialog::sendButtonFilledChanged,
            m_chatWidget, &ChatWidget::setSendButtonFilled);
    connect(&dialog, &SettingsDialog::voiceAutoSendChanged,
            m_chatWidget, &ChatWidget::setVoiceAutoSend);
    connect(&dialog, &SettingsDialog::statsColorsChanged,
            m_statsStrip, &StatsStripWidget::applyMeterColors);
    connect(&dialog, &SettingsDialog::contextLengthSettingChanged,
            m_chatWidget, &ChatWidget::refreshContextLengthSetting);
    connect(&dialog, &SettingsDialog::modelOptimizationChanged,
            m_chatWidget, &ChatWidget::setModelOptimizationEnabled);
    connect(&dialog, &SettingsDialog::audioInputDeviceChanged,
            m_chatWidget, &ChatWidget::refreshAudioInputDevice);
    connect(&dialog, &SettingsDialog::meterSmoothingChanged,
            m_chatWidget, &ChatWidget::refreshMeterSmoothing);
    dialog.exec();
}

void MainWindow::updateWindowIconForTheme()
{
    setWindowIcon(Theme::loadThemedIconMultiSize(
        ":/icons/ollama.svg", m_themeManager->isDarkActive(), {16, 24, 32, 48, 64, 128}, "text"));
}

void MainWindow::onSidebarToggleClicked()
{
    setSidebarCollapsed(!m_sidebarCollapsed);
}

void MainWindow::setSidebarCollapsed(bool collapsed)
{
    m_sidebarCollapsed = collapsed;

    if (collapsed) {
        // Remember the real (pre-collapse) width so expanding restores it
        // exactly, rather than snapping back to some fixed default.
        const QList<int> sizes = m_splitter->sizes();
        if (!sizes.isEmpty() && sizes[0] > 0)
            m_expandedSidebarWidth = sizes[0];

        // Hiding the widget (not just narrowing it) is what lets the chat
        // pane actually claim the full reclaimed width — QSplitter treats a
        // hidden child as zero-size and gives its space to the visible
        // siblings automatically.
        m_sidebar->setVisible(false);
    } else {
        m_sidebar->setVisible(true);
        QList<int> sizes = m_splitter->sizes();
        if (sizes.size() >= 2) {
            const int delta = m_expandedSidebarWidth - sizes[0];
            sizes[0] = m_expandedSidebarWidth;
            sizes[1] = qMax(0, sizes[1] - delta); // chat pane gives back exactly what the sidebar reclaims
            m_splitter->setSizes(sizes);
        }
    }

    reloadSidebarIcons();
}

void MainWindow::reloadSidebarIcons()
{
    const bool dark = m_themeManager->isDarkActive();
    // Same "secondaryText" tone as the other icon-only toolbar buttons —
    // now that new-conversation lives in the top bar rather than a bordered
    // sidebar button, it should read as a toolbar icon, not a CTA.
    m_newConversationButton->setIcon(Theme::loadThemedIcon(":/icons/new-chat.svg", dark, 16, "secondaryText"));

    m_sidebarToggleButton->setIcon(Theme::loadThemedIcon(
        m_sidebarCollapsed ? ":/icons/sidebar-expand.svg" : ":/icons/sidebar-collapse.svg",
        dark, 16, "secondaryText"));
    m_sidebarToggleButton->setToolTip(m_sidebarCollapsed ? "Expand sidebar" : "Collapse sidebar");
}

void MainWindow::onSidebarContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem *item = m_sidebarList->itemAt(pos);
    if (!item)
        return;

    // Right-clicking a row also selects it, matching the common list
    // convention (and matches what the row's own "⋮" button implicitly does,
    // since that button only shows once you're already hovering the row).
    m_sidebarList->setCurrentItem(item);

    const QString id = item->data(Qt::UserRole).toString();
    const Conversation *conv = m_store->find(id);
    const QString title = conv ? conv->title : QString();

    QMenu *menu = buildDeleteMenu(id, title);
    menu->exec(m_sidebarList->viewport()->mapToGlobal(pos));
    menu->deleteLater();
}

QMenu *MainWindow::buildDeleteMenu(const QString &conversationId, const QString &title)
{
    auto *menu = new QMenu(this);

    // A styled QLabel rather than a plain QAction so "Delete chat" can be
    // given red "danger" text — see Theme.cpp's #deleteMenuItem rule. This
    // is the same trick ConversationListItemWidget uses for its own "⋮"
    // menu, so both entry points look identical.
    auto *deleteLabel = new QLabel("  Delete chat  ");
    deleteLabel->setObjectName("deleteMenuItem");

    auto *deleteAction = new QWidgetAction(menu);
    deleteAction->setDefaultWidget(deleteLabel);
    menu->addAction(deleteAction);

    connect(deleteAction, &QAction::triggered, this, [this, conversationId, title]() {
        confirmAndDeleteConversation(conversationId, title);
    });

    return menu;
}

void MainWindow::confirmAndDeleteConversation(const QString &conversationId, const QString &title)
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle("Delete chat");
    box.setText(title.isEmpty()
        ? "Delete this conversation?"
        : QString("Delete \"%1\"?").arg(title));
    box.setInformativeText("This can't be undone.");

    QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
    QPushButton *deleteButton = box.addButton("Delete", QMessageBox::DestructiveRole);
    deleteButton->setObjectName("dangerButton"); // red styling comes from the app stylesheet
    box.setDefaultButton(cancelButton);

    box.exec();
    if (box.clickedButton() != deleteButton)
        return;

    // If we're deleting the conversation currently shown, clear the chat
    // view first — otherwise ChatWidget is left pointing at a conversation
    // id the store no longer has, and the next render would just find nothing.
    if (conversationId == m_chatWidget->activeConversationId())
        m_chatWidget->setActiveConversation(QString());

    m_store->deleteConversation(conversationId);
}

void MainWindow::refreshSidebar()
{
    m_updatingSidebarProgrammatically = true;

    const QString previouslySelectedId = m_sidebarList->currentItem()
        ? m_sidebarList->currentItem()->data(Qt::UserRole).toString()
        : QString();

    m_sidebarList->clear();
    for (const Conversation &conv : m_store->conversations()) {
        auto *item = new QListWidgetItem;
        item->setData(Qt::UserRole, conv.id);

        auto *rowWidget = new ConversationListItemWidget(conv.id, conv.title);
        connect(rowWidget, &ConversationListItemWidget::deleteRequested,
                this, [this, title = conv.title](const QString &id) {
                    confirmAndDeleteConversation(id, title);
                });

        item->setSizeHint(rowWidget->sizeHint());
        m_sidebarList->addItem(item);
        m_sidebarList->setItemWidget(item, rowWidget);

        if (conv.id == previouslySelectedId)
            m_sidebarList->setCurrentItem(item);
    }

    // Overrides each item's just-set sizeHint width (which reflects its
    // ConversationListItemWidget's *full, un-elided* title) with the list's
    // real viewport width — see updateSidebarItemWidths()'s own comment for
    // why that matters.
    updateSidebarItemWidths();

    m_updatingSidebarProgrammatically = false;
}

void MainWindow::updateSidebarItemWidths()
{
    const int viewportWidth = m_sidebarList->viewport()->width();
    if (viewportWidth <= 0)
        return; // not laid out yet (e.g. called before the window has real geometry) — the next resize will catch it

    for (int i = 0; i < m_sidebarList->count(); ++i) {
        QListWidgetItem *item = m_sidebarList->item(i);
        QSize hint = item->sizeHint();
        if (hint.width() != viewportWidth) {
            hint.setWidth(viewportWidth);
            item->setSizeHint(hint);
        }
    }
}
