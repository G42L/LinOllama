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
                        QWidget *parent)
    : QMainWindow(parent)
    , m_store(store)
    , m_ollamaClient(ollamaClient)
    , m_themeManager(themeManager)
{
    setWindowTitle("Ollama GUI");
    resize(1000, 650);

    updateWindowIconForTheme();
    connect(m_themeManager, &ThemeManager::themeChanged, this, &MainWindow::updateWindowIconForTheme);

    // --- Sidebar --------------------------------------------------------
    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setMinimumWidth(160); // draggable via the splitter below, not fixed
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(8, 8, 8, 8);
    sidebarLayout->setSpacing(6);

    m_newConversationButton = new QPushButton("+ New conversation");
    m_newConversationButton->setObjectName("newConversationButton");
    connect(m_newConversationButton, &QPushButton::clicked,
            this, &MainWindow::onNewConversationClicked);
    sidebarLayout->addWidget(m_newConversationButton);

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
    m_chatWidget = new ChatWidget(ollamaClient, store, themeManager);
    connect(m_chatWidget, &ChatWidget::conversationTitleMayHaveChanged,
            this, &MainWindow::onConversationTitleMayHaveChanged);
    connect(m_chatWidget, &ChatWidget::conversationCreated,
            this, &MainWindow::onChatConversationCreated);

    // --- Stats strip --------------------------------------------------------
    m_statsStrip = new StatsStripWidget(systemMonitor, themeManager);

    // All three panes live in one splitter so every boundary between them
    // is drag-resizable, sidebar included. setChildrenCollapsible(false)
    // stops a drag from accidentally hiding a pane entirely — that read as
    // a bug the first few times, not a feature.
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(sidebar);
    splitter->addWidget(m_chatWidget);
    splitter->addWidget(m_statsStrip);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1); // chat is the only pane that should grab extra space
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({220, 600, 180});
    splitter->setChildrenCollapsible(false);
    setCentralWidget(splitter);

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
    SettingsDialog dialog(m_themeManager, m_ollamaClient, this);
    connect(&dialog, &SettingsDialog::sendButtonStyleChanged,
            m_chatWidget, &ChatWidget::setSendButtonStyle);
    connect(&dialog, &SettingsDialog::statsColorsChanged,
            m_statsStrip, &StatsStripWidget::applyMeterColors);
    dialog.exec();
}

void MainWindow::updateWindowIconForTheme()
{
    setWindowIcon(Theme::loadThemedIconMultiSize(
        ":/icons/ollama.svg", m_themeManager->isDarkActive(), {16, 24, 32, 48, 64, 128}, "text"));
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

    m_updatingSidebarProgrammatically = false;
}
