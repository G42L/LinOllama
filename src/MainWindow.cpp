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
#include <QWebEngineView>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
    resize(1280, 720);

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

    m_importConversationButton = new QToolButton;
    m_importConversationButton->setObjectName("importConversationButton");
    m_importConversationButton->setToolTip("Import conversation…");
    m_importConversationButton->setIconSize(QSize(16, 16));
    m_importConversationButton->setAutoRaise(true);
    m_importConversationButton->setCursor(Qt::PointingHandCursor);
    connect(m_importConversationButton, &QToolButton::clicked,
            this, &MainWindow::onImportConversationClicked);
    topBarLayout->addWidget(m_importConversationButton);

    m_sidebarToggleButton = new QToolButton;
    m_sidebarToggleButton->setObjectName("sidebarToggleButton");
    m_sidebarToggleButton->setIconSize(QSize(16, 16));
    m_sidebarToggleButton->setAutoRaise(true);
    m_sidebarToggleButton->setCursor(Qt::PointingHandCursor);
    connect(m_sidebarToggleButton, &QToolButton::clicked, this, &MainWindow::onSidebarToggleClicked);
    topBarLayout->addWidget(m_sidebarToggleButton);

    topBarLayout->addStretch(1);

    m_ollamaVersionLabel = new QLabel;
    m_ollamaVersionLabel->setObjectName("ollamaVersionLabel");
    m_ollamaVersionLabel->setToolTip("Ollama server version");
    topBarLayout->addWidget(m_ollamaVersionLabel);

    connect(m_ollamaClient, &OllamaClient::serverVersionFetched,
            this, &MainWindow::onServerVersionFetched);
    // reachable() already fires on every periodic refreshStatus() poll
    // (see main.cpp) — piggybacking the version fetch on it means this
    // stays current if the server restarts on a different build, without
    // its own separate timer.
    connect(m_ollamaClient, &OllamaClient::reachable, this, [this](bool isReachable) {
        if (isReachable)
            m_ollamaClient->fetchServerVersion();
        else
            m_ollamaVersionLabel->clear();
    });

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

    // Second half of the QtWebEngine pre-warm started in main.cpp. That one
    // uses a bare QWebEnginePage (no view) to pay for Chromium's subprocess
    // spin-up early; this one specifically covers the *on-screen*
    // QWebEngineView path — GL/surface sharing with this window via
    // Qt::AA_ShareOpenGLContexts — which a headless page alone doesn't
    // exercise, and which was still the likely cause of a first-HTML-render
    // flicker/glitch even after the page-only warmup. A child of this
    // window (not a separate top-level widget, unlike an earlier attempt at
    // this) so it shares this window's own surface instead of needing its
    // own — Wayland doesn't let a client position a *top-level* window at
    // all, which is what made that attempt visibly relocate the whole app;
    // a plain child widget has no such restriction. Runs here, before this
    // window is ever shown (main() calls show() only after this constructor
    // returns), so there's nothing on screen yet for it to visibly disrupt.
    {
        auto *warmupView = new QWebEngineView(this);
        warmupView->setGeometry(0, 0, 1, 1);
        warmupView->hide();
        connect(warmupView, &QWebEngineView::loadFinished, warmupView, [warmupView](bool) {
            warmupView->deleteLater();
        });
        warmupView->setHtml(QStringLiteral("<html></html>"));
    }
}

void MainWindow::discardEmptyActiveConversation()
{
    m_chatWidget->discardConversationIfEmpty(m_chatWidget->activeConversationId());
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

void MainWindow::onImportConversationClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import conversation", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "Conversation files (*.json);;All files (*)");
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Import failed", "Couldn't open \"" + path + "\".");
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    // Both a malformed file and a well-formed-but-wrong-shape one (some
    // unrelated JSON that just happens to parse) are treated the same way
    // here — "messages" is the one field an exported conversation can't
    // possibly be missing, so its absence is what actually catches the
    // latter case, since QJsonObject::value() on a non-object silently
    // returns an empty QJsonValue rather than erroring.
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || !doc.object().contains("messages")) {
        QMessageBox::warning(this, "Import failed",
            "\"" + path + "\" doesn't look like an exported conversation file.");
        return;
    }

    const Conversation imported = Conversation::fromJson(doc.object());
    const QString id = m_store->importConversation(imported);
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
    SettingsDialog dialog(m_themeManager, m_ollamaClient, m_store, m_whisperManager, this);
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

void MainWindow::onServerVersionFetched(const QString &version)
{
    m_ollamaVersionLabel->setText(version.isEmpty() ? QString() : QString("v%1").arg(version));
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
    m_importConversationButton->setIcon(Theme::loadThemedIcon(":/icons/import.svg", dark, 16, "secondaryText"));

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
    // Without this, Qt still paints the popup's native window background as
    // an opaque square behind the QSS-drawn rounded rect (see Theme.cpp's
    // QMenu { border-radius: 8px; }) — a visible color "halo" peeking out
    // past the rounded corners. Translucent background lets the corners
    // outside the rounded rect actually be transparent instead.
    menu->setAttribute(Qt::WA_TranslucentBackground);

    QAction *exportAction = menu->addAction("Export conversation…");
    connect(exportAction, &QAction::triggered, this, [this, conversationId, title]() {
        exportConversation(conversationId, title);
    });

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

void MainWindow::exportConversation(const QString &conversationId, const QString &title)
{
    const Conversation *conv = m_store->find(conversationId);
    if (!conv)
        return;

    // Filesystem-unsafe characters swapped for "_" rather than stripped
    // outright, so e.g. "Q&A: setup?" still reads recognizably as
    // "Q&A_ setup_" instead of losing the separators entirely.
    static const QRegularExpression unsafeChars(R"([/\\:*?"<>|])");
    QString safeName = title.isEmpty() ? "conversation" : title;
    safeName.replace(unsafeChars, "_");

    const QString suggestedPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + "/" + safeName + ".json";
    const QString path = QFileDialog::getSaveFileName(
        this, "Export conversation", suggestedPath, "Conversation files (*.json)");
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, "Export failed", "Couldn't write to \"" + path + "\".");
        return;
    }

    // The exact same shape ConversationStore itself persists internally
    // (see Conversation::toJson()) — no separate export format to maintain,
    // and it round-trips through onImportConversationClicked() perfectly
    // since that's the same schema Conversation::fromJson() already reads.
    const QJsonDocument doc(conv->toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
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
        connect(rowWidget, &ConversationListItemWidget::exportRequested,
                this, [this, title = conv.title](const QString &id) {
                    exportConversation(id, title);
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
