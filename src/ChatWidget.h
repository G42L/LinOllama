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
#include "WebSearchClient.h"
#include "VoiceRecorder.h"
#include "WhisperManager.h"
#include "MapEmbedWidget.h"
#include "HtmlEmbedWidget.h"

class QVBoxLayout;
class QHBoxLayout;
class QFrame;
class QMenu;
class QAction;
class ThinkingSectionWidget;
class ThemeManager;

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
    void onChatDelta(const QString &conversationId, const QString &tokenText);
    void onChatThinkingDelta(const QString &conversationId, const QString &tokenText);
    void onChatDone(const QString &conversationId);
    void onChatError(const QString &conversationId, const QString &message);
    void onChatUsage(const QString &conversationId, int promptTokens, int completionTokens);
    void onModelContextLengthFetched(const QString &model, int contextLength);
    void onModelComboChanged(int index);

    // ChatQueue's own progress signals — see setModelOptimizationEnabled()
    // and streamAssistantReplyForCurrentHistory() for how turns get there.
    void onQueueTurnStarted(const QString &conversationId);
    void onQueuePositionChanged(const QString &conversationId, int aheadCount);

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
    // attachmentsContext/webSearchContext), and starts streaming. Split out
    // from onSendClicked() because when the "Search the web" tool is on,
    // there's an async search round-trip to wait on first — see
    // onSendClicked()/onWebSearchToggled().
    void finalizeAndSendUserMessage(ChatMessage userMessage, bool wasNewConversation);
    // Builds the API request from the conversation's *current* stored
    // history and starts streaming a reply — the tail end of
    // finalizeAndSendUserMessage(), factored out so editMessage()/
    // retryMessage() can reuse it after truncating history instead of
    // appending a new message.
    void streamAssistantReplyForCurrentHistory();
    // Cuts the *currently displayed* conversation's in-flight reply short,
    // if any (same cleanup as onStopClicked(), used here so editing/
    // retrying an earlier message can't race with a stream still writing
    // into a later bubble that's about to be removed). Never touches a
    // different conversation's background stream.
    void abortActiveStreamIfAny();

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
    };
    QHash<QString, StreamState> m_streams;

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

    // "Tools" dropdown (right of the attach button): checkable Search-the-web
    // and Thinking toggles. Persistent QMenu (not rebuilt per click, unlike
    // the "Jump to" menu) since its checked state has to survive being closed.
    QToolButton *m_toolsButton = nullptr;
    QAction *m_webSearchAction = nullptr;
    QAction *m_thinkingAction = nullptr;
    bool m_webSearchEnabled = false; // off by default, per spec
    bool m_thinkingEnabled = true;   // matches this app's prior always-on behavior

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

    // Holds the message being sent while an async web search is in flight
    // (see onSendClicked()/finalizeAndSendUserMessage()) — unused, empty
    // when web search is off, since that path finalizes synchronously.
    ChatMessage m_pendingUserMessage;
    bool m_pendingWasNewConversation = false;
    WebSearchClient m_webSearchClient;

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

    // Last known generation speed per conversation, kept separately from
    // m_streams (which is erased once a turn finishes/errors) so the speed
    // of the most recent reply still shows in the context-usage bar rather
    // than reverting to blank the moment that turn completes.
    QHash<QString, double> m_lastTokensPerSecondByConversation;
};
