#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <QJsonArray>
#include <QVector>
#include <QHash>

// A model currently loaded in Ollama's memory (from GET /api/ps), used to
// populate the "Offload model" list in the tray menu and Settings dialog.
struct LoadedModelInfo
{
    QString name;
    quint64 sizeVramBytes = 0;
};

// Thin wrapper around Ollama's local REST API.
class OllamaClient : public QObject
{
    Q_OBJECT

public:
    explicit OllamaClient(QObject *parent = nullptr);

    // Default assumes a local server on the standard port; override via
    // Settings once that dialog exists (maps to OLLAMA_HOST on the server side).
    void setBaseUrl(const QUrl &url);

    // Hits GET /api/tags. Emits reachable(true) + modelsListed(...) on success,
    // reachable(false) on any network error (including "connection refused",
    // which is the common case of the server just not being started yet).
    void refreshStatus();

    // Streams POST /api/chat with "stream": true. `messages` is a JSON
    // array of {"role", "content"} objects, oldest first — the caller
    // builds this from Conversation::messages. Emits chatThinkingDelta() for
    // reasoning-model "thinking" tokens (see docs.ollama.com/capabilities/
    // thinking — a separate message.thinking field, distinct from the actual
    // reply) when think is true, chatDelta() for regular content tokens,
    // then chatDone() (and chatUsage(), see below) when the server reports
    // done:true. Requesting "think": true on a model that doesn't support it
    // is a documented no-op, not an error, so the caller doesn't need a
    // per-model check — just its own on/off preference (see ChatWidget's
    // "Thinking" tool toggle). Streams are independent per conversationId —
    // calling this again for the SAME conversationId aborts that
    // conversation's own previous stream first, but a different
    // conversationId runs concurrently alongside it (this is what lets a
    // background chat keep generating while another conversation is shown).
    //
    // customNumCtx, if > 0, is sent as options.num_ctx — Ollama's own
    // context-window-size request parameter (see SettingsDialog's "Context
    // length" section). 0 (the default) omits the field entirely rather
    // than sending a literal 0, since Ollama does *not* treat num_ctx: 0 as
    // "unlimited" — there's no such thing; every model has a hard trained
    // context ceiling, and 0 just means "let Ollama pick its own default."
    // Omitting the field reproduces exactly that same "let Ollama decide"
    // behavior, so this is a real choice, not a workaround.
    void sendChatMessage(const QString &conversationId, const QString &model, const QJsonArray &messages,
                          bool think = true, int customNumCtx = 0);

    // Aborts the in-flight chat stream for the given conversationId, if any.
    // Safe to call when none is active; doesn't touch any other
    // conversation's stream.
    void abortChat(const QString &conversationId);

    // Hits POST /api/show for the given model to discover its context-window
    // size, so the UI can show "X / Y tokens" rather than just "X tokens".
    // Result arrives via modelContextLengthFetched(); contextLength is 0
    // there if it couldn't be determined (older Ollama versions, or a model
    // whose metadata doesn't expose it) — callers should treat 0 as "unknown"
    // rather than "zero-size context".
    void fetchModelContextLength(const QString &model);

    // Hits GET /api/ps for the models currently loaded into memory. Result
    // arrives via loadedModelsListed(), with an empty list on any error —
    // callers can't distinguish "none loaded" from "couldn't ask", but both
    // cases render the same way (nothing to offload).
    void fetchLoadedModels();

    // Unloads a model immediately by sending an empty-message chat request
    // with "keep_alive": 0 — Ollama's documented way to force an early
    // unload rather than waiting for the model's normal idle timeout. This
    // is independent of any in-flight sendChatMessage() stream (a different
    // model, or even the same one from a past turn, can be offloaded without
    // touching the active conversation) and does not call abortChat().
    void unloadModel(const QString &model);

    // Streams POST /api/pull for the given model reference (e.g. "llama3.2"
    // or "llama3.2:3b" — any tag Ollama itself understands). Progress
    // arrives via modelPullProgress() as Ollama reports each layer's
    // download (its NDJSON stream reports one "status" line per layer, not
    // one combined total, so completed/total reset per layer — see that
    // signal's own doc), and modelPullFinished() once the stream ends
    // either way. Calling this again for a model reference already pulling
    // replaces that pull (same one-per-key semantics as sendChatMessage()),
    // so a second click just restarts it rather than running two at once.
    void pullModel(const QString &model);
    // Aborts an in-flight pull for the given model reference, if any. The
    // partially-downloaded layers are left on Ollama's side (it resumes
    // from them on a later pull of the same model, same as its own CLI) —
    // this only stops the client from waiting on it further.
    void cancelPull(const QString &model);

    // Hits DELETE /api/delete for the given model. Result arrives via
    // modelDeleted(); does not touch whether it's currently loaded/running
    // (Ollama refuses the delete in that case — see modelDeleted()'s error
    // string, surfaced as-is rather than special-cased).
    void deleteModel(const QString &model);

signals:
    void reachable(bool isReachable);
    void modelsListed(const QStringList &modelNames);

    void chatDelta(const QString &conversationId, const QString &tokenText);
    // Reasoning-trace tokens for thinking-capable models — arrives before
    // chatDelta() for the same turn, since Ollama streams the thinking
    // trace first and only then starts the actual answer.
    void chatThinkingDelta(const QString &conversationId, const QString &tokenText);
    void chatDone(const QString &conversationId);
    void chatError(const QString &conversationId, const QString &message);

    // Emitted right alongside chatDone() when Ollama reported token counts
    // for the exchange that just finished. promptTokens covers the whole
    // conversation history sent *this* turn (Ollama has no session/cache
    // concept from the client's point of view — every turn resends full
    // history), completionTokens is just the reply that was generated.
    // promptTokens + completionTokens is "current context usage in tokens"
    // for that conversation as of now.
    void chatUsage(const QString &conversationId, int promptTokens, int completionTokens);

    // One-shot result of fetchModelContextLength(). Emitted even on
    // failure/unknown, with contextLength == 0, so callers don't have to
    // guess whether the request is still pending.
    void modelContextLengthFetched(const QString &model, int contextLength);

    void loadedModelsListed(const QVector<LoadedModelInfo> &models);
    void modelUnloaded(const QString &model, bool success);

    // status is Ollama's own line verbatim (e.g. "pulling manifest",
    // "pulling 4f5b8c...", "verifying sha256 digest", "writing manifest") —
    // shown as-is rather than translated, since it already reads fine as
    // status text and Ollama doesn't document a fixed enum of values to
    // switch on. completed/total are only meaningful (both > 0) during a
    // "pulling <digest>" line, which reports progress for *that one layer*,
    // not a combined whole-model total — a multi-layer model's progress
    // bar will visibly reset a few times as each layer starts, which is
    // Ollama's own reporting granularity, not a bug here.
    void modelPullProgress(const QString &model, const QString &status, qint64 completed, qint64 total);
    void modelPullFinished(const QString &model, bool success, const QString &error);

    void modelDeleted(const QString &model, bool success, const QString &error);

private:
    QNetworkAccessManager m_manager;
    QUrl m_baseUrl{QStringLiteral("http://127.0.0.1:11434")};

    // One in-flight /api/chat stream. Defined in the .cpp (only ever
    // referenced through a pointer here) so the header doesn't need
    // QNetworkReply's full definition. Accumulates token counts across the
    // NDJSON lines of its own stream; Ollama only sends these on the final
    // (done:true) line, but they're read defensively off whichever line has
    // them.
    struct ChatStream;
    // Keyed by conversationId — this is what makes streams independent per
    // conversation, so switching which one is displayed doesn't have to
    // touch any of them (see sendChatMessage()/abortChat()).
    QHash<QString, ChatStream *> m_chatStreams;

    // One in-flight /api/pull stream — same NDJSON-over-POST shape as
    // ChatStream, but pull's progress lines carry byte counts instead of
    // token content, hence a separate (simpler) struct rather than reusing
    // ChatStream directly.
    struct PullStream;
    // Keyed by model reference, same one-per-key semantics as m_chatStreams.
    QHash<QString, PullStream *> m_pullStreams;
};
