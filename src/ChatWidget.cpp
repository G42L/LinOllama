#include "ChatWidget.h"
#include "ThinkingSectionWidget.h"
#include "ToolCallSectionWidget.h"
#include "BuiltinTools.h"
#include "ThemeManager.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QScrollBar>
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QPointer>
#include <QStyle>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QEvent>
#include <QMenu>
#include <QSettings>
#include <QPlainTextEdit>
#include <QTextCursor>
#include <QDebug>
#include <QMessageBox>

ChatWidget::ChatWidget(OllamaClient *ollamaClient, ConversationStore *store, ThemeManager *themeManager,
                       WhisperManager *whisperManager, QWidget *parent)
    : QWidget(parent)
    , m_ollamaClient(ollamaClient)
    , m_store(store)
    , m_themeManager(themeManager)
    , m_whisperManager(whisperManager)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // --- Message list ------------------------------------------------------
    m_scrollArea = new QScrollArea;
    m_scrollArea->setObjectName("messageScrollArea");
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    // Scrolling itself (wheel, drag, scrollToBottom()) still works exactly
    // as before — this only hides the bar's own track/handle, for a
    // cleaner, more Claude-Desktop-like look than a visible scrollbar.
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_messagesContainer = new QWidget;
    m_messagesContainer->setObjectName("messagesContainer");
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setContentsMargins(20, 16, 20, 16);
    m_messagesLayout->setSpacing(12);
    m_messagesLayout->addStretch(1); // keeps bubbles pinned to the top until there are enough to scroll

    m_scrollArea->setWidget(m_messagesContainer);
    layout->addWidget(m_scrollArea, /*stretch=*/1);

    // --- Empty state: centered intro + the same input bar docked in -------
    // (see dockInputBar()) rather than a plain "select a conversation"
    // label filling the whole pane.
    m_emptyStatePanel = new QWidget;
    m_emptyStatePanel->setObjectName("emptyStatePanel");
    // Watched for QEvent::Resize in eventFilter() so the docked input card's
    // width tracks the panel (80%) instead of a fixed pixel cap.
    m_emptyStatePanel->installEventFilter(this);
    auto *emptyStateLayout = new QVBoxLayout(m_emptyStatePanel);
    emptyStateLayout->setContentsMargins(40, 0, 40, 0);
    emptyStateLayout->addStretch(1);

    auto *emptyStateTitle = new QLabel("Chat with your local models");
    emptyStateTitle->setObjectName("emptyStateTitle");
    emptyStateTitle->setAlignment(Qt::AlignHCenter);
    emptyStateTitle->setWordWrap(true);
    emptyStateLayout->addWidget(emptyStateTitle);

    auto *emptyStateSubtitle = new QLabel("Pick a model and send a message to get started.");
    emptyStateSubtitle->setObjectName("emptyStateSubtitle");
    emptyStateSubtitle->setAlignment(Qt::AlignHCenter);
    emptyStateSubtitle->setWordWrap(true);
    emptyStateLayout->addWidget(emptyStateSubtitle);

    auto *emptyStateInputDock = new QWidget;
    m_emptyStateInputDockLayout = new QHBoxLayout(emptyStateInputDock);
    m_emptyStateInputDockLayout->setContentsMargins(0, 20, 0, 0);
    m_emptyStateInputDockLayout->addStretch(1);
    m_emptyStateInputDockLayout->addStretch(1); // m_inputBar is inserted between these two (index 1) by dockInputBar()
    emptyStateLayout->addWidget(emptyStateInputDock);

    emptyStateLayout->addStretch(2);

    layout->addWidget(m_emptyStatePanel, /*stretch=*/1);

    // --- Context usage strip -------------------------------------------
    m_contextUsageBar = new QWidget;
    m_contextUsageBar->setObjectName("contextUsageBar");
    auto *usageLayout = new QHBoxLayout(m_contextUsageBar);
    usageLayout->setContentsMargins(20, 6, 20, 6);
    usageLayout->setSpacing(10);

    // Jumps straight to any past question instead of scrolling through the
    // whole history — see onJumpToClicked()/m_userMessageMarkers.
    m_jumpToButton = new QToolButton;
    m_jumpToButton->setObjectName("jumpToButton");
    m_jumpToButton->setText("Jump to...");
    m_jumpToButton->setCursor(Qt::PointingHandCursor);
    m_jumpToButton->setAutoRaise(true);
    connect(m_jumpToButton, &QToolButton::clicked, this, &ChatWidget::onJumpToClicked);
    usageLayout->addWidget(m_jumpToButton);

    m_contextUsageProgress = new QProgressBar;
    m_contextUsageProgress->setObjectName("contextUsageProgress");
    m_contextUsageProgress->setRange(0, 100);
    m_contextUsageProgress->setValue(0);
    m_contextUsageProgress->setTextVisible(false);
    m_contextUsageProgress->setFixedHeight(6);
    usageLayout->addWidget(m_contextUsageProgress, /*stretch=*/1);

    m_contextUsageLabel = new QLabel("Context: —");
    m_contextUsageLabel->setObjectName("contextUsageLabel");
    usageLayout->addWidget(m_contextUsageLabel);

    layout->addWidget(m_contextUsageBar);

    // --- Bottom input dock (active conversation) ----------------------------
    // The other endpoint of dockInputBar()'s swap — empty while no
    // conversation is active, since m_inputBar lives in the empty-state
    // panel's dock instead (see setActiveConversation()).
    auto *bottomInputDock = new QWidget;
    m_bottomInputDockLayout = new QVBoxLayout(bottomInputDock);
    m_bottomInputDockLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(bottomInputDock);

    // --- Input bar: one rounded card holding chips + textarea + toolbar ----
    // (Claude-Desktop-style: text on top, attach/model/send in a bottom row
    // inside the same card, rather than everything side-by-side in a strip.)
    m_inputBar = new QWidget;
    m_inputBar->setObjectName("inputBar");
    auto *inputOuterLayout = new QVBoxLayout(m_inputBar);
    inputOuterLayout->setContentsMargins(16, 10, 16, 14);

    m_inputCard = new QFrame;
    m_inputCard->setObjectName("inputCard");
    auto *cardLayout = new QVBoxLayout(m_inputCard);
    cardLayout->setContentsMargins(14, 10, 10, 8);
    cardLayout->setSpacing(4);

    // Queued-but-not-yet-sent file attachments, shown as removable chips
    // above the textarea. Hidden entirely while empty.
    m_attachmentsBar = new QWidget;
    m_attachmentsBar->setObjectName("attachmentsBar");
    m_attachmentsLayout = new QHBoxLayout(m_attachmentsBar);
    m_attachmentsLayout->setContentsMargins(0, 0, 0, 4);
    m_attachmentsLayout->setSpacing(6);
    m_attachmentsLayout->addStretch(1); // chips insert before this, keeping them left-aligned
    m_attachmentsBar->setVisible(false);
    cardLayout->addWidget(m_attachmentsBar);

    m_inputEdit = new ChatInputEdit;
    m_inputEdit->setObjectName("messageInput");
    m_inputEdit->setPlaceholderText("How can I help you today?");
    connect(m_inputEdit, &ChatInputEdit::submitRequested, this, &ChatWidget::onSendClicked);
    // The card (not the textarea) draws the focus ring, since the border
    // now belongs to the outer frame — see eventFilter().
    m_inputEdit->installEventFilter(this);
    cardLayout->addWidget(m_inputEdit);

    auto *toolRow = new QWidget;
    toolRow->setObjectName("inputToolRow");
    auto *toolLayout = new QHBoxLayout(toolRow);
    toolLayout->setContentsMargins(0, 4, 0, 0);
    toolLayout->setSpacing(8);

    m_attachButton = new QToolButton;
    m_attachButton->setObjectName("attachButton");
    m_attachButton->setText("+");
    m_attachButton->setToolTip("Attach files");
    m_attachButton->setCursor(Qt::PointingHandCursor);
    m_attachButton->setAutoRaise(true);
    connect(m_attachButton, &QToolButton::clicked, this, &ChatWidget::onAttachClicked);
    toolLayout->addWidget(m_attachButton);

    // "Tools" dropdown: checkable toggles for each built-in tool-calling
    // tool (see BuiltinTools.h) plus Thinking. A persistent QMenu (built
    // once, not per-click) since checked state has to survive being closed
    // and reopened. Checking one of the tool toggles just makes that tool
    // available to the model (via buildToolDefinitions()) — the model
    // itself decides per-reply whether to actually call it, this doesn't
    // trigger anything eagerly.
    m_toolsButton = new QToolButton;
    m_toolsButton->setObjectName("toolsButton");
    m_toolsButton->setCursor(Qt::PointingHandCursor);
    m_toolsButton->setAutoRaise(true);
    m_toolsButton->setPopupMode(QToolButton::InstantPopup);

    auto *toolsMenu = new QMenu(m_toolsButton);
    // See MainWindow::buildDeleteMenu()'s own comment — avoids a background-
    // color halo showing past the QSS-rounded corners (Theme.cpp's
    // QMenu { border-radius: 8px; }).
    toolsMenu->setAttribute(Qt::WA_TranslucentBackground);
    m_webSearchAction = toolsMenu->addAction("Search Wikipedia");
    m_webSearchAction->setCheckable(true);
    m_webSearchAction->setChecked(m_webSearchEnabled);
    connect(m_webSearchAction, &QAction::toggled, this, &ChatWidget::onWebSearchToggled);

    m_stackOverflowAction = toolsMenu->addAction("Search Stack Overflow");
    m_stackOverflowAction->setCheckable(true);
    m_stackOverflowAction->setChecked(m_stackOverflowEnabled);
    connect(m_stackOverflowAction, &QAction::toggled, this, &ChatWidget::onStackOverflowToggled);

    m_calculatorAction = toolsMenu->addAction("Calculator");
    m_calculatorAction->setCheckable(true);
    m_calculatorAction->setChecked(m_calculatorEnabled);
    connect(m_calculatorAction, &QAction::toggled, this, &ChatWidget::onCalculatorToggled);

    m_dateTimeAction = toolsMenu->addAction("Current date && time");
    m_dateTimeAction->setCheckable(true);
    m_dateTimeAction->setChecked(m_dateTimeEnabled);
    connect(m_dateTimeAction, &QAction::toggled, this, &ChatWidget::onDateTimeToolToggled);

    m_thinkingAction = toolsMenu->addAction("Thinking");
    m_thinkingAction->setCheckable(true);
    m_thinkingAction->setChecked(m_thinkingEnabled);
    connect(m_thinkingAction, &QAction::toggled, this, &ChatWidget::onThinkingToggled);

    m_toolsButton->setMenu(toolsMenu);
    toolLayout->addWidget(m_toolsButton);
    updateToolsButtonAppearance(); // sets the initial "Tools" label

    m_toolExecutor = new ToolExecutor(this);
    connect(m_toolExecutor, &ToolExecutor::toolCallCompleted, this, &ChatWidget::onToolCallCompleted);
    connect(m_toolExecutor, &ToolExecutor::allToolCallsCompleted, this, &ChatWidget::onAllToolCallsCompleted);

    toolLayout->addStretch(1);

    // No "Model:" label — the dropdown itself is self-explanatory once
    // you've used it once. Styled flat/borderless (see Theme.cpp) to read
    // as a toolbar item rather than a form control.
    m_modelCombo = new QComboBox;
    m_modelCombo->setObjectName("modelCombo");
    m_modelCombo->setMinimumWidth(110);
    // AdjustToContents makes the box's own width track whatever model name
    // is actually selected (long ones like "qwen2.5-coder:32b-instruct-
    // q4_K_M" no longer get cropped/elided) instead of a fixed box a long
    // name would overflow — the 320px ceiling just keeps a truly extreme
    // name from squeezing the keep-alive combo/voice/send buttons off the
    // toolbar, which the addStretch() to its left otherwise has room to
    // absorb for anything shorter.
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_modelCombo->setMaximumWidth(320);
    connect(m_modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChatWidget::onModelComboChanged);
    toolLayout->addWidget(m_modelCombo);

    // Per-conversation keep_alive — the lighter-weight sibling of the tray/
    // Settings "offload model" button (see Conversation::keepAliveSeconds).
    // Same flat/borderless toolbar styling as m_modelCombo, no separate
    // label for the same reason that one doesn't have one.
    m_keepAliveCombo = new QComboBox;
    m_keepAliveCombo->setObjectName("keepAliveCombo");
    m_keepAliveCombo->setToolTip("How long to keep this conversation's model loaded after replying");
    m_keepAliveCombo->addItem("Keep alive: Default", kKeepAliveUseServerDefault);
    m_keepAliveCombo->addItem("5 minutes", 300);
    m_keepAliveCombo->addItem("30 minutes", 1800);
    m_keepAliveCombo->addItem("1 hour", 3600);
    m_keepAliveCombo->addItem("Until manually unloaded", -1);
    m_keepAliveCombo->addItem("Unload right after reply", 0);
    connect(m_keepAliveCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChatWidget::onKeepAliveComboChanged);
    toolLayout->addWidget(m_keepAliveCombo);

    // Push-to-talk: pressed()/released() (not clicked()) so holding the
    // button down brackets the recording, matching "hold press to record."
    m_voiceButton = new QToolButton;
    m_voiceButton->setObjectName("voiceButton");
    m_voiceButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_voiceButton->setIconSize(QSize(16, 16));
    m_voiceButton->setToolTip("Hold to record a voice message");
    m_voiceButton->setCursor(Qt::PointingHandCursor);
    m_voiceButton->setAutoRaise(true);
    connect(m_voiceButton, &QToolButton::pressed, this, &ChatWidget::onVoicePressed);
    connect(m_voiceButton, &QToolButton::released, this, &ChatWidget::onVoiceReleased);
    toolLayout->addWidget(m_voiceButton);
    connect(&m_voiceRecorder, &VoiceRecorder::recordingFinished, this, &ChatWidget::onVoiceRecordingFinished);
    connect(&m_voiceRecorder, &VoiceRecorder::recordingFailed, this, &ChatWidget::onVoiceRecordingFailed);
    connect(&m_voiceRecorder, &VoiceRecorder::audioLevelChanged, this, &ChatWidget::audioLevelChanged);
    connect(&m_voiceRecorder, &VoiceRecorder::liveChunkReady, this, &ChatWidget::onVoiceLiveChunkReady);
    if (m_whisperManager) {
        connect(m_whisperManager, &WhisperManager::transcriptionFinished,
                this, &ChatWidget::onWhisperTranscriptionFinished);
        connect(m_whisperManager, &WhisperManager::transcriptionProgress,
                this, &ChatWidget::onWhisperTranscriptionProgress);
        connect(m_whisperManager, &WhisperManager::liveChunkTranscribed,
                this, &ChatWidget::onWhisperLiveChunkTranscribed);
        connect(m_whisperManager, &WhisperManager::liveServerStateChanged,
                this, &ChatWidget::onWhisperLiveServerStateChanged);
    }

    m_sendButton = new QPushButton;
    m_sendButton->setObjectName("sendButton");
    m_sendButton->setIconSize(QSize(16, 16));
    // Without this, Fusion still paints QPushButton's own raised/bevel
    // chrome underneath whatever QSS says — background-color/border rules
    // alone don't fully suppress it the way they do for a QToolButton with
    // autoRaise(true). Flat hands painting over to QSS completely, which is
    // what actually lets #sendButton's flat (default) rule read as truly
    // flat instead of just a differently-colored version of the native frame.
    m_sendButton->setFlat(true);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatWidget::onSendButtonClicked);
    toolLayout->addWidget(m_sendButton);
    // Restores whatever was last saved in Settings (see SettingsDialog's
    // "Send button" combo) — defaults to the paper-plane icon.
    setSendButtonStyle(QSettings().value("chat/sendButtonStyle", "plane").toString());
    // Restores the background style (see SettingsDialog's "Filled send
    // button" checkbox) — defaults to flat/off, matching the new look.
    setSendButtonFilled(QSettings().value("chat/sendButtonFilled", false).toBool());
    // Restores "Send automatically after transcription" (see SettingsDialog) —
    // defaults to off, so a transcription lands in the box for review rather
    // than going straight to Ollama.
    m_voiceAutoSend = QSettings().value("chat/voiceAutoSend", false).toBool();

    cardLayout->addWidget(toolRow);

    inputOuterLayout->addWidget(m_inputCard);

    // m_inputBar isn't placed here — setActiveConversation() (called at the
    // end of this constructor, starting in the empty state) docks it via
    // dockInputBar() instead, so it lands in whichever slot matches the
    // initial state.

    connect(m_ollamaClient, &OllamaClient::chatDelta, this, &ChatWidget::onChatDelta);
    connect(m_ollamaClient, &OllamaClient::chatThinkingDelta, this, &ChatWidget::onChatThinkingDelta);
    connect(m_ollamaClient, &OllamaClient::chatToolCalls, this, &ChatWidget::onChatToolCalls);
    connect(m_ollamaClient, &OllamaClient::chatDone, this, &ChatWidget::onChatDone);
    connect(m_ollamaClient, &OllamaClient::chatError, this, &ChatWidget::onChatError);
    connect(m_ollamaClient, &OllamaClient::chatUsage, this, &ChatWidget::onChatUsage);
    connect(m_ollamaClient, &OllamaClient::modelContextLengthFetched,
            this, &ChatWidget::onModelContextLengthFetched);
    connect(m_ollamaClient, &OllamaClient::modelMetadataFetched,
            this, &ChatWidget::onModelMetadataFetched);

    m_streamRenderTimer = new QTimer(this);
    m_streamRenderTimer->setInterval(50); // ~20 Hz — see the member's own comment
    m_streamRenderTimer->setSingleShot(true);
    connect(m_streamRenderTimer, &QTimer::timeout, this, &ChatWidget::flushStreamRender);

    m_chatQueue = new ChatQueue(m_ollamaClient, this);
    m_chatQueue->setOptimizeModelSwaps(QSettings().value("chat/modelOptimization", false).toBool());
    connect(m_chatQueue, &ChatQueue::turnStarted, this, &ChatWidget::onQueueTurnStarted);
    connect(m_chatQueue, &ChatQueue::queuePositionChanged, this, &ChatWidget::onQueuePositionChanged);

    if (m_themeManager)
        connect(m_themeManager, &ThemeManager::themeChanged, this, &ChatWidget::reloadThemedIcons);
    reloadThemedIcons();

    setActiveConversation(QString()); // start in the empty state
}

void ChatWidget::setAvailableModels(const QStringList &modelNames)
{
    const QString current = m_modelCombo->currentText();
    m_modelCombo->blockSignals(true);
    m_modelCombo->clear();
    m_modelCombo->addItems(modelNames);
    if (!current.isEmpty()) {
        const int idx = m_modelCombo->findText(current);
        if (idx >= 0)
            m_modelCombo->setCurrentIndex(idx);
    }
    m_modelCombo->blockSignals(false);
}

void ChatWidget::setSendButtonStyle(const QString &style)
{
    m_sendButtonStyle = style;
    const bool isArrow = (style == "arrow");
    const bool isPlane = (style == "plane");

    if (isPlane) {
        m_sendButton->setText(QString());
    } else if (isArrow) {
        // U+2191 UPWARDS ARROW — a plain glyph, not an emoji, so it renders
        // reliably everywhere (see ThinkingSectionWidget's spinner for why
        // emoji specifically were dropped from this app after one didn't
        // render on a real machine).
        m_sendButton->setIcon(QIcon());
        m_sendButton->setText(QString::fromUtf8("\xE2\x86\x91"));
    } else { // "text"
        m_sendButton->setIcon(QIcon());
        m_sendButton->setText("Send");
    }

    // Drives Theme.cpp's QPushButton#sendButton[arrowStyle="true"]/
    // [planeStyle="true"] rules — the pill shape and accent color still
    // come from the base #sendButton rule either way.
    m_sendButton->setProperty("arrowStyle", isArrow);
    m_sendButton->setProperty("planeStyle", isPlane);
    m_sendButton->style()->unpolish(m_sendButton);
    m_sendButton->style()->polish(m_sendButton);

    reloadSendButtonIcon();
}

void ChatWidget::setSendButtonFilled(bool filled)
{
    m_sendButtonFilled = filled;

    // Drives Theme.cpp's QPushButton#sendButton[filled="true"] rules — the
    // base (unfilled) rule already covers the flat/default look.
    m_sendButton->setProperty("filled", filled);
    m_sendButton->style()->unpolish(m_sendButton);
    m_sendButton->style()->polish(m_sendButton);

    reloadSendButtonIcon();
    if (m_isGenerating)
        setSendButtonBusy(true); // re-picks the stop icon's color for the new background
}

void ChatWidget::setVoiceAutoSend(bool enabled)
{
    m_voiceAutoSend = enabled;
}

void ChatWidget::refreshContextLengthSetting()
{
    updateContextUsageDisplay();
}

void ChatWidget::setModelOptimizationEnabled(bool enabled)
{
    m_chatQueue->setOptimizeModelSwaps(enabled);
}

void ChatWidget::refreshAudioInputDevice()
{
    m_voiceRecorder.refreshAudioInputDevice();
}

void ChatWidget::refreshMeterSmoothing()
{
    m_voiceRecorder.refreshMeterSmoothing();
}

void ChatWidget::refreshFormattingSettings()
{
    // renderConversation() already no-ops cleanly when there's no active
    // conversation (a fresh empty chat, or the very first launch).
    renderConversation();
}

void ChatWidget::setActiveConversation(const QString &conversationId)
{
    // Switching conversations no longer touches any in-flight stream — a
    // reply keeps generating in the background (still landing in
    // ConversationStore via onChatDelta()) even while a different
    // conversation is shown. Only the *live widget pointers* are dropped
    // here, since renderConversation() (below) is about to delete whatever
    // bubbles currently exist in the shared message list anyway; the
    // streaming data itself lives in m_streams, keyed by conversation, and
    // survives the switch untouched.
    m_streamingBrowser = nullptr;
    m_streamingBubbleLayout = nullptr;
    m_streamingThinkingWidget = nullptr;

    // Files queued for the next message don't carry over to a different
    // conversation you switch into.
    m_pendingAttachments.clear();
    rebuildAttachmentsBar();

    const QString previousConversationId = m_activeConversationId;
    m_activeConversationId = conversationId;

    // Leaving an empty "New conversation" behind (e.g. clicking + then
    // navigating away without typing anything) shouldn't leave it cluttering
    // the sidebar forever.
    if (previousConversationId != conversationId)
        discardConversationIfEmpty(previousConversationId);

    const bool hasConversation = !conversationId.isEmpty();
    m_scrollArea->setVisible(hasConversation);
    m_contextUsageBar->setVisible(hasConversation);
    m_emptyStatePanel->setVisible(!hasConversation);
    dockInputBar(!hasConversation);
    // The input stays usable even with nothing selected — sending from the
    // empty state auto-creates a conversation (see onSendClicked).
    setInputEnabled(true);

    if (!hasConversation) {
        setSendButtonBusy(false);
        return;
    }

    const Conversation *conv = m_store->find(conversationId);
    if (!conv)
        return;

    if (!conv->model.isEmpty()) {
        m_modelCombo->blockSignals(true);
        const int idx = m_modelCombo->findText(conv->model);
        if (idx >= 0)
            m_modelCombo->setCurrentIndex(idx);
        m_modelCombo->blockSignals(false);
    }
    // Once a conversation has messages, its model is locked in — Ollama
    // has no notion of "switch models mid-chat" and doing so silently would
    // just confuse whatever context the new model doesn't share.
    m_modelCombo->setEnabled(conv->messages.isEmpty());

    {
        m_keepAliveCombo->blockSignals(true);
        const int idx = m_keepAliveCombo->findData(conv->keepAliveSeconds);
        // findData() misses if an old/foreign value snuck in (e.g. hand-
        // edited JSON) — falls back to "Default" rather than leaving
        // whatever the previous conversation's selection happened to be.
        m_keepAliveCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        m_keepAliveCombo->blockSignals(false);
    }

    // Reconnects a live bubble automatically if this conversation still has
    // a stream running in the background (see reconnectStreamingBubble()).
    renderConversation();

    // Reflects *this* conversation's own generation state — not whatever
    // the previously-shown one left the send button/input showing.
    const bool streaming = m_streams.contains(conversationId);
    setSendButtonBusy(streaming);
    setInputEnabled(!streaming);

    if (!conv->model.isEmpty())
        ensureContextLengthKnown(conv->model);
    updateContextUsageDisplay();
}

void ChatWidget::discardConversationIfEmpty(const QString &conversationId)
{
    // Never a conversation still generating a reply in the background —
    // that would silently kill a turn the user is still waiting on (also
    // covers the case where conversationId is still the active one, e.g.
    // called from MainWindow on app quit: a streaming reply keeps a
    // conversation from being "empty" in spirit even before its first
    // message is persisted).
    if (conversationId.isEmpty() || m_streams.contains(conversationId))
        return;

    const Conversation *conv = m_store->find(conversationId);
    if (!conv || !conv->messages.isEmpty() || conv->title != "New conversation")
        return;

    m_store->deleteConversation(conversationId);
}

void ChatWidget::dockInputBar(bool inEmptyState)
{
    // Reparenting a widget by adding it to a different layout doesn't
    // automatically drop it from whichever layout held it before — that has
    // to be done explicitly, or the old layout is left with a stale item.
    // Both calls are safe no-ops if m_inputBar isn't currently in that layout.
    m_bottomInputDockLayout->removeWidget(m_inputBar);
    m_emptyStateInputDockLayout->removeWidget(m_inputBar);

    if (inEmptyState) {
        m_emptyStateInputDockLayout->insertWidget(1, m_inputBar); // between the dock's two stretches
        // Sized immediately against the panel's current width, not just on
        // the next resize — see updateEmptyStateInputWidth().
        updateEmptyStateInputWidth();
    } else {
        m_inputBar->setMinimumWidth(0);
        m_inputBar->setMaximumWidth(QWIDGETSIZE_MAX);
        m_bottomInputDockLayout->addWidget(m_inputBar);
    }
}

void ChatWidget::updateEmptyStateInputWidth()
{
    if (!m_activeConversationId.isEmpty())
        return; // only the homepage composer is width-capped; the active-chat bar stays full width

    // setFixedWidth, not setMaximumWidth: sitting between two stretch items
    // in the dock's QHBoxLayout, m_inputBar's default (Preferred) size
    // policy means it only ever claims its own sizeHint's worth of space —
    // raising the *maximum* doesn't make a Preferred-policy widget actually
    // grow to fill the extra room, since the stretches greedily absorb it
    // first. A fixed width forces the exact target instead. Recomputed on
    // every resize (see eventFilter()'s QEvent::Resize branch) so the card
    // scales with the window rather than sticking to one size.
    const int target = qMax(320, qRound(m_emptyStatePanel->width() * 0.8));
    m_inputBar->setFixedWidth(target);
}

void ChatWidget::clearMessages()
{
    // Index 0 is always the earliest remaining message row while the
    // trailing stretch (added once, in the constructor) sits last — so
    // repeatedly popping the front leaves just the stretch behind.
    while (m_messagesLayout->count() > 1) {
        QLayoutItem *item = m_messagesLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    // The rows just queued for deletion are what m_userMessageMarkers points
    // at — clear it now (synchronously, before any stale pointer could be
    // used) rather than waiting for the deferred deleteLater() to run.
    m_userMessageMarkers.clear();

    // renderConversation() rebuilds the whole message list immediately
    // after calling this — deleteLater() above only *schedules* destruction,
    // so without this, the just-removed rows (still alive, still at their
    // old size) would keep occupying space alongside the freshly-built ones
    // until the event loop eventually got around to the deferred deletion.
    // Most visible with a font-scale change: shrinking back down left the
    // previous, taller bubbles lingering underneath, showing as empty space
    // where they used to be. Flushing immediately closes that gap.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void ChatWidget::renderConversation()
{
    clearMessages();
    const Conversation *conv = m_store->find(m_activeConversationId);
    if (!conv)
        return;

    // If a background stream is still writing into this conversation, its
    // reply renders as a *live* bubble instead of a static one — see
    // reconnectStreamingBubble(). ConversationStore doesn't gain an
    // assistant ChatMessage for it until the first *content* token arrives
    // (see updateStreamingAssistantMessage(), only called from onChatDelta,
    // never from onChatThinkingDelta) — so during the thinking-only phase,
    // conv->messages still ends on the user's own message, with no
    // assistant entry yet to anchor the live bubble to. That's handled
    // after the loop below instead of inside it.
    const bool hasLiveStream = m_streams.contains(m_activeConversationId);
    // A pure tool-call request message (empty content, non-empty toolCalls)
    // is never the live placeholder — that's always the plain assistant
    // message updateStreamingAssistantMessage() lazily creates on the first
    // real *content* token of whichever round ends up answering. See the
    // tool-call folding logic in the loop below for how such a message (and
    // any "tool" role results after it) gets displayed instead.
    const bool lastMessageIsLiveAssistant = hasLiveStream && !conv->messages.isEmpty()
        && conv->messages.last().role == "assistant" && conv->messages.last().toolCalls.isEmpty();
    const int liveMessageIndex = lastMessageIsLiveAssistant ? conv->messages.size() - 1 : -1;

    // Accumulates a run of [assistant tool_calls, tool, tool, ...] messages
    // (possibly several rounds' worth) as they're encountered, so they can
    // be attached as ToolCallSectionWidgets above the *next* real assistant
    // bubble — the same "collapsible trace above the answer" shape the live
    // round-trip builds in handleToolCalls(), just reconstructed from
    // persisted history instead of built as it happens. Neither message
    // type gets a bubble of its own.
    struct PendingToolCall
    {
        QString name;
        QJsonObject arguments;
        QString resultText;
        bool resolved = false;
    };
    QVector<PendingToolCall> pendingToolCalls;

    // Note: past "thinking" traces aren't persisted to disk (see
    // Conversation.h / ChatMessage — there's no field for it), so reloading
    // an old conversation shows only the final answers, with no thinking
    // section above them. That's a deliberate scope call, not a bug: the
    // trace is a live reasoning scratchpad, not part of the saved transcript.
    // The in-progress stream's own thinking trace (if any) is still shown
    // live via reconnectStreamingBubble(), which reads it from m_streams
    // rather than from disk.
    for (int i = 0; i < conv->messages.size(); ++i) {
        if (i == liveMessageIndex) {
            reconnectStreamingBubble();
            continue;
        }
        const ChatMessage &msg = conv->messages[i];

        if (msg.role == "assistant" && !msg.toolCalls.isEmpty() && msg.content.isEmpty()) {
            for (const QJsonValue &callValue : msg.toolCalls) {
                const BuiltinTools::ParsedToolCall parsed = BuiltinTools::parseToolCall(callValue.toObject());
                pendingToolCalls.append(PendingToolCall{parsed.name, parsed.arguments, QString(), false});
            }
            continue;
        }

        if (msg.role == "tool") {
            for (PendingToolCall &call : pendingToolCalls) {
                if (!call.resolved) {
                    call.resultText = msg.content;
                    call.resolved = true;
                    break;
                }
            }
            continue;
        }

        QVBoxLayout *bubbleLayout = nullptr;
        appendMessageBubble(msg.role, msg.content, &bubbleLayout, msg.attachmentNames, i, msg.timestamp);

        // Only an assistant bubble can sensibly host a "here's what I
        // called before answering" trace — a user message interrupting a
        // pending tool-call run (e.g. the previous turn was stopped mid
        // tool execution) just silently drops it rather than attaching it
        // somewhere that wouldn't make sense.
        if (msg.role == "assistant" && !pendingToolCalls.isEmpty() && bubbleLayout) {
            int insertPos = 0;
            for (const PendingToolCall &call : pendingToolCalls) {
                auto *widget = new ToolCallSectionWidget(call.name, call.arguments);
                if (call.resolved)
                    widget->setResult(call.resultText);
                bubbleLayout->insertWidget(insertPos++, widget);
            }
        }
        pendingToolCalls.clear();
    }

    // Thinking-only phase: there's a live stream but no assistant message in
    // the store yet to have matched inside the loop above, so the live
    // bubble (empty answer text, possibly a thinking section) is appended
    // here instead, as a new trailing row.
    if (hasLiveStream && !lastMessageIsLiveAssistant)
        reconnectStreamingBubble();
}

QString ChatWidget::livePreviewText(const QString &buffer)
{
    // Complete ```html ... ``` blocks — collapsed to a placeholder wherever
    // they appear (a reply can have text before/after one).
    static const QRegularExpression completeHtmlFence(
        QStringLiteral("```html\\s*\\n.*?```"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QString placeholder = QStringLiteral("\n\n*(rendering HTML once the reply finishes…)*\n\n");

    QString preview = buffer;
    preview.replace(completeHtmlFence, placeholder);

    // Whatever's left of a still-open ```html fence (the model hasn't typed
    // its closing ``` yet) — since every *complete* one was already replaced
    // above, any "```html" still present has to be one of these. Everything
    // from that point on is inside it, so it's dropped entirely rather than
    // streamed token-by-token as raw HTML/CSS/JS source text.
    const int openIdx = preview.indexOf(QStringLiteral("```html"));
    if (openIdx >= 0) {
        preview.truncate(openIdx);
        preview += placeholder;
    }

    return preview;
}

void ChatWidget::reconnectStreamingBubble()
{
    StreamState &st = m_streams[m_activeConversationId];

    m_streamingBrowser = appendMessageBubble("assistant", QString(), &m_streamingBubbleLayout);
    m_streamingThinkingWidget = nullptr;
    if (!st.thinkingBuffer.isEmpty()) {
        m_streamingThinkingWidget = new ThinkingSectionWidget;
        m_streamingThinkingWidget->setThinking(st.isThinkingActive);
        m_streamingThinkingWidget->appendThinkingText(st.thinkingBuffer);
        m_streamingBubbleLayout->insertWidget(0, m_streamingThinkingWidget); // above the answer text
    }
    if (!st.buffer.isEmpty())
        m_streamingBrowser->setMarkdownWithHtmlBlocks(livePreviewText(st.buffer));
    else if (m_chatQueue->isQueued(m_activeConversationId))
        onQueuePositionChanged(m_activeConversationId, m_chatQueue->aheadCount(m_activeConversationId));
}

AutoHeightTextBrowser *ChatWidget::appendMessageBubble(const QString &role, const QString &content,
                                                        QVBoxLayout **bubbleLayoutOut,
                                                        const QStringList &attachmentNames,
                                                        int messageIndex, const QDateTime &timestamp)
{
    const bool isUser = (role == "user");
    const bool isError = (role == "error");

    auto *row = new QWidget;
    row->setObjectName("messageRow");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);

    // No maximum width and no stretch on either side — bubbles fill the full
    // width of the chat pane instead of being capped/aligned by role.
    // Role is conveyed by background alone (see Theme.cpp): the user bubble
    // keeps its fill color, the assistant bubble has none.
    auto *bubble = new QFrame;
    bubble->setObjectName(isError ? "errorBubble" : (isUser ? "userBubble" : "assistantBubble"));

    auto *bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(14, 10, 14, 10);
    bubbleLayout->setSpacing(4);
    if (bubbleLayoutOut)
        *bubbleLayoutOut = bubbleLayout;

    if (!attachmentNames.isEmpty()) {
        auto *attachmentsLabel = new QLabel("Attached: " + attachmentNames.join(", "));
        attachmentsLabel->setObjectName("attachmentsSummaryLabel");
        attachmentsLabel->setWordWrap(true);
        bubbleLayout->addWidget(attachmentsLabel);
    }

    auto *browser = new AutoHeightTextBrowser;
    browser->setObjectName("bubbleText");
    if (isUser || isError) {
        // Show the person's own text (and error messages) literally — no
        // markdown interpretation of something they typed or a raw error
        // string — but still swap recognized emoji for the bundled images.
        browser->setPlainTextWithEmoji(content);
        bubbleLayout->addWidget(browser);
    } else {
        bubbleLayout->addWidget(browser); // added before rendering so map widgets (see below) land after it
        renderAssistantContent(browser, bubbleLayout, content);
    }

    // Inline editor — lives inside the bubble (replaces the text in place),
    // hidden until "Edit" is clicked. Built here since it has to sit inside
    // bubbleLayout, in the text's own spot; the button that reveals it
    // lives in the separate actionsRow below (built next), so all the
    // Edit/Cancel/Save wiring happens together once both exist.
    QWidget *editContainer = nullptr;
    QPlainTextEdit *editBox = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *saveButton = nullptr;
    if (isUser && messageIndex >= 0) {
        editContainer = new QWidget;
        editContainer->setVisible(false);
        auto *editContainerLayout = new QVBoxLayout(editContainer);
        editContainerLayout->setContentsMargins(0, 6, 0, 0);
        editContainerLayout->setSpacing(6);

        editBox = new QPlainTextEdit(content);
        editBox->setObjectName("messageEditBox");
        editContainerLayout->addWidget(editBox);

        auto *editButtonsRow = new QWidget;
        auto *editButtonsLayout = new QHBoxLayout(editButtonsRow);
        editButtonsLayout->setContentsMargins(0, 0, 0, 0);
        editButtonsLayout->addStretch(1);
        cancelButton = new QPushButton("Cancel");
        saveButton = new QPushButton("Resend");
        saveButton->setObjectName("editSaveButton");
        editButtonsLayout->addWidget(cancelButton);
        editButtonsLayout->addWidget(saveButton);
        editContainerLayout->addWidget(editButtonsRow);

        bubbleLayout->addWidget(editContainer);
    }

    rowLayout->addWidget(bubble, /*stretch=*/1);

    // Timestamp + Edit/Retry: a separate row below the bubble (plain
    // background, not the bubble's fill color) rather than inside it, so it
    // sits between this prompt and whatever comes next — inserted into
    // m_messagesLayout right after `row` itself, below. messageIndex < 0
    // for the empty streaming placeholder, which has no message yet to
    // point at.
    QWidget *actionsRow = nullptr;
    if (isUser && messageIndex >= 0) {
        actionsRow = new QWidget;
        actionsRow->setObjectName("messageActionsRow");
        auto *actionsLayout = new QHBoxLayout(actionsRow);
        actionsLayout->setContentsMargins(14, 2, 14, 0);
        actionsLayout->setSpacing(6);

        auto *timestampLabel = new QLabel(
            timestamp.isValid() ? timestamp.toString("MMM d, yyyy 'at' h:mm AP") : QString());
        timestampLabel->setObjectName("messageTimestampLabel");
        actionsLayout->addWidget(timestampLabel, /*stretch=*/1);

        const bool dark = m_themeManager && m_themeManager->isDarkActive();

        auto *editButton = new QToolButton;
        editButton->setObjectName("messageActionButton");
        editButton->setIcon(Theme::loadThemedIcon(":/icons/edit.svg", dark, 13, "secondaryText"));
        editButton->setToolTip("Edit");
        editButton->setCursor(Qt::PointingHandCursor);
        editButton->setAutoRaise(true);
        actionsLayout->addWidget(editButton);

        auto *retryButton = new QToolButton;
        retryButton->setObjectName("messageActionButton");
        retryButton->setIcon(Theme::loadThemedIcon(":/icons/retry.svg", dark, 13, "secondaryText"));
        retryButton->setToolTip("Retry");
        retryButton->setCursor(Qt::PointingHandCursor);
        retryButton->setAutoRaise(true);
        connect(retryButton, &QToolButton::clicked, this, [this, messageIndex]() {
            retryMessage(messageIndex);
        });
        actionsLayout->addWidget(retryButton);

        connect(editButton, &QToolButton::clicked, this, [browser, actionsRow, editContainer, editBox]() {
            browser->setVisible(false);
            actionsRow->setVisible(false);
            editContainer->setVisible(true);
            editBox->setFocus();
            QTextCursor cursor = editBox->textCursor();
            cursor.movePosition(QTextCursor::End);
            editBox->setTextCursor(cursor);
        });
        connect(cancelButton, &QPushButton::clicked, this, [browser, actionsRow, editContainer]() {
            editContainer->setVisible(false);
            browser->setVisible(true);
            actionsRow->setVisible(true);
        });
        connect(saveButton, &QPushButton::clicked, this, [this, messageIndex, editBox]() {
            const QString newText = editBox->toPlainText().trimmed();
            if (!newText.isEmpty())
                editMessage(messageIndex, newText);
        });
    }

    if (isUser) {
        UserMessageMarker marker;
        marker.preview = content.left(60).simplified();
        if (content.length() > 60)
            marker.preview += "...";
        if (marker.preview.isEmpty())
            marker.preview = attachmentNames.isEmpty()
                ? QString("Message %1").arg(m_userMessageMarkers.size() + 1)
                : attachmentNames.join(", ");
        marker.row = row;
        m_userMessageMarkers.append(marker);
    }

    // Insert just before the trailing stretch, which must always stay last —
    // actionsRow (if any) goes directly after row, so it sits between this
    // prompt and whatever's appended next.
    const int insertIndex = m_messagesLayout->count() - 1;
    m_messagesLayout->insertWidget(insertIndex, row);
    if (actionsRow)
        m_messagesLayout->insertWidget(insertIndex + 1, actionsRow);
    scrollToBottom();
    return browser;
}

void ChatWidget::renderAssistantContent(AutoHeightTextBrowser *browser, QVBoxLayout *bubbleLayout, const QString &content)
{
    // ```html blocks are pulled out and rendered via a real Chromium view
    // (HtmlEmbedWidget) rather than left for AutoHeightTextBrowser's own
    // static-subset ```html handling — that has no JavaScript engine and no
    // <canvas>, so anything script-driven (Chart.js, D3, etc.) rendered as
    // inert markup there. This is done first, ahead of the ```map
    // extraction below, so both fence types can appear in the same reply.
    static const QRegularExpression htmlFence(
        QStringLiteral("```html\\s*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);

    QVector<QString> htmlBlocks;
    QString withoutHtml;
    withoutHtml.reserve(content.size());
    {
        int lastEnd = 0;
        QRegularExpressionMatchIterator it = htmlFence.globalMatch(content);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            withoutHtml += content.mid(lastEnd, match.capturedStart() - lastEnd);
            htmlBlocks.append(match.captured(1));
            lastEnd = match.capturedEnd();
        }
        withoutHtml += content.mid(lastEnd);
    }

    static const QRegularExpression mapFence(
        QStringLiteral("```map\\s*\\n(.*?)```"),
        QRegularExpression::DotMatchesEverythingOption);

    struct MapBlockSpec
    {
        QString query;
        int zoom = 12;
    };
    QVector<MapBlockSpec> mapBlocks;

    QString textOnly;
    textOnly.reserve(withoutHtml.size());
    int lastEnd = 0;
    QRegularExpressionMatchIterator it = mapFence.globalMatch(withoutHtml);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        textOnly += withoutHtml.mid(lastEnd, match.capturedStart() - lastEnd);
        lastEnd = match.capturedEnd();

        const QJsonObject obj = QJsonDocument::fromJson(match.captured(1).trimmed().toUtf8()).object();
        const QString query = obj.value("query").toString();
        if (!query.isEmpty())
            mapBlocks.append({query, obj.value("zoom").toInt(12)});
    }
    textOnly += withoutHtml.mid(lastEnd);

    // No ```html fence survives into textOnly (already extracted above), so
    // this is effectively plain Markdown rendering now — still routed
    // through setMarkdownWithHtmlBlocks() rather than setMarkdown() in case
    // a model ever nests one inside e.g. a code fence in a way the regex
    // above doesn't catch; harmless either way since it's a no-op when
    // there's nothing left for it to find.
    browser->setMarkdownWithHtmlBlocks(textOnly);

    QVector<QWidget *> htmlWidgets;
    htmlWidgets.reserve(htmlBlocks.size());
    for (const QString &html : htmlBlocks) {
        // A thin, empty placeholder goes in immediately so bubbleLayout's
        // ordering and htmlWidgets (used by the raw/rendered toggle below)
        // are correct right away; the real HtmlEmbedWidget is only actually
        // constructed a moment later, off this call stack entirely — see
        // the comment on the singleShot below for why.
        auto *placeholder = new QWidget;
        auto *placeholderLayout = new QVBoxLayout(placeholder);
        placeholderLayout->setContentsMargins(0, 0, 0, 0);
        bubbleLayout->addWidget(placeholder);
        htmlWidgets.append(placeholder);

        // The very first QWebEngineView constructed in this process spins
        // up Chromium's own subprocess synchronously, which is heavy enough
        // that doing it inside the same call stack as the click that
        // selected this conversation (renderConversation() -> ... ->
        // here) was observed to desync Qt's own mouse press/release
        // pairing on the sidebar — its list view was left thinking a
        // press-drag was still in progress, so the next mouse *move* (no
        // button even held) scrolled it, until a second click somewhere
        // reset that state. Deferring construction to the next event-loop
        // turn keeps the (one-time, first-ever) Chromium startup cost
        // outside the mouse event's own call stack entirely.
        QPointer<QWidget> placeholderGuard(placeholder);
        QTimer::singleShot(0, this, [placeholderGuard, html]() {
            if (!placeholderGuard)
                return; // the bubble (e.g. its whole conversation) was torn down before this ran
            auto *embed = new HtmlEmbedWidget(html);
            placeholderGuard->layout()->addWidget(embed);
        });
    }

    for (const MapBlockSpec &spec : mapBlocks)
        bubbleLayout->addWidget(new MapEmbedWidget(spec.query, spec.zoom));

    // Lets someone check what the model actually produced (including the
    // literal ```html source) versus the rendered/live result — only shown
    // when there's actually an html block to compare against; a plain-
    // Markdown or map-only reply has nothing a raw view would add.
    if (!htmlBlocks.isEmpty()) {
        auto *rawBrowser = new AutoHeightTextBrowser;
        rawBrowser->setObjectName("bubbleText");
        rawBrowser->setPlainText(content);
        rawBrowser->setVisible(false);
        bubbleLayout->insertWidget(bubbleLayout->indexOf(browser) + 1, rawBrowser);

        auto *toggleRow = new QWidget;
        auto *toggleLayout = new QHBoxLayout(toggleRow);
        toggleLayout->setContentsMargins(0, 4, 0, 0);
        toggleLayout->addStretch(1);

        auto *toggleButton = new QToolButton;
        toggleButton->setObjectName("htmlRawToggleButton");
        toggleButton->setText("View source");
        toggleButton->setCheckable(true);
        toggleButton->setCursor(Qt::PointingHandCursor);
        toggleButton->setAutoRaise(true);
        connect(toggleButton, &QToolButton::toggled, browser,
                [browser, rawBrowser, htmlWidgets, toggleButton](bool showRaw) {
            browser->setVisible(!showRaw);
            for (QWidget *w : htmlWidgets)
                w->setVisible(!showRaw);
            rawBrowser->setVisible(showRaw);
            toggleButton->setText(showRaw ? "View rendered" : "View source");
        });
        toggleLayout->addWidget(toggleButton);

        bubbleLayout->addWidget(toggleRow);
    }
}

void ChatWidget::scrollToBottom()
{
    // Deferred to the next event-loop turn: a widget added this turn hasn't
    // updated the scrollbar's range yet, so jumping immediately lands one
    // message short.
    QTimer::singleShot(0, this, [this]() {
        if (m_scrollArea)
            m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
    });
}

void ChatWidget::onSendButtonClicked()
{
    if (m_isGenerating)
        onStopClicked();
    else
        onSendClicked();
}

void ChatWidget::onStopClicked()
{
    abortActiveStreamIfAny();
    m_inputEdit->setFocus();
}

void ChatWidget::abortActiveStreamIfAny()
{
    const QString conversationId = m_activeConversationId;
    if (!m_streams.contains(conversationId))
        return; // nothing streaming/queued for the conversation currently on screen

    m_chatQueue->cancel(conversationId); // no-ops on OllamaClient if this turn was still queued, not yet running

    m_streamRenderTimer->stop();
    m_streamRenderDirty = false;

    // Neither OllamaClient::abortChat() nor a still-queued cancel() emits
    // chatDone()/chatError() (see OllamaClient's header), so this replicates
    // onChatDone()'s cleanup directly. Whatever had already streamed in
    // stays (finalizeStreamingAssistantMessage() just persists it), rather
    // than discarding a partial answer.
    if (m_streamingThinkingWidget)
        m_streamingThinkingWidget->setThinking(false);
    m_store->finalizeStreamingAssistantMessage(conversationId);

    m_streams.remove(conversationId);
    m_toolRoundCountByConversation.remove(conversationId);
    m_pendingToolCallsByConversation.remove(conversationId);
    m_streamingBrowser = nullptr;
    m_streamingBubbleLayout = nullptr;
    m_streamingThinkingWidget = nullptr;
    m_streamingToolCallWidgets.clear();

    setInputEnabled(true);
    setSendButtonBusy(false);
}

void ChatWidget::setSendButtonBusy(bool busy)
{
    m_isGenerating = busy;

    if (busy) {
        m_sendButton->setText(QString());
        const bool dark = m_themeManager && m_themeManager->isDarkActive();
        // Flat (default): "danger" (red), reading as a stop cue on its own
        // with no background to help it stand out. Filled (classic look):
        // "onAccent" (white), for contrast against the accent-colored pill
        // that's still there in that mode.
        m_sendButton->setIcon(Theme::loadThemedIcon(
            ":/icons/stop.svg", dark, 14, m_sendButtonFilled ? "onAccent" : "danger"));
        m_sendButton->setToolTip("Stop generating");
    } else {
        m_sendButton->setToolTip(QString());
        // Re-applies whichever style is actually configured (plane/arrow/
        // text) rather than duplicating that logic here.
        setSendButtonStyle(m_sendButtonStyle);
    }
}

void ChatWidget::onSendClicked()
{
    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty() && m_pendingAttachments.isEmpty())
        return;

    if (m_activeConversationId.isEmpty()) {
        // Typing straight into the empty state (nothing selected in the
        // sidebar yet) — create a conversation on the fly instead of making
        // the user click "+ New conversation" first.
        const QString id = m_store->createConversation(m_modelCombo->currentText());
        emit conversationCreated(id);
        // MainWindow listens for conversationCreated and selects the new
        // sidebar row, which calls back into setActiveConversation()
        // synchronously. Fall back to calling it ourselves in case nothing
        // is listening.
        if (m_activeConversationId.isEmpty())
            setActiveConversation(id);
    }

    const Conversation *convBefore = m_store->find(m_activeConversationId);
    const bool wasNewConversation = convBefore && convBefore->title == "New conversation";

    QStringList attachmentNames;
    for (const PendingAttachment &att : m_pendingAttachments)
        attachmentNames << att.displayName;

    ChatMessage userMessage;
    userMessage.role = "user";
    userMessage.content = text;
    userMessage.timestamp = QDateTime::currentDateTime();
    userMessage.attachmentNames = attachmentNames;
    userMessage.attachmentsContext = buildAttachmentsContext(m_pendingAttachments);
    userMessage.imagesBase64 = buildImagesBase64(m_pendingAttachments);

    m_inputEdit->clear();
    m_pendingAttachments.clear();
    rebuildAttachmentsBar();

    finalizeAndSendUserMessage(userMessage, wasNewConversation);
}

void ChatWidget::finalizeAndSendUserMessage(ChatMessage userMessage, bool wasNewConversation)
{
    m_store->appendMessage(m_activeConversationId, userMessage);
    const Conversation *convAfterAppend = m_store->find(m_activeConversationId);
    const int messageIndex = convAfterAppend ? convAfterAppend->messages.size() - 1 : -1;
    appendMessageBubble("user", userMessage.content, nullptr, userMessage.attachmentNames,
                         messageIndex, userMessage.timestamp);

    if (wasNewConversation)
        emit conversationTitleMayHaveChanged(m_activeConversationId);

    streamAssistantReplyForCurrentHistory();
}

void ChatWidget::streamAssistantReplyForCurrentHistory()
{
    const Conversation *conv = m_store->find(m_activeConversationId);
    if (!conv)
        return;

    const QString conversationId = m_activeConversationId;

    // Reserve an empty assistant bubble now, and keep both its content
    // widget and its layout — the latter is where a ThinkingSectionWidget
    // or ToolCallSectionWidget gets inserted later, above the answer text,
    // if this turn ends up thinking and/or calling a tool (see
    // onChatThinkingDelta()/handleToolCalls()). A fresh StreamState
    // replaces any stale leftover (shouldn't normally exist — sending a new
    // turn only happens once the previous one for this conversation has
    // finished/been stopped).
    m_streams[conversationId] = StreamState();
    m_streams[conversationId].baselineUsedTokens = m_usedTokensByConversation.value(conversationId, 0);
    m_streamingThinkingWidget = nullptr;
    m_streamingToolCallWidgets.clear();
    m_toolRoundCountByConversation.remove(conversationId);
    m_streamingBrowser = appendMessageBubble("assistant", QString(), &m_streamingBubbleLayout);
    setInputEnabled(false);
    setSendButtonBusy(true);

    sendTurnRequest(conversationId);
}

void ChatWidget::sendTurnRequest(const QString &conversationId)
{
    const Conversation *conv = m_store->find(conversationId);
    if (!conv)
        return;

    // Re-read fresh every call rather than passed/cached from the caller —
    // this is what picks up the assistant tool_calls + tool result
    // messages handleToolCalls()/onAllToolCallsCompleted() append between
    // rounds of the same turn.
    QJsonArray apiMessages;
    for (const ChatMessage &m : conv->messages) {
        // attachmentsContext/webSearchContext were extracted once (at
        // attach-time / send-time) and persisted on the message, so this
        // reconstructs the same augmented content on every subsequent turn
        // without re-reading files or re-searching.
        QString apiContent = m.content;
        if (!m.attachmentsContext.isEmpty())
            apiContent += (apiContent.isEmpty() ? QString() : QStringLiteral("\n\n")) + m.attachmentsContext;
        if (!m.webSearchContext.isEmpty())
            apiContent += (apiContent.isEmpty() ? QString() : QStringLiteral("\n\n")) + m.webSearchContext;

        QJsonObject obj{{"role", m.role}, {"content", apiContent}};
        if (!m.imagesBase64.isEmpty())
            obj["images"] = QJsonArray::fromStringList(m.imagesBase64);
        // Resent verbatim so the model sees exactly what it itself asked
        // for — see Conversation::toolCalls' own comment. The matching
        // "tool" role result message(s) that follow need no special-casing
        // here: role/content above already builds them correctly, same as
        // any other message.
        if (!m.toolCalls.isEmpty())
            obj["tool_calls"] = m.toolCalls;
        apiMessages.append(obj);
    }

    // 0 (not set/unchecked) omits options.num_ctx entirely rather than
    // sending a literal 0 — see OllamaClient::sendChatMessage's comment for
    // why: 0 isn't "unlimited" in Ollama, it just means "use your own
    // default," which omitting the field already does on its own.
    const bool useCustomContextLength = QSettings().value("chat/useCustomContextLength", false).toBool();
    const int customNumCtx = useCustomContextLength
        ? QSettings().value("chat/customContextLength", 8192).toInt()
        : 0;

    // Mirrors customNumCtx's own read-fresh-at-send-time approach, but as
    // one struct instead of one variable per field — see GenerationOptions'
    // own comment for why every field is always included once the master
    // toggle is on, rather than each having its own independent on/off.
    GenerationOptions genOptions;
    genOptions.enabled = QSettings().value("chat/useCustomGenParams", false).toBool();
    genOptions.temperature = QSettings().value("chat/temperature", 0.8).toDouble();
    genOptions.topP = QSettings().value("chat/topP", 0.9).toDouble();
    genOptions.topK = QSettings().value("chat/topK", 40).toInt();
    genOptions.seed = QSettings().value("chat/seed", 0).toInt();
    genOptions.numPredict = QSettings().value("chat/numPredict", -1).toInt();
    genOptions.repeatPenalty = QSettings().value("chat/repeatPenalty", 1.1).toDouble();
    const QString stopSetting = QSettings().value("chat/stopSequences").toString();
    if (!stopSetting.isEmpty()) {
        for (const QString &s : stopSetting.split(',', Qt::SkipEmptyParts))
            genOptions.stop << s.trimmed();
    }

    // The conversation's own pinned model (set once, at creation — see
    // ConversationStore::createConversation()) is the authoritative source,
    // not the combo's live UI state: it's already correct immediately after
    // a fork (editMessage() carries the original conversation's model
    // straight into createConversationWithMessages()) without depending on
    // the model combo having been synced by setActiveConversation()'s
    // signal cascade first. Only a genuinely brand-new conversation with no
    // model recorded yet falls back to whatever the combo currently shows.
    const QString model = !conv->model.isEmpty() ? conv->model : m_modelCombo->currentText();

    // Caps how many tool-call rounds a single turn can chain through before
    // forcing a real answer — see kMaxToolCallRounds' own comment and
    // m_toolRoundCountByConversation.
    static constexpr int kMaxToolCallRounds = 4;
    const int toolRound = m_toolRoundCountByConversation.value(conversationId, 0);
    const QJsonArray tools = toolRound < kMaxToolCallRounds ? buildToolDefinitions() : QJsonArray();

    // Submitted to the queue rather than sent straight to OllamaClient —
    // see ChatQueue's header comment. If nothing else is running, this
    // starts immediately (turnStarted fires synchronously, before enqueue()
    // returns); otherwise it waits its turn, and onQueuePositionChanged()
    // shows "N chats ahead" in the meantime if this conversation is the one
    // on screen.
    ChatQueue::Turn turn;
    turn.conversationId = conversationId;
    turn.model = model;
    turn.apiMessages = apiMessages;
    turn.think = m_thinkingEnabled;
    turn.customNumCtx = customNumCtx;
    turn.genOptions = genOptions;
    turn.keepAliveSeconds = conv->keepAliveSeconds;
    turn.tools = tools;
    m_chatQueue->enqueue(turn);
}

QJsonArray ChatWidget::buildToolDefinitions() const
{
    QJsonArray tools;
    if (m_webSearchEnabled)
        tools.append(BuiltinTools::webSearchDefinition());
    if (m_calculatorEnabled)
        tools.append(BuiltinTools::calculateDefinition());
    if (m_dateTimeEnabled)
        tools.append(BuiltinTools::currentDateTimeDefinition());
    if (m_stackOverflowEnabled)
        tools.append(BuiltinTools::stackOverflowSearchDefinition());
    return tools;
}

void ChatWidget::handleToolCalls(const QString &conversationId, const QJsonArray &toolCalls)
{
    // Persisted first and exactly as Ollama emitted it — see
    // Conversation::toolCalls' own comment on why this has to round-trip
    // verbatim on the next request.
    ChatMessage callMessage;
    callMessage.role = "assistant";
    callMessage.timestamp = QDateTime::currentDateTime();
    callMessage.toolCalls = toolCalls;
    m_store->appendMessage(conversationId, callMessage);

    m_toolRoundCountByConversation[conversationId] = m_toolRoundCountByConversation.value(conversationId, 0) + 1;

    if (conversationId == m_activeConversationId && m_streamingBubbleLayout) {
        if (m_streamingThinkingWidget)
            m_streamingThinkingWidget->setThinking(false);

        m_streamingToolCallWidgets.clear();
        // Just above the (still-empty) answer text, which is always the
        // last item in the bubble layout — see appendMessageBubble(). Later
        // rounds' widgets land between earlier rounds' and the answer text,
        // preserving chronological top-to-bottom order.
        int insertPos = m_streamingBubbleLayout->count() - 1;
        for (const QJsonValue &callValue : toolCalls) {
            const BuiltinTools::ParsedToolCall parsed = BuiltinTools::parseToolCall(callValue.toObject());
            auto *widget = new ToolCallSectionWidget(parsed.name, parsed.arguments);
            m_streamingBubbleLayout->insertWidget(insertPos++, widget);
            m_streamingToolCallWidgets.append(widget);
        }
        scrollToBottom();
    }

    m_toolExecutor->executeToolCalls(conversationId, toolCalls);
}

void ChatWidget::onToolCallCompleted(const QString &conversationId, int callIndex, const QString &toolName,
                                      const QJsonObject &arguments, const QString &resultText)
{
    Q_UNUSED(toolName);
    Q_UNUSED(arguments);
    if (conversationId != m_activeConversationId)
        return;
    if (callIndex >= 0 && callIndex < m_streamingToolCallWidgets.size())
        m_streamingToolCallWidgets[callIndex]->setResult(resultText);
}

void ChatWidget::onAllToolCallsCompleted(const QString &conversationId, const QVector<ToolCallResult> &results)
{
    for (const ToolCallResult &result : results) {
        ChatMessage toolMessage;
        toolMessage.role = "tool";
        toolMessage.content = result.resultText;
        toolMessage.toolName = result.name;
        toolMessage.timestamp = QDateTime::currentDateTime();
        m_store->appendMessage(conversationId, toolMessage);
    }

    continueTurnAfterToolResults(conversationId);
}

void ChatWidget::continueTurnAfterToolResults(const QString &conversationId)
{
    auto it = m_streams.find(conversationId);
    if (it == m_streams.end())
        return; // turn was stopped/errored while tools were still executing — nothing to continue

    StreamState &st = it.value();
    st.buffer.clear();
    st.thinkingBuffer.clear();
    st.isThinkingActive = false;
    st.startMs = 0;
    st.tokenCount = 0;
    st.baselineUsedTokens = m_usedTokensByConversation.value(conversationId, 0);

    sendTurnRequest(conversationId);
}

void ChatWidget::editMessage(int messageIndex, const QString &newText)
{
    const Conversation *conv = m_store->find(m_activeConversationId);
    if (!conv || messageIndex < 0 || messageIndex >= conv->messages.size())
        return;

    // "The last prompt" = no other user message follows it in this
    // conversation — editing that one only discards its own (about to be
    // regenerated) reply, so it's safe to edit in place. Editing an earlier
    // one would destroy real conversation built on top of it, so that forks
    // into a new chat instead (with confirmation) rather than truncating
    // the original.
    bool isLastUserMessage = true;
    for (int i = messageIndex + 1; i < conv->messages.size(); ++i) {
        if (conv->messages[i].role == "user") {
            isLastUserMessage = false;
            break;
        }
    }

    if (!isLastUserMessage) {
        // Same QMessageBox-with-custom-buttons pattern MainWindow uses for
        // its own destructive-action confirmation (delete chat).
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle("Start a New Conversation?");
        box.setText("Editing an earlier message will start a new conversation with your edit.");
        box.setInformativeText("This chat will stay exactly as it is.");
        QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
        QPushButton *continueButton = box.addButton("Start New Conversation", QMessageBox::AcceptRole);
        box.setDefaultButton(continueButton);
        Q_UNUSED(cancelButton);

        box.exec();
        if (box.clickedButton() != continueButton)
            return; // leaves the inline editor open with their typed text, untouched

        // Deliberately does NOT abort the original conversation's stream:
        // its own dialog text promises "This chat will stay exactly as it
        // is," and now that streams are per-conversation (see m_streams),
        // nothing about forking a new one requires stopping it — it keeps
        // generating in the background exactly as it would if you'd simply
        // switched away without editing anything.

        // Copies (not references) everything up to the edited message —
        // conv may be invalidated the moment the store gains a new
        // conversation below, so nothing from it is touched after this point.
        QVector<ChatMessage> seedMessages(conv->messages.begin(), conv->messages.begin() + messageIndex);
        ChatMessage edited = conv->messages[messageIndex];
        edited.content = newText;
        edited.timestamp = QDateTime::currentDateTime();
        seedMessages.append(edited);
        const QString model = conv->model;

        const QString newId = m_store->createConversationWithMessages(model, seedMessages);
        emit conversationCreated(newId);
        // Same fallback as onSendClicked(): conversationCreated() should
        // already have triggered setActiveConversation(newId) synchronously
        // via MainWindow's sidebar-selection wiring, but cover the case
        // where nothing is listening.
        if (m_activeConversationId != newId)
            setActiveConversation(newId);

        streamAssistantReplyForCurrentHistory();
        return;
    }

    abortActiveStreamIfAny(); // an edit elsewhere shouldn't race with a reply still streaming into a bubble that's about to disappear

    // Keeps the original message's attachments/images — only the typed
    // text and timestamp change.
    ChatMessage edited = conv->messages[messageIndex];
    edited.content = newText;
    edited.timestamp = QDateTime::currentDateTime();

    m_store->truncateMessagesFrom(m_activeConversationId, messageIndex);
    m_store->appendMessage(m_activeConversationId, edited);

    renderConversation();
    streamAssistantReplyForCurrentHistory();
}

void ChatWidget::retryMessage(int messageIndex)
{
    const Conversation *conv = m_store->find(m_activeConversationId);
    if (!conv || messageIndex < 0 || messageIndex >= conv->messages.size())
        return;

    abortActiveStreamIfAny();

    // Drops only what followed this message (the old reply), not the
    // message itself — same prompt, fresh answer.
    m_store->truncateMessagesFrom(m_activeConversationId, messageIndex + 1);

    renderConversation();
    streamAssistantReplyForCurrentHistory();
}

void ChatWidget::onChatThinkingDelta(const QString &conversationId, const QString &tokenText)
{
    StreamState &st = m_streams[conversationId]; // must already exist (created by streamAssistantReplyForCurrentHistory)
    st.thinkingBuffer += tokenText;
    st.isThinkingActive = true;

    if (conversationId != m_activeConversationId)
        return; // still streaming into ConversationStore-visible state, just not into any on-screen widget right now

    if (!m_streamingThinkingWidget && m_streamingBubbleLayout) {
        m_streamingThinkingWidget = new ThinkingSectionWidget;
        m_streamingThinkingWidget->setThinking(true);
        m_streamingBubbleLayout->insertWidget(0, m_streamingThinkingWidget); // above the answer text
    }
    if (m_streamingThinkingWidget)
        m_streamingThinkingWidget->appendThinkingText(tokenText);
    scrollToBottom();
}

void ChatWidget::onChatDelta(const QString &conversationId, const QString &tokenText)
{
    StreamState &st = m_streams[conversationId];

    // The first real content token marks the end of the thinking phase for
    // this turn, if it had one at all.
    st.isThinkingActive = false;
    if (conversationId == m_activeConversationId && m_streamingThinkingWidget)
        m_streamingThinkingWidget->setThinking(false);

    // Ollama only reports exact token counts once the turn is done (see
    // OllamaClient::sendChatMessage), so this is a live estimate: one
    // content delta ~= one token in practice, timed from the first one.
    // onChatUsage() refines it against the server's real eval_count once
    // the turn finishes.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (st.startMs == 0)
        st.startMs = nowMs;
    ++st.tokenCount;
    const qint64 elapsedMs = nowMs - st.startMs;
    if (elapsedMs > 0) {
        st.lastTokensPerSecond = st.tokenCount / (elapsedMs / 1000.0);
        m_lastTokensPerSecondByConversation[conversationId] = st.lastTokensPerSecond;
    }

    // Same live-estimate approach as tok/s just above: the context-usage
    // bar/label would otherwise sit frozen at the previous turn's total for
    // this whole reply, only jumping to the real number once onChatUsage()
    // arrives at the very end. baselineUsedTokens + tokenCount is corrected
    // to the exact real prompt_eval_count/eval_count either way once it does.
    m_usedTokensByConversation[conversationId] = st.baselineUsedTokens + st.tokenCount;

    st.buffer += tokenText;

    // updateStreamingAssistantMessage() persists into ConversationStore's
    // in-memory state regardless of whether this conversation is on screen
    // — this is what makes a background reply's *content* actually keep
    // building while another conversation is shown, even though nothing
    // visible updates for it until you switch back.
    m_store->updateStreamingAssistantMessage(conversationId, st.buffer);

    if (conversationId != m_activeConversationId)
        return;

    // The actual render work happens in flushStreamRender(), at a capped
    // rate — see m_streamRenderTimer's own comment. Only arming it when
    // it's not already running (rather than restarting it every token, i.e.
    // debouncing) is what gives a fixed ~20 Hz cadence regardless of how
    // fast tokens are arriving, instead of a timer that keeps getting
    // pushed back and never actually fires until tokens stop.
    m_streamRenderDirty = true;
    if (!m_streamRenderTimer->isActive())
        m_streamRenderTimer->start();
}

void ChatWidget::flushStreamRender()
{
    if (!m_streamRenderDirty)
        return;
    m_streamRenderDirty = false;

    // Re-reads the active conversation's current stream state fresh, rather
    // than capturing anything from whenever the timer was armed — any
    // number of onChatDelta() calls could have landed in between, and this
    // always renders whatever's accumulated as of right now.
    if (m_streamingBrowser) {
        const StreamState &st = m_streams[m_activeConversationId];
        m_streamingBrowser->setMarkdownWithHtmlBlocks(livePreviewText(st.buffer));
    }
    scrollToBottom();
    updateContextUsageDisplay();
}

void ChatWidget::onChatToolCalls(const QString &conversationId, const QJsonArray &toolCalls)
{
    m_pendingToolCallsByConversation[conversationId] = toolCalls;
}

void ChatWidget::onChatDone(const QString &conversationId)
{
    const QJsonArray toolCalls = m_pendingToolCallsByConversation.take(conversationId);
    if (!toolCalls.isEmpty()) {
        // The turn isn't finished — handleToolCalls() either continues it
        // itself (once tool execution resolves) or, if the round cap was
        // hit, sendTurnRequest() omits "tools" on the next round so the
        // model's own following chatDone() arrives with no tool_calls and
        // falls through to the normal-finish path below instead.
        handleToolCalls(conversationId, toolCalls);
        return;
    }

    m_store->finalizeStreamingAssistantMessage(conversationId);

    const StreamState st = m_streams.value(conversationId);
    m_streams.remove(conversationId);
    m_toolRoundCountByConversation.remove(conversationId);

    if (conversationId != m_activeConversationId)
        return; // background turn finished; its bubble will render as static/final next time this conversation is shown

    // Nothing left to coalesce now that the reply is done, and the final
    // render just below is unconditional either way — stopping this avoids
    // a stray flushStreamRender() firing right after m_streamingBrowser is
    // cleared below (harmless — it guards on that being non-null — but
    // pointless work all the same).
    m_streamRenderTimer->stop();
    m_streamRenderDirty = false;

    if (m_streamingThinkingWidget)
        m_streamingThinkingWidget->setThinking(false);

    // Re-renders the now-complete reply through the map-block-aware path —
    // during streaming, onChatDelta() only ever calls setMarkdownWithHtmlBlocks()
    // directly (a ```map fence isn't meaningful until it's actually closed),
    // so this is what upgrades a bubble that turned out to contain one from
    // showing raw fenced JSON to an actual embedded map.
    if (m_streamingBrowser && m_streamingBubbleLayout)
        renderAssistantContent(m_streamingBrowser, m_streamingBubbleLayout, st.buffer);

    m_streamingBrowser = nullptr;
    m_streamingBubbleLayout = nullptr;
    m_streamingThinkingWidget = nullptr;
    m_streamingToolCallWidgets.clear();
    setInputEnabled(true);
    setSendButtonBusy(false);
    m_inputEdit->setFocus();
}

void ChatWidget::onChatError(const QString &conversationId, const QString &message)
{
    m_streams.remove(conversationId);
    m_toolRoundCountByConversation.remove(conversationId);
    m_pendingToolCallsByConversation.remove(conversationId);

    if (conversationId != m_activeConversationId)
        return; // background turn failed; whatever it streamed before failing is already persisted

    m_streamRenderTimer->stop();
    m_streamRenderDirty = false;

    appendMessageBubble("error", message);

    m_streamingBrowser = nullptr;
    m_streamingBubbleLayout = nullptr;
    m_streamingThinkingWidget = nullptr;
    m_streamingToolCallWidgets.clear();
    setInputEnabled(true);
    setSendButtonBusy(false);
}

void ChatWidget::onChatUsage(const QString &conversationId, int promptTokens, int completionTokens)
{
    if (conversationId.isEmpty())
        return;
    m_usedTokensByConversation[conversationId] = promptTokens + completionTokens;

    // Recompute against Ollama's own completion-token count now that it's
    // known, rather than leaving the streaming estimate as the final value.
    // Emitted (see OllamaClient) before chatDone() specifically so the
    // StreamState this reads is still present.
    auto it = m_streams.find(conversationId);
    if (it != m_streams.end() && it->startMs != 0) {
        const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - it->startMs;
        if (elapsedMs > 0 && completionTokens > 0) {
            it->lastTokensPerSecond = completionTokens / (elapsedMs / 1000.0);
            m_lastTokensPerSecondByConversation[conversationId] = it->lastTokensPerSecond;
        }
    }

    if (conversationId == m_activeConversationId)
        updateContextUsageDisplay();
}

void ChatWidget::onModelContextLengthFetched(const QString &model, int contextLength)
{
    if (contextLength > 0)
        m_contextLengthByModel[model] = contextLength;

    const Conversation *conv = m_store->find(m_activeConversationId);
    if (conv && conv->model == model)
        updateContextUsageDisplay();
}

void ChatWidget::onModelMetadataFetched(const QString &model, const ModelMetadata &metadata)
{
    if (!metadata.isEmpty())
        m_modelMetadataByModel[model] = metadata;

    const Conversation *conv = m_store->find(m_activeConversationId);
    if (conv && conv->model == model)
        updateContextUsageDisplay();
}

void ChatWidget::onModelComboChanged(int index)
{
    Q_UNUSED(index);
    const QString model = m_modelCombo->currentText();
    if (model.isEmpty())
        return;

    // The combo is only ever enabled while the active conversation has no
    // messages yet (see setActiveConversation()), so reaching here with a
    // real change means the user is picking a model before their first
    // message — persist it onto the conversation itself so it's what
    // streamAssistantReplyForCurrentHistory() actually sends, not just
    // whatever model the conversation happened to be created with.
    if (!m_activeConversationId.isEmpty()) {
        const Conversation *conv = m_store->find(m_activeConversationId);
        if (conv && conv->messages.isEmpty())
            m_store->setConversationModel(m_activeConversationId, model);
    }

    ensureContextLengthKnown(model);
    updateContextUsageDisplay();
}

void ChatWidget::onKeepAliveComboChanged(int index)
{
    if (m_activeConversationId.isEmpty())
        return;
    const int keepAliveSeconds = m_keepAliveCombo->itemData(index).toInt();
    m_store->setConversationKeepAlive(m_activeConversationId, keepAliveSeconds);
}

void ChatWidget::onQueueTurnStarted(const QString &conversationId)
{
    // Clears any "Waiting…" placeholder text left by onQueuePositionChanged()
    // now that this conversation's turn has actually started — real tokens
    // are about to start arriving and will overwrite it anyway, but this
    // avoids a stale placeholder lingering if the turn errors before any
    // token comes through.
    if (conversationId == m_activeConversationId && m_streamingBrowser)
        m_streamingBrowser->setPlainText(QString());
}

void ChatWidget::onQueuePositionChanged(const QString &conversationId, int aheadCount)
{
    if (conversationId != m_activeConversationId || !m_streamingBrowser || aheadCount <= 0)
        return;

    m_streamingBrowser->setPlainText(aheadCount == 1
        ? "Waiting for another chat to finish…"
        : QString("Waiting — %1 chats ahead in the queue…").arg(aheadCount));
}

void ChatWidget::ensureContextLengthKnown(const QString &model)
{
    if (model.isEmpty() || m_contextLengthByModel.contains(model))
        return;
    m_ollamaClient->fetchModelContextLength(model);
}

void ChatWidget::updateContextUsageDisplay()
{
    const int used = m_usedTokensByConversation.value(m_activeConversationId, 0);
    const QString model = m_modelCombo->currentText();
    const int modelMaxContext = m_contextLengthByModel.value(model, 0);

    // If a custom context length is set, it's what's actually being
    // requested (see streamAssistantReplyForCurrentHistory()) — but Ollama
    // still hard-clamps to the model's own trained maximum regardless, so
    // the smaller of the two is the real effective ceiling. Whichever one
    // isn't known yet (0) just falls out of the min() naturally.
    const bool useCustomContextLength = QSettings().value("chat/useCustomContextLength", false).toBool();
    const int customContextLength = useCustomContextLength
        ? QSettings().value("chat/customContextLength", 8192).toInt()
        : 0;
    const int maxContext = (modelMaxContext > 0 && customContextLength > 0)
        ? qMin(modelMaxContext, customContextLength)
        : qMax(modelMaxContext, customContextLength);

    const double lastTokensPerSecond = m_lastTokensPerSecondByConversation.value(m_activeConversationId, 0.0);
    const QString speedSuffix = lastTokensPerSecond > 0.0
        ? QString(" · %1 tok/s").arg(lastTokensPerSecond, 0, 'f', 1)
        : QString();

    // Family/parameter-size/quantization, e.g. "llama 7B Q4_K_M" — appended
    // after tok/s since it's the same "about this generation" status line,
    // and only ever shown once fetchModelContextLength()'s /api/show call
    // has resolved for the active model (any field Ollama didn't report is
    // just skipped rather than leaving a stray blank).
    const ModelMetadata metadata = m_modelMetadataByModel.value(model);
    QStringList metadataParts;
    if (!metadata.family.isEmpty())
        metadataParts << metadata.family;
    if (!metadata.parameterSize.isEmpty())
        metadataParts << metadata.parameterSize;
    if (!metadata.quantizationLevel.isEmpty())
        metadataParts << metadata.quantizationLevel;
    const QString metadataSuffix = metadataParts.isEmpty()
        ? QString()
        : QString(" · %1").arg(metadataParts.join(' '));

    if (maxContext > 0) {
        const int percent = qBound(0, static_cast<int>((double(used) / double(maxContext)) * 100.0), 100);
        m_contextUsageProgress->setValue(percent);

        // Flip the bar red near the ceiling — see Theme.cpp for the
        // [nearLimit="true"] rule this property drives.
        m_contextUsageProgress->setProperty("nearLimit", percent >= 85);
        m_contextUsageProgress->style()->unpolish(m_contextUsageProgress);
        m_contextUsageProgress->style()->polish(m_contextUsageProgress);

        m_contextUsageLabel->setText(
            QString("%1 / %2 tokens (%3%)%4%5").arg(used).arg(maxContext).arg(percent).arg(speedSuffix, metadataSuffix));
    } else {
        m_contextUsageProgress->setValue(0);
        m_contextUsageLabel->setText(used > 0
            ? QString("%1 tokens used (context size unknown)%2%3").arg(used).arg(speedSuffix, metadataSuffix)
            : "Context: —");
    }
}

void ChatWidget::onAttachClicked()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, "Attach files to message");
    if (paths.isEmpty())
        return;

    for (const QString &path : paths) {
        PendingAttachment att;
        att.filePath = path;
        att.displayName = QFileInfo(path).fileName();
        att.isImage = isImageFile(path);
        m_pendingAttachments.append(att);
    }
    rebuildAttachmentsBar();
}

void ChatWidget::onJumpToClicked()
{
    if (m_userMessageMarkers.isEmpty())
        return;

    // Built fresh each click rather than kept around — same one-shot popup
    // pattern MainWindow uses for its own context menus (see
    // MainWindow::buildDeleteMenu).
    QMenu menu(this);
    // See MainWindow::buildDeleteMenu()'s own comment.
    menu.setAttribute(Qt::WA_TranslucentBackground);
    for (const UserMessageMarker &marker : m_userMessageMarkers) {
        QAction *action = menu.addAction(marker.preview);
        QWidget *row = marker.row;
        connect(action, &QAction::triggered, this, [this, row]() {
            m_scrollArea->ensureWidgetVisible(row, 0, 40);
        });
    }
    menu.exec(m_jumpToButton->mapToGlobal(QPoint(0, m_jumpToButton->height())));
}

void ChatWidget::onWebSearchToggled(bool enabled)
{
    m_webSearchEnabled = enabled;
    updateToolsButtonAppearance();
}

void ChatWidget::onCalculatorToggled(bool enabled)
{
    m_calculatorEnabled = enabled;
    updateToolsButtonAppearance();
}

void ChatWidget::onDateTimeToolToggled(bool enabled)
{
    m_dateTimeEnabled = enabled;
    updateToolsButtonAppearance();
}

void ChatWidget::onStackOverflowToggled(bool enabled)
{
    m_stackOverflowEnabled = enabled;
    updateToolsButtonAppearance();
}

void ChatWidget::onThinkingToggled(bool enabled)
{
    m_thinkingEnabled = enabled;
    updateToolsButtonAppearance();
}

void ChatWidget::updateToolsButtonAppearance()
{
    QStringList activeParts;
    if (m_webSearchEnabled)
        activeParts << "Wiki";
    if (m_stackOverflowEnabled)
        activeParts << "SO";
    if (m_calculatorEnabled)
        activeParts << "Calc";
    if (m_dateTimeEnabled)
        activeParts << "Date/time";
    if (!m_thinkingEnabled)
        activeParts << "No thinking";

    m_toolsButton->setText(activeParts.isEmpty() ? "Tools" : "Tools: " + activeParts.join(", "));
    // Drives Theme.cpp's #toolsButton[active="true"] rule — an accent tint
    // so an enabled tool stays visible even with the menu closed.
    m_toolsButton->setProperty("active", !activeParts.isEmpty());
    m_toolsButton->style()->unpolish(m_toolsButton);
    m_toolsButton->style()->polish(m_toolsButton);
    reloadThemedIcons(); // each menu item's icon tints to accent while its own toggle is on
}

ChatWidget::~ChatWidget()
{
    if (!m_pendingVoiceFilePath.isEmpty())
        QFile::remove(m_pendingVoiceFilePath);
}

void ChatWidget::onVoicePressed()
{
    m_voiceButton->setProperty("recording", true);
    m_voiceButton->style()->unpolish(m_voiceButton);
    m_voiceButton->style()->polish(m_voiceButton);

    // Live mode (transcribing in short chunks *while* still recording,
    // rather than once at release) needs both a whisper-server binary
    // available AND Settings' "Enable live transcription" checkbox on —
    // off by default even when the binary is there, since it's a real
    // behavior change from the classic push-to-talk flow, not something to
    // silently switch on just because a build step happened to succeed.
    // ensureLiveServerRunning() is safe to call every press either way: a
    // no-op if already up against the right model, and doesn't block
    // waiting for readiness — any chunk that arrives before it's actually
    // ready just gets queued (see WhisperManager::sendNextQueuedChunk()).
    const bool live = m_whisperManager && m_whisperManager->isServerBinaryAvailable()
        && QSettings().value("voice/liveTranscriptionEnabled", false).toBool();
    if (live) {
        m_inputEdit->clear();
        m_whisperManager->ensureLiveServerRunning();
    }
    m_voiceRecorder.startRecording(live);
    reloadThemedIcons();
}

void ChatWidget::onVoiceReleased()
{
    m_voiceButton->setProperty("recording", false);
    m_voiceButton->style()->unpolish(m_voiceButton);
    m_voiceButton->style()->polish(m_voiceButton);
    // Result arrives asynchronously either way — via onVoiceRecordingFinished()
    // for a normal (non-live) recording, or onVoiceLiveChunkReady() with
    // isFinalChunk=true for a live one (VoiceRecorder itself remembers
    // which mode startRecording() was called with).
    m_voiceRecorder.stopRecording();
    reloadThemedIcons();
}

void ChatWidget::onVoiceRecordingFinished(const QString &filePath)
{
    if (!m_whisperManager) {
        QFile::remove(filePath);
        appendMessageBubble("error", "Voice recorded, but Whisper isn't available in this build.");
        return;
    }

    // Kept until onWhisperTranscriptionFinished() fires — whisper-cli runs
    // as a real subprocess (see WhisperManager::transcribe), so this is
    // async rather than a same-call return.
    m_pendingVoiceFilePath = filePath;
    // Disabled for the same reason it's disabled mid-reply: whisper-cli is
    // about to start overwriting this box's contents live (see
    // onWhisperTranscriptionProgress()), so typing into it at the same time
    // would just get clobbered. Re-enabled in onWhisperTranscriptionFinished().
    setInputEnabled(false);
    m_inputEdit->clear();
    m_whisperManager->transcribe(filePath);
}

void ChatWidget::onVoiceRecordingFailed(const QString &errorMessage)
{
    appendMessageBubble("error", "Voice recording failed: " + errorMessage);
}

void ChatWidget::onWhisperTranscriptionProgress(const QString &partialText)
{
    m_inputEdit->setPlainText(partialText);
    m_inputEdit->moveCursor(QTextCursor::End);
}

void ChatWidget::onWhisperTranscriptionFinished(const QString &text, bool success, const QString &error)
{
    if (!m_pendingVoiceFilePath.isEmpty()) {
        // Nothing keeps the raw audio around once whisper-cli is done with
        // it — VoiceRecorder writes it to a tmpfs-backed path when one's
        // available (see its own header comment) so it never really touches
        // a disk either, but it's still an on-disk path as far as this app
        // is concerned, hence the explicit remove() rather than relying on
        // the OS to eventually reclaim /tmp.
        QFile::remove(m_pendingVoiceFilePath);
        m_pendingVoiceFilePath.clear();
    }

    setInputEnabled(true);

    if (!success) {
        appendMessageBubble("error", "Voice transcription failed: " + error);
        return;
    }

    m_inputEdit->setPlainText(text);

    if (m_voiceAutoSend) {
        // Explicitly opted into this in Settings — the review step below is
        // skipped on purpose.
        onSendClicked();
        return;
    }

    // Default: fills the box and hands focus back, but deliberately does
    // NOT send — speech-to-text is error-prone enough that silently
    // auto-submitting whatever it guessed was more surprising than helpful.
    // The person can read it over, fix anything wrong, and hit Send themselves.
    m_inputEdit->moveCursor(QTextCursor::End);
    m_inputEdit->setFocus();
}

void ChatWidget::onVoiceLiveChunkReady(const QByteArray &wavData, bool isFinalChunk)
{
    if (!m_whisperManager)
        return;

    if (wavData.isEmpty()) {
        // Nothing new since the last chunk boundary — still need to close
        // out the recording if this was the final one (e.g. a chunk
        // boundary happened to land exactly at button release).
        if (isFinalChunk)
            finishLiveVoiceRecording();
        return;
    }

    m_whisperManager->transcribeChunkLive(wavData, isFinalChunk);
}

void ChatWidget::onWhisperLiveChunkTranscribed(const QString &text, bool isFinalChunk, bool success, const QString &error)
{
    if (success && !text.isEmpty()) {
        // Appended, not replaced — each chunk is its own already-finished
        // piece of transcription (unlike onWhisperTranscriptionProgress()'s
        // single growing partial for one whole-file transcription), so the
        // box just keeps building up sentence by sentence as the person talks.
        QString current = m_inputEdit->toPlainText();
        if (!current.isEmpty() && !current.endsWith(' '))
            current += ' ';
        current += text;
        m_inputEdit->setPlainText(current);
        m_inputEdit->moveCursor(QTextCursor::End);
    } else if (!success) {
        // Best-effort: one failed chunk (a dropped request, the server
        // briefly unavailable) doesn't interrupt the recording or spam an
        // error bubble — whatever text did come through from other chunks
        // is still worth keeping.
        qWarning() << "Live transcription chunk failed:" << error;
    }

    if (isFinalChunk)
        finishLiveVoiceRecording();
}

void ChatWidget::onWhisperLiveServerStateChanged(bool running, const QString &error)
{
    if (!running && !error.isEmpty())
        appendMessageBubble("error", "Live transcription: " + error);
}

void ChatWidget::finishLiveVoiceRecording()
{
    if (m_voiceAutoSend && !m_inputEdit->toPlainText().trimmed().isEmpty())
        onSendClicked();
}

void ChatWidget::reloadThemedIcons()
{
    // updateToolsButtonAppearance() calls this to refresh the web-search/
    // thinking icons' tint, and it's invoked once during construction
    // before m_voiceButton (built later in the constructor) exists yet —
    // guard each widget rather than relying on call order.
    if (!m_webSearchAction || !m_stackOverflowAction || !m_thinkingAction || !m_voiceButton)
        return;

    const bool dark = m_themeManager && m_themeManager->isDarkActive();

    // Each icon's own color reflects its own toggle/recording state —
    // accent (or danger, for an active recording) when on, the normal
    // secondary-text tone otherwise — rather than being tied to any
    // "non-default" summary logic elsewhere.
    m_webSearchAction->setIcon(Theme::loadThemedIcon(
        ":/icons/web-search.svg", dark, 16, m_webSearchEnabled ? "accent" : "secondaryText"));
    // Same search icon as Wikipedia — both are "look this up online" tools,
    // just different sources; no separate asset needed to tell them apart
    // since their labels already do that.
    m_stackOverflowAction->setIcon(Theme::loadThemedIcon(
        ":/icons/web-search.svg", dark, 16, m_stackOverflowEnabled ? "accent" : "secondaryText"));
    m_thinkingAction->setIcon(Theme::loadThemedIcon(
        ":/icons/thinking.svg", dark, 16, m_thinkingEnabled ? "accent" : "secondaryText"));
    // The button's own "recording" property, set synchronously in
    // onVoicePressed()/onVoiceReleased() — simpler than reading
    // m_voiceRecorder.isRecording() and just as correct now that
    // stopRecording() updates its own state synchronously too.
    const bool recordingUi = m_voiceButton->property("recording").toBool();
    m_voiceButton->setIcon(Theme::loadThemedIcon(
        ":/icons/microphone.svg", dark, 16, recordingUi ? "danger" : "secondaryText"));

    reloadSendButtonIcon(); // also theme-dependent, though driven by m_sendButtonStyle rather than a toggle
}

void ChatWidget::reloadSendButtonIcon()
{
    if (!m_sendButton || m_sendButtonStyle != "plane") {
        if (m_sendButton)
            m_sendButton->setIcon(QIcon());
        return;
    }

    const bool dark = m_themeManager && m_themeManager->isDarkActive();
    // Flat (default): "accent", since the icon itself has to carry the
    // color that used to come from a solid accent-colored fill. Filled
    // (classic look): "onAccent" (white), for contrast against that fill
    // now that it's back.
    m_sendButton->setIcon(Theme::loadThemedIcon(
        ":/icons/send.svg", dark, 16, m_sendButtonFilled ? "onAccent" : "accent"));
}

void ChatWidget::rebuildAttachmentsBar()
{
    // Index 0 is always the earliest remaining chip while the trailing
    // stretch (added once, in the constructor) sits last — same pattern as
    // clearMessages().
    while (m_attachmentsLayout->count() > 1) {
        QLayoutItem *item = m_attachmentsLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    for (const PendingAttachment &att : m_pendingAttachments) {
        auto *chip = new QFrame;
        chip->setObjectName("attachmentChip");
        auto *chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(8, 3, 4, 3);
        chipLayout->setSpacing(4);

        auto *nameLabel = new QLabel(att.displayName);
        nameLabel->setObjectName("attachmentChipLabel");
        chipLayout->addWidget(nameLabel);

        auto *removeButton = new QToolButton;
        removeButton->setObjectName("attachmentChipRemove");
        removeButton->setText(QString::fromUtf8("\xC3\x97")); // "×" — plain glyph, not an emoji (see ThinkingSectionWidget's spinner for why emoji are avoided here)
        removeButton->setToolTip("Remove");
        removeButton->setCursor(Qt::PointingHandCursor);
        removeButton->setAutoRaise(true);
        const QString filePath = att.filePath;
        connect(removeButton, &QToolButton::clicked, this, [this, filePath]() {
            removeAttachmentByPath(filePath);
        });
        chipLayout->addWidget(removeButton);

        m_attachmentsLayout->insertWidget(m_attachmentsLayout->count() - 1, chip);
    }

    m_attachmentsBar->setVisible(!m_pendingAttachments.isEmpty());
}

void ChatWidget::removeAttachmentByPath(const QString &filePath)
{
    for (int i = 0; i < m_pendingAttachments.size(); ++i) {
        if (m_pendingAttachments[i].filePath == filePath) {
            m_pendingAttachments.removeAt(i);
            break;
        }
    }
    rebuildAttachmentsBar();
}

bool ChatWidget::isImageFile(const QString &path)
{
    static const QStringList imageExtensions = {"png", "jpg", "jpeg", "gif", "bmp", "webp"};
    return imageExtensions.contains(QFileInfo(path).suffix().toLower());
}

QString ChatWidget::buildAttachmentsContext(const QVector<PendingAttachment> &attachments) const
{
    QString context;
    for (const PendingAttachment &att : attachments) {
        if (att.isImage)
            continue; // images travel via the "images" field instead, see buildImagesBase64()

        QFile file(att.filePath);
        if (!file.open(QIODevice::ReadOnly))
            continue;

        const QByteArray bytes = file.readAll();
        if (bytes.contains('\0'))
            continue; // looks binary — don't dump garbage into the model's context

        if (!context.isEmpty())
            context += "\n\n";
        context += QString("--- Attached file: %1 ---\n%2\n--- End of %1 ---")
                       .arg(att.displayName, QString::fromUtf8(bytes));
    }
    return context;
}

QStringList ChatWidget::buildImagesBase64(const QVector<PendingAttachment> &attachments) const
{
    QStringList images;
    for (const PendingAttachment &att : attachments) {
        if (!att.isImage)
            continue;
        QFile file(att.filePath);
        if (file.open(QIODevice::ReadOnly))
            images << QString::fromLatin1(file.readAll().toBase64());
    }
    return images;
}

bool ChatWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_inputEdit && m_inputCard
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        m_inputCard->setProperty("focused", event->type() == QEvent::FocusIn);
        m_inputCard->style()->unpolish(m_inputCard);
        m_inputCard->style()->polish(m_inputCard);
    } else if (watched == m_emptyStatePanel && event->type() == QEvent::Resize) {
        updateEmptyStateInputWidth();
    }
    return QWidget::eventFilter(watched, event);
}

void ChatWidget::setInputEnabled(bool enabled)
{
    m_inputEdit->setEnabled(enabled);
    // m_sendButton deliberately isn't touched here — it stays enabled
    // throughout a reply so it can double as the stop button (see
    // setSendButtonBusy()/onSendButtonClicked()), unlike the textarea,
    // which genuinely can't be used mid-stream.
}
