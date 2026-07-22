#pragma once

#include <QWidget>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QProgressBar>
#include <QHash>
#include <QVector>
#include <QStringList>
#include <QDateTime>

#include "OllamaClient.h"
#include "ConversationStore.h"
#include "ChatQueue.h"
#include "ChatInputEdit.h"
#include "AutoHeightTextBrowser.h"
#include "VoiceRecorder.h"
#include "WhisperManager.h"
#include "MapEmbedWidget.h"
#include "HtmlEmbedWidget.h"
#include "ToolExecutor.h" // for ToolCallResult, used by the onAllToolCallsCompleted() slot signature

class QVBoxLayout;
class QHBoxLayout;
class QFrame;
class QMenu;
class QAction;
class ThinkingSectionWidget;
class ToolCallSectionWidget;
class ThemeManager;
class QTimer;

// Displays the active conversation and handles sending messages + streaming
// the assistant's reply in. Does not own OllamaClient/ConversationStore —
// both are shared with the rest of the app.
class ChatWidget : public QWidget
{
    Q_OBJECT

public:
    // themeManager isn't owned here (shared with the rest of the app, same
    // as ollamaClient/store) — only used to know light-vs-dark for
    // reloadThemedIcons() and to hear about live theme switches.
    ChatWidget(OllamaClient *ollamaClient, ConversationStore *store, ThemeManager *themeManager,
               WhisperManager *whisperManager, QWidget *parent = nullptr);
    // Deletes m_pendingVoiceFilePath if the app is closed while a
    // transcription is still in flight — otherwise it'd never get cleaned
    // up, since onWhisperTranscriptionFinished() is what normally does that
    // and it would never fire once this widget (and the transcribe() call
    // it's waiting on) is gone.
    ~ChatWidget() override;

    // Switches which conversation is displayed/edited. Pass an empty string
    // to show an empty state (e.g. no conversation selected yet).
    void setActiveConversation(const QString &conversationId);

    // Populates the model dropdown. Called once the app's model list is known.
    void setAvailableModels(const QStringList &modelNames);

    QString activeConversationId() const { return m_activeConversationId; }

    // Deletes conversationId if it's still completely untouched — no
    // messages and never renamed away from "New conversation" — and isn't
    // generating a reply in the background. A no-op otherwise (including if
    // conversationId is empty or unknown). Called from setActiveConversation()
    // for the conversation being switched *away* from, and by MainWindow when
    // the app is about to quit with an empty conversation still on screen, so
    // a chat the user never actually used doesn't linger in the sidebar/on disk.
    void discardConversationIfEmpty(const QString &conversationId);

    // Applies the "Send" button's style — one of "plane" (default), "arrow",
    // or "text" (see SettingsDialog's "Send button" combo) — and persists
    // nothing itself; the caller reads the persisted QSettings value, this
    // just applies it to the UI. Called once at construction (restoring the
    // last saved choice) and again live whenever Settings changes it.
    void setSendButtonStyle(const QString &style);

    // Applies the "Send" button's background treatment — false (default):
    // flat, matching the attach/tools/voice buttons it sits next to; true:
    // the old solid accent-colored pill. Same "caller persists, this just
    // applies" pattern as setSendButtonStyle() above — see SettingsDialog's
    // "Filled send button" checkbox.
    void setSendButtonFilled(bool filled);

    // Applies "Send automatically after transcription" live — see
    // SettingsDialog's checkbox. false (default): onWhisperTranscriptionFinished()
    // just fills the input box for review. true: it also calls
    // onSendClicked() right away, same as this feature's original behavior.
    void setVoiceAutoSend(bool enabled);

    // Re-reads "chat/useCustomContextLength"/"chat/customContextLength"
    // from QSettings and refreshes the context-usage bar accordingly —
    // called live when SettingsDialog's context-length checkbox/slider
    // changes, same pattern as setSendButtonStyle() above. The actual
    // num_ctx sent with each request is read fresh at send time (see
    // streamAssistantReplyForCurrentHistory()), so this only needs to
    // update the *display*, not anything mid-stream.
    void refreshContextLengthSetting();

    // Applies "Queing model optimization" live — see SettingsDialog's
    // checkbox and ChatQueue's own header comment for what this actually
    // changes (whether the queue may reorder turns to group by currently-
    // loaded model, vs. always running them strictly in submitted order).
    void setModelOptimizationEnabled(bool enabled);

    // Re-applies the "voice/audioInputDeviceId" QSettings value to the
    // recorder — see VoiceRecorder::refreshAudioInputDevice(). Called live
    // when SettingsDialog's microphone combo changes, same pattern as
    // refreshContextLengthSetting() above.
    void refreshAudioInputDevice();

    // Re-applies the "voice/meterSmoothingPercent" QSettings value to the
    // recorder — see VoiceRecorder::refreshMeterSmoothing(). Called live
    // when SettingsDialog's meter-smoothing slider changes.
    void refreshMeterSmoothing();

    // Re-renders the active conversation from scratch — called live when
    // any Formatting-tab slider changes (paragraph/list/heading spacing, or
    // the whole-app font-size scale). Unlike the settings above, these
    // don't have a single live "current value" to just re-apply: Qt bakes
    // the resolved font size and block-format margins directly into each
    // message's own QTextDocument content at the moment it's rendered (see
    // AutoHeightTextBrowser::setMarkdownWithHtmlBlocks()), so a setting
    // change alone doesn't retroactively touch anything already on screen —
    // only calling renderConversation() again, which reconstructs every
    // bubble from the stored message text, picks up the new values.
    void refreshFormattingSettings();

protected:
    // Watches two unrelated children, both for things QSS/layouts can't
    // express directly:
    // - m_inputEdit's focus in/out toggles m_inputCard's "focused" property,
    //   so the card can draw the accent focus ring on behalf of its child —
    //   the border lives on the outer frame now, not the textarea, so QSS's
    //   :focus pseudo-state (which only applies to the focused widget
    //   itself) can't reach it.
    // - m_emptyStatePanel's resize keeps the docked input card at 80% of
    //   the panel's width (see updateEmptyStateInputWidth()).
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    // Emitted right after a user message is appended to a *new* (still
    // "New conversation"-titled) conversation, so the sidebar can refresh
    // its label without ConversationStore needing to broadcast on every
    // single message append.
    void conversationTitleMayHaveChanged(const QString &conversationId);

    // Emitted right after ChatWidget itself creates a conversation (i.e. the
    // user typed into the empty state and hit send with nothing selected in
    // the sidebar yet), so MainWindow can select the new row.
    void conversationCreated(const QString &conversationId);

    // Straight relay of VoiceRecorder::audioLevelChanged() — MainWindow
    // wires this to StatsStripWidget::setMicLevel() so the "Mic" meter in
    // the system stats strip lives up there rather than ChatWidget reaching
    // into a sibling widget itself.
    void audioLevelChanged(qreal level);

private slots:
    // Connected to m_sendButton::clicked — dispatches to onSendClicked() or
    // onStopClicked() depending on m_isGenerating, since the button is
    // reused for both roles rather than being disabled mid-stream (see
    // setSendButtonBusy()).
    void onSendButtonClicked();
    void onSendClicked();
    // Aborts the in-flight reply and finalizes whatever partial answer had
    // already streamed in — OllamaClient::abortChat() itself emits neither
    // chatDone() nor chatError(), so this does that cleanup directly,
    // same as setActiveConversation() already does when switching
    // conversations mid-stream.
    void onStopClicked();
    void onAttachClicked();
    void onJumpToClicked();
    void onWebSearchToggled(bool enabled);
    void onCalculatorToggled(bool enabled);
    void onDateTimeToolToggled(bool enabled);
    void onStackOverflowToggled(bool enabled);
    void onThinkingToggled(bool enabled);
    void onVoicePressed();
    void onVoiceReleased();
    void onVoiceRecordingFinished(const QString &filePath);
    void onVoiceRecordingFailed(const QString &errorMessage);
    void onWhisperTranscriptionFinished(const QString &text, bool success, const QString &error);
    // Live feedback while whisper-cli is still running — see WhisperManager::
    // transcriptionProgress(). Just fills the input box with whatever's been
    // transcribed so far, same as the final text does, minus the auto-send.
    void onWhisperTranscriptionProgress(const QString &partialText);

    // Live transcription (see VoiceRecorder::liveChunkReady()/WhisperManager's
    // whisper-server integration) — used instead of onVoiceRecordingFinished/
    // onWhisperTranscriptionFinished whenever onVoicePressed() decided this
    // recording should stream, rather than transcribing once at release.
    void onVoiceLiveChunkReady(const QByteArray &wavData, bool isFinalChunk);
    void onWhisperLiveChunkTranscribed(const QString &text, bool isFinalChunk, bool success, const QString &error);
    // Surfaces a one-time error bubble if whisper-server fails to start —
    // a quiet no-op on success, since onVoicePressed() doesn't block
    // waiting for readiness before recording starts (the mic shouldn't feel
    // laggy just because the server is still warming up).
    void onWhisperLiveServerStateChanged(bool running, const QString &error);
    void onChatDelta(const QString &conversationId, const QString &tokenText);
    void onChatThinkingDelta(const QString &conversationId, const QString &tokenText);
    // Stashes toolCalls for onChatDone() (arrives just before it, from the
    // same NDJSON line — see OllamaClient::chatToolCalls()) rather than
    // handling it here directly, so onChatDone() has one single place that
    // decides whether a finished turn was a normal answer or a tool call.
    void onChatToolCalls(const QString &conversationId, const QJsonArray &toolCalls);
    void onChatDone(const QString &conversationId);
    void onChatError(const QString &conversationId, const QString &message);
    void onChatUsage(const QString &conversationId, int promptTokens, int completionTokens);
    void onModelContextLengthFetched(const QString &model, int contextLength);
    void onModelMetadataFetched(const QString &model, const ModelMetadata &metadata);
    void onModelComboChanged(int index);
    void onKeepAliveComboChanged(int index);

    // ToolExecutor's own progress signals — see handleToolCalls() for how a
    // turn ends up here, and continueTurnAfterToolResults() for what
    // happens once every call in the batch is done.
    void onToolCallCompleted(const QString &conversationId, int callIndex, const QString &toolName,
                              const QJsonObject &arguments, const QString &resultText);
    void onAllToolCallsCompleted(const QString &conversationId, const QVector<ToolCallResult> &results);

    // ChatQueue's own progress signals — see setModelOptimizationEnabled()
    // and streamAssistantReplyForCurrentHistory() for how turns get there.
    void onQueueTurnStarted(const QString &conversationId);
    void onQueuePositionChanged(const QString &conversationId, int aheadCount);

    // Fires at a fixed ~20 Hz cadence while streaming (see onChatDelta()) to
    // actually do the expensive part — re-rendering the bubble text,
    // scrolling, refreshing the context-usage bar — instead of doing all of
    // that on every single token. A no-op if nothing changed since the last
    // flush (m_streamRenderDirty).
    void flushStreamRender();

private:
    // A file queued via the "+" button, not yet attached to a sent message.
    struct PendingAttachment
    {
        QString filePath;
        QString displayName;
        bool isImage = false;
    };

    // One entry in the "Jump to" menu: a truncated preview of a user
    // message and the row widget to scroll to. Rebuilt from scratch by
    // clearMessages()/appendMessageBubble() every time the message list is
    // (re)rendered, so it never needs patching in place.
    struct UserMessageMarker
    {
        QString preview;
        QWidget *row = nullptr;
    };

    void renderConversation();
    // If the conversation being rendered has a background stream still in
    // flight (see m_streams), appends its last (assistant) message as a
    // *live* bubble instead of a static one: wires up m_streamingBrowser/
    // m_streamingBubbleLayout/m_streamingThinkingWidget to it and replays
    // whatever's already accumulated in that stream's StreamState, so future
    // tokens keep landing in it. Called from renderConversation() only when
    // that condition holds; a normal (no live stream) render never calls this.
    void reconnectStreamingBubble();
    // Live-streaming equivalent of a ```html block: rather than actually
    // re-parsing it as HTML on every single token (setMarkdownWithHtmlBlocks()
    // calls QTextDocument::setHtml() on the *whole* accumulated buffer each
    // time — for a large/growing html block, especially one with a lot of
    // inline CSS/JS like a Chart.js snippet, that's real, repeated parsing
    // work on an ever-larger string every token, which is what made the UI
    // visibly bog down while such a reply was still generating), any
    // ```html block — complete or still being typed out — is swapped for a
    // short placeholder line here. The real block only actually gets
    // rendered (as a proper HtmlEmbedWidget — see renderAssistantContent())
    // once the reply is done and the buffer stops changing every token.
    static QString livePreviewText(const QString &buffer);
    void clearMessages();
    // Appends a message row (a rounded "bubble") to the list and returns the
    // browser widget holding its text, so streaming can keep updating it in
    // place via setMarkdown()/setPlainText() rather than re-rendering the
    // whole history per token. User/error content is shown as plain text;
    // assistant content goes through renderAssistantContent(). If
    // bubbleLayoutOut is non-null, it receives the bubble's own internal
    // layout, so a ThinkingSectionWidget can be inserted above the text
    // later (see onChatThinkingDelta), or a map block appended below it
    // once streaming finishes (see onChatDone), without having to re-find
    // the bubble. attachmentNames, if non-empty, shows a small "Attached:
    // ..." caption above the message text. For a user message,
    // messageIndex (its position in Conversation::messages) and timestamp
    // drive the timestamp/Edit/Retry row shown below the text — pass -1 to
    // omit it (used for the empty assistant placeholder streaming will fill
    // in, which has no message index yet).
    AutoHeightTextBrowser *appendMessageBubble(const QString &role, const QString &content,
                                                QVBoxLayout **bubbleLayoutOut = nullptr,
                                                const QStringList &attachmentNames = {},
                                                int messageIndex = -1,
                                                const QDateTime &timestamp = QDateTime());
    // Strips ```html fenced block(s) out of `content` first — each one
    // becomes its own HtmlEmbedWidget (a real, isolated Chromium view;
    // AutoHeightTextBrowser's own HTML support has no JavaScript/<canvas>,
    // so anything script-driven needs this instead) — then ```map fenced
    // block(s) (JSON: {"query": "...", "zoom": N}) out of what's left,
    // renders the remaining text into `browser` via
    // setMarkdownWithHtmlBlocks(), and appends one MapEmbedWidget per map
    // block to `bubbleLayout` — landing after browser and any html embeds,
    // since QVBoxLayout::addWidget() always appends, and browser is
    // expected to already be the last item in bubbleLayout when this is
    // called (true both for a fresh bubble and for upgrading a
    // just-finished streaming one in onChatDone()). Also adds a "View
    // source" toggle when there's at least one html block, letting it
    // swap between the rendered/live view and the original raw reply text.
    // No html/map block present is just the normal-render case.
    void renderAssistantContent(AutoHeightTextBrowser *browser, QVBoxLayout *bubbleLayout, const QString &content);
    void setInputEnabled(bool enabled);
    void scrollToBottom();
    void ensureContextLengthKnown(const QString &model);
    void updateContextUsageDisplay();

    // Moves m_inputBar between the bottom dock (active conversation) and the
    // empty-state panel's centered dock (no conversation selected) — Qt
    // reparents a widget automatically when it's added to a different
    // layout, so this is just two addWidget() calls, not a rebuild.
    void dockInputBar(bool inEmptyState);
    // Caps m_inputBar's width at 80% of m_emptyStatePanel's current width;
    // a no-op once a conversation is active (the bottom-docked bar is
    // unconstrained). Called from dockInputBar() and from eventFilter() on
    // every panel resize, so the homepage composer scales with the window.
    void updateEmptyStateInputWidth();

    // Rebuilds the row of removable chips above the input box from
    // m_pendingAttachments — called after every add/remove rather than
    // patching individual chips in place, since the list is always small.
    void rebuildAttachmentsBar();
    void removeAttachmentByPath(const QString &filePath);
    static bool isImageFile(const QString &path);
    // Reads non-image attachments as text (skipping anything that looks
    // binary) and joins them into the delimited block appended to the
    // message's content when building the API request. Read once at send
    // time and persisted on the ChatMessage, rather than re-read from disk
    // on every subsequent turn.
    QString buildAttachmentsContext(const QVector<PendingAttachment> &attachments) const;
    // Base64-encodes image attachments for Ollama's per-message "images"
    // field (vision-capable models).
    QStringList buildImagesBase64(const QVector<PendingAttachment> &attachments) const;

    // Second half of sending a message: persists it, renders its bubble,
    // builds the API request from full history (including every message's
    // attachmentsContext/webSearchContext), and starts streaming.
    void finalizeAndSendUserMessage(ChatMessage userMessage, bool wasNewConversation);
    // Builds the API request from the conversation's *current* stored
    // history and starts streaming a reply — the tail end of
    // finalizeAndSendUserMessage(), factored out so editMessage()/
    // retryMessage() can reuse it after truncating history instead of
    // appending a new message. Reserves a fresh bubble and StreamState,
    // then hands off to sendTurnRequest() — the part that's also reused,
    // without a fresh bubble, by continueTurnAfterToolResults().
    void streamAssistantReplyForCurrentHistory();
    // Builds one /api/chat request from conversationId's *current* stored
    // history (re-read fresh every call, not cached — this is what picks up
    // tool-call/tool messages appended between rounds) and enqueues it via
    // ChatQueue. Shared by streamAssistantReplyForCurrentHistory() (the
    // first round of a turn) and continueTurnAfterToolResults() (every
    // round after a tool call) — neither owns any bubble/StreamState setup
    // itself, that's the caller's job.
    void sendTurnRequest(const QString &conversationId);
    // Cuts the *currently displayed* conversation's in-flight reply short,
    // if any (same cleanup as onStopClicked(), used here so editing/
    // retrying an earlier message can't race with a stream still writing
    // into a later bubble that's about to be removed). Never touches a
    // different conversation's background stream.
    void abortActiveStreamIfAny();

    // Assembles the /api/chat "tools" array from whichever built-in tools
    // are currently enabled (see m_webSearchEnabled/m_calculatorEnabled/
    // m_dateTimeEnabled and BuiltinTools.h) — empty if none are, which
    // sendTurnRequest() then omits from the request entirely rather than
    // sending an empty array.
    QJsonArray buildToolDefinitions() const;
    // The model decided to call one or more tools instead of (or before)
    // answering — see OllamaClient::chatToolCalls(), consumed here from
    // onChatDone(). Persists the assistant's own tool-call request message,
    // shows a "Calling <tool>…" ToolCallSectionWidget per call above the
    // (still-empty) answer text if this is the active conversation, and
    // hands the batch to m_toolExecutor. The turn is NOT finished at this
    // point — onAllToolCallsCompleted() is what continues it.
    void handleToolCalls(const QString &conversationId, const QJsonArray &toolCalls);
    // Resets the per-round streaming state (answer/thinking buffers, tok/s
    // timing) and calls sendTurnRequest() again for the same bubble — the
    // continuation of a turn after handleToolCalls()'s tool results are all
    // in. No-ops if the turn was stopped/errored while tools were still
    // executing (m_streams no longer has an entry for conversationId by
    // then), so a late-arriving tool result can never resurrect an
    // already-cancelled turn.
    void continueTurnAfterToolResults(const QString &conversationId);

    // Truncates the conversation to just before messageIndex, re-appends
    // that message with newText as its content (keeping its original
    // attachments — only the text changes) and a fresh timestamp, then
    // re-renders and regenerates the reply. Connected to each user bubble's
    // "Edit" button.
    void editMessage(int messageIndex, const QString &newText);
    // Truncates the conversation to just after messageIndex (dropping only
    // what followed it, not the message itself) and regenerates the reply —
    // same prompt, new answer. Connected to each user bubble's "Retry" button.
    void retryMessage(int messageIndex);

    // Auto-sends (if m_voiceAutoSend) once a live-mode recording's very
    // last chunk has come back — the live-transcription counterpart of
    // onWhisperTranscriptionFinished()'s own tail end. Doesn't otherwise
    // touch the input box (each chunk already appended its own text as it
    // arrived, via onWhisperLiveChunkTranscribed()), and doesn't disable/
    // re-enable input either, since live mode never disables it in the
    // first place — see onVoicePressed()'s own comment.
    void finishLiveVoiceRecording();

    void updateToolsButtonAppearance();
    // Re-derives the web-search/thinking/microphone SVG icons for the
    // current theme (light/dark) and toggle/recording state — called at
    // construction, on ThemeManager::themeChanged, and whenever a toggle or
    // the recording state changes, since each icon's color reflects its own
    // on/off state (see Theme::loadThemedIcon).
    void reloadThemedIcons();
    // Re-renders the send button's paper-plane icon for the current theme
    // (a no-op when m_sendButtonStyle isn't "plane") — split out from
    // reloadThemedIcons() since it's driven by m_sendButtonStyle, not a
    // toggle, and called from setSendButtonStyle() too.
    void reloadSendButtonIcon();
    // Swaps the send button between its normal (plane/arrow/text, per
    // m_sendButtonStyle) appearance and a stop icon, and sets m_isGenerating
    // — called when a reply starts/finishes streaming (or is stopped),
    // independent of setInputEnabled(): the button itself stays enabled and
    // clickable throughout a reply so it can be used to abort it, unlike
    // the textarea, which is genuinely not usable mid-stream.
    void setSendButtonBusy(bool busy);

    OllamaClient *m_ollamaClient = nullptr;
    ConversationStore *m_store = nullptr;
    ThemeManager *m_themeManager = nullptr;
    // Serializes every conversation's turns against the one real Ollama
    // server — see ChatQueue's own header comment. Owned here (parented to
    // this widget), constructed once in the constructor.
    ChatQueue *m_chatQueue = nullptr;
    bool m_isGenerating = false;

    // Per-conversation state for an in-flight assistant reply — keyed by
    // conversationId (not just tracked for whichever one is on screen), so a
    // reply keeps streaming into ConversationStore even while a *different*
    // conversation is being viewed. Entry is created when a stream starts
    // (streamAssistantReplyForCurrentHistory()) and removed when it finishes
    // or errors (onChatDone()/onChatError()) or is stopped
    // (abortActiveStreamIfAny()) — so `m_streams.contains(id)` alone answers
    // "is conversation id currently generating a reply?".
    struct StreamState
    {
        QString buffer;          // accumulated answer text (not thinking) streamed so far
        QString thinkingBuffer;  // accumulated thinking-trace text streamed so far
        bool isThinkingActive = false; // true between the first thinking token and the first content token
        qint64 startMs = 0;      // wall-clock time of the first content token, for tok/s
        int tokenCount = 0;
        double lastTokensPerSecond = 0.0;
        // Context tokens used as of just before this turn started (i.e.
        // whatever m_usedTokensByConversation already held) — Ollama only
        // reports the real prompt_eval_count/eval_count once the turn is
        // fully done, so onChatDelta() uses baselineUsedTokens + tokenCount
        // as a live running estimate in the meantime (same "one content
        // delta ~= one token" approximation tok/s already uses), letting
        // the context-usage bar/label update live instead of sitting frozen
        // at the previous turn's number until this one finishes. Corrected
        // to the exact real number by onChatUsage() either way.
        int baselineUsedTokens = 0;
    };
    QHash<QString, StreamState> m_streams;

    // Throttles onChatDelta()'s UI work (see flushStreamRender()) — without
    // this, a fast/long reply calls setMarkdownWithHtmlBlocks() (a full
    // Markdown-to-rich-text re-parse of the *entire* accumulated reply so
    // far, from scratch, since Qt has no incremental-append API for it) on
    // every single token, which is O(current reply length) per call and
    // therefore O(reply length squared) in total by the time a long reply
    // finishes — that's what made the UI visibly bog down on long outputs.
    // Capping actual render work to a fixed ~20 Hz cadence instead bounds
    // the number of full re-renders by wall-clock time rather than token
    // count, independent of how fast or long the reply is.
    QTimer *m_streamRenderTimer = nullptr;
    bool m_streamRenderDirty = false;

    QString m_activeConversationId;
    // Live widgets for m_activeConversationId's own entry in m_streams, if
    // it has one — null whenever the displayed conversation isn't currently
    // generating a reply, and also null (but the StreamState itself still
    // exists in m_streams) for any *other* conversation generating one in
    // the background, since its bubble isn't on screen right now. Rebuilt by
    // reconnectStreamingBubble() when switching into a conversation that has
    // a live background stream.
    AutoHeightTextBrowser *m_streamingBrowser = nullptr; // bubble content widget being streamed into, if any
    QVBoxLayout *m_streamingBubbleLayout = nullptr;       // that bubble's own layout, for inserting the thinking section
    ThinkingSectionWidget *m_streamingThinkingWidget = nullptr; // created lazily on the first thinking token, if any

    QComboBox *m_modelCombo = nullptr;
    // Per-conversation keep_alive choice — see Conversation::keepAliveSeconds
    // and onKeepAliveComboChanged(). Unlike m_modelCombo, always enabled:
    // keep_alive only affects the *next* turn's request, so it's never
    // "locked in" by existing messages the way the model itself is.
    QComboBox *m_keepAliveCombo = nullptr;

    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_messagesContainer = nullptr;
    QVBoxLayout *m_messagesLayout = nullptr;

    // Shown instead of m_scrollArea when there's no active conversation: a
    // centered title/subtitle with the *same* m_inputBar widget docked into
    // it (see dockInputBar()) rather than a duplicate input, so there's only
    // ever one textarea/model-combo/send-button to keep in sync.
    QWidget *m_emptyStatePanel = nullptr;
    // Horizontal, with a stretch on either side of the dock slot, so the
    // capped-width card (see dockInputBar()) sits centered rather than
    // stretching edge-to-edge like it does at the bottom of an active chat.
    QHBoxLayout *m_emptyStateInputDockLayout = nullptr;

    // Where m_inputBar lives for an active conversation (docked at the
    // bottom, below the context usage strip) — the other endpoint of the
    // same docking swap.
    QVBoxLayout *m_bottomInputDockLayout = nullptr;

    // Context-window usage strip, sitting between the message list and the
    // input bar. Per-conversation, so switching chats always shows *that*
    // chat's own usage rather than whatever the previously-active one left behind.
    QWidget *m_contextUsageBar = nullptr;
    QProgressBar *m_contextUsageProgress = nullptr;
    QLabel *m_contextUsageLabel = nullptr;
    QToolButton *m_jumpToButton = nullptr; // opens a menu of past questions to scroll straight to (see onJumpToClicked)

    // In display order, rebuilt every time the message list is (re)rendered
    // — see appendMessageBubble()/clearMessages(). Used to populate the
    // "Jump to" menu without re-deriving anything from ConversationStore.
    QVector<UserMessageMarker> m_userMessageMarkers;

    QWidget *m_inputBar = nullptr; // outer margin wrapper around m_inputCard; the unit that gets docked/undocked
    QFrame *m_inputCard = nullptr; // the rounded card wrapping the textarea + toolbar
    ChatInputEdit *m_inputEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QToolButton *m_attachButton = nullptr;
    QString m_sendButtonStyle = "plane"; // "plane" | "arrow" | "text" — see setSendButtonStyle()
    bool m_sendButtonFilled = false; // false = flat (default), true = classic accent pill — see setSendButtonFilled()
    bool m_voiceAutoSend = false; // false = fill box for review (default), true = send immediately — see setVoiceAutoSend()

    // "Tools" dropdown (right of the attach button): checkable toggles for
    // each built-in tool-calling tool (see BuiltinTools.h) plus Thinking.
    // Persistent QMenu (not rebuilt per click, unlike the "Jump to" menu)
    // since its checked state has to survive being closed. Enabling one of
    // the tool toggles doesn't search/calculate/etc. eagerly — it just adds
    // that tool to the /api/chat "tools" array (see buildToolDefinitions()),
    // leaving the model itself to decide whether to actually call it.
    QToolButton *m_toolsButton = nullptr;
    QAction *m_webSearchAction = nullptr;
    QAction *m_calculatorAction = nullptr;
    QAction *m_dateTimeAction = nullptr;
    QAction *m_stackOverflowAction = nullptr;
    QAction *m_thinkingAction = nullptr;
    bool m_webSearchEnabled = false; // off by default, per spec
    bool m_calculatorEnabled = false;
    bool m_dateTimeEnabled = false;
    bool m_stackOverflowEnabled = false;
    bool m_thinkingEnabled = true;   // matches this app's prior always-on behavior

    // Executes whatever built-in tools the model actually decided to call
    // (see handleToolCalls()/onAllToolCallsCompleted()) — owned here,
    // constructed once in the constructor.
    ToolExecutor *m_toolExecutor = nullptr;
    // Live ToolCallSectionWidget(s) for the *current* round of the active
    // conversation's in-flight turn, indexed exactly like ToolExecutor's own
    // callIndex — see handleToolCalls() (populates this) and
    // onToolCallCompleted() (updates the matching entry once its result is
    // in). Cleared at the start of every round, including the very first
    // (non-tool) one, so a stale index from an earlier round/turn can never
    // be misapplied to the wrong widget.
    QVector<ToolCallSectionWidget *> m_streamingToolCallWidgets;
    // How many tool-call rounds the active turn has already gone through,
    // per conversation — see kMaxToolCallRounds in ChatWidget.cpp.
    // Guards against a model that keeps calling tools without ever
    // actually answering: once the cap is hit, sendTurnRequest() omits the
    // "tools" field entirely for that round, forcing a real answer instead
    // of another call. Reset (removed) whenever a turn starts or finishes
    // normally.
    QHash<QString, int> m_toolRoundCountByConversation;
    // Set by onChatToolCalls(), consumed (take()'d) by onChatDone() right
    // after — see that slot's own comment for why it's split this way.
    QHash<QString, QJsonArray> m_pendingToolCallsByConversation;

    // Push-to-talk mic button, between the model combo and Send. Recording
    // is VoiceRecorder's job; turning the resulting .wav into text is
    // WhisperManager's (shared, not owned here — see main.cpp).
    QToolButton *m_voiceButton = nullptr;
    VoiceRecorder m_voiceRecorder;
    WhisperManager *m_whisperManager = nullptr;
    // The .wav VoiceRecorder just finished writing, kept around only until
    // onWhisperTranscriptionFinished() fires (transcription is async), so it
    // can be deleted once WhisperManager is done with it either way.
    QString m_pendingVoiceFilePath;

    // Row of removable chips for files queued via m_attachButton, shown
    // above the input box; hidden entirely while empty.
    QWidget *m_attachmentsBar = nullptr;
    QHBoxLayout *m_attachmentsLayout = nullptr;
    QVector<PendingAttachment> m_pendingAttachments;

    // Tokens used so far, keyed by conversation id. Populated from Ollama's
    // prompt_eval_count + eval_count after each completed reply. Since every
    // /api/chat call resends the full message history, this is effectively
    // "current context usage" for that conversation, not just the last turn.
    QHash<QString, int> m_usedTokensByConversation;

    // Context-window size per model name, fetched lazily via /api/show and
    // cached — this is a property of the model, not of any one conversation,
    // so it only needs fetching once per model per app session.
    QHash<QString, int> m_contextLengthByModel;

    // Family/parameter-size/quantization per model name, fetched alongside
    // m_contextLengthByModel off the very same /api/show call (see
    // OllamaClient::fetchModelContextLength()) and cached the same way.
    QHash<QString, ModelMetadata> m_modelMetadataByModel;

    // Last known generation speed per conversation, kept separately from
    // m_streams (which is erased once a turn finishes/errors) so the speed
    // of the most recent reply still shows in the context-usage bar rather
    // than reverting to blank the moment that turn completes.
    QHash<QString, double> m_lastTokensPerSecondByConversation;
};
