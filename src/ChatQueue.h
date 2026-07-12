#pragma once

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QVector>

#include "OllamaClient.h" // for GenerationOptions

class OllamaClient;

// Serializes /api/chat turns across every conversation so only one is ever
// actually in flight against the real Ollama server at a time. OllamaClient
// itself is capable of true concurrent streams (see its own header), but on
// typical single-GPU setups Ollama can only usefully generate for one model
// at once anyway — so ChatWidget submits every turn (new message, retry,
// edit-resend) here instead of calling OllamaClient::sendChatMessage()
// directly, and this queues whatever can't start immediately.
//
// Also owns explicit model-swap bookkeeping: Ollama loads a model
// implicitly on first use of it, but doesn't reliably unload a *previous*
// one on its own within a single VRAM budget — so when the next turn to run
// uses a different model than the last one this queue sent, the old model
// is explicitly unloaded (via OllamaClient::unloadModel()) and the new
// turn's request only goes out once that's acknowledged.
//
// By default, turns run strictly in the order they were submitted (FIFO),
// even if that means swapping models back and forth. With
// setOptimizeModelSwaps(true), the next turn to run is instead whichever
// queued turn matches the currently-loaded model, if any is waiting —
// falling back to FIFO otherwise — trading strict ordering for fewer
// load/unload round trips (see SettingsDialog's "Queing Model 
// optimization").
class ChatQueue : public QObject
{
    Q_OBJECT

public:
    struct Turn
    {
        QString conversationId;
        QString model;
        QJsonArray apiMessages;
        bool think = true;
        int customNumCtx = 0;
        GenerationOptions genOptions;
        int keepAliveSeconds = kKeepAliveUseServerDefault;
    };

    explicit ChatQueue(OllamaClient *ollamaClient, QObject *parent = nullptr);

    // Adds a turn to the queue and starts it immediately if nothing else is
    // currently running or waiting on an unload.
    void enqueue(const Turn &turn);

    // Removes conversationId's turn if it hasn't started yet, or aborts it
    // if it's the one currently running/waiting on an unload — either way,
    // the next queued turn (if any) is started right after. Safe to call
    // for a conversationId with nothing queued or running (no-op).
    void cancel(const QString &conversationId);

    bool isQueued(const QString &conversationId) const;
    bool isRunning(const QString &conversationId) const;

    // "N turns ahead of you" for a still-queued conversationId — 0 if it's
    // not queued (either running already, or not submitted at all).
    int aheadCount(const QString &conversationId) const;

    void setOptimizeModelSwaps(bool enabled);

signals:
    // A turn actually started sending to Ollama (after any needed model
    // swap finished) — conversationId is now the "running" one.
    void turnStarted(const QString &conversationId);
    // Emitted for every still-queued turn whenever the queue changes shape
    // (something enqueued, something finished, something cancelled), so the
    // UI can show "N chats ahead" and keep it current.
    void queuePositionChanged(const QString &conversationId, int aheadCount);

private slots:
    void onStreamEnded(const QString &conversationId);
    void onStreamErrored(const QString &conversationId, const QString &message);
    void onModelUnloaded(const QString &model, bool success);

private:
    void startNext();
    int pickNextIndex() const; // FIFO, or model-matching first if m_optimize
    void broadcastQueuePositions();

    OllamaClient *m_ollamaClient = nullptr;
    QVector<Turn> m_pending;

    QString m_runningConversationId; // empty if nothing is currently sending
    QString m_loadedModel;           // best-known currently-loaded model, empty if unknown/none

    bool m_waitingForUnload = false;
    QString m_pendingUnloadModel;
    Turn m_turnWaitingForUnload;

    bool m_optimize = false;
};
