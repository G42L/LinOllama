#include "ChatQueue.h"
#include "OllamaClient.h"

ChatQueue::ChatQueue(OllamaClient *ollamaClient, QObject *parent)
    : QObject(parent)
    , m_ollamaClient(ollamaClient)
{
    connect(m_ollamaClient, &OllamaClient::chatDone, this, &ChatQueue::onStreamEnded);
    connect(m_ollamaClient, &OllamaClient::chatError, this, &ChatQueue::onStreamErrored);
    connect(m_ollamaClient, &OllamaClient::modelUnloaded, this, &ChatQueue::onModelUnloaded);
}

void ChatQueue::enqueue(const Turn &turn)
{
    m_pending.append(turn);
    broadcastQueuePositions();

    if (m_runningConversationId.isEmpty() && !m_waitingForUnload)
        startNext();
}

void ChatQueue::cancel(const QString &conversationId)
{
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].conversationId == conversationId) {
            m_pending.removeAt(i);
            broadcastQueuePositions();
            return;
        }
    }

    if (m_waitingForUnload && m_turnWaitingForUnload.conversationId == conversationId) {
        m_waitingForUnload = false;
        startNext();
        return;
    }

    if (m_runningConversationId == conversationId) {
        // OllamaClient::abortChat() emits neither chatDone() nor chatError()
        // (see its own header comment), so onStreamEnded()/onStreamErrored()
        // aren't coming for this one — the queue has to notice the slot is
        // free itself rather than waiting on a signal that isn't coming.
        m_ollamaClient->abortChat(conversationId);
        m_runningConversationId.clear();
        startNext();
    }
}

bool ChatQueue::isQueued(const QString &conversationId) const
{
    for (const Turn &t : m_pending) {
        if (t.conversationId == conversationId)
            return true;
    }
    return m_waitingForUnload && m_turnWaitingForUnload.conversationId == conversationId;
}

bool ChatQueue::isRunning(const QString &conversationId) const
{
    return m_runningConversationId == conversationId;
}

int ChatQueue::aheadCount(const QString &conversationId) const
{
    if (m_waitingForUnload && m_turnWaitingForUnload.conversationId == conversationId)
        return m_runningConversationId.isEmpty() ? 0 : 1;

    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].conversationId == conversationId)
            return i + (m_runningConversationId.isEmpty() ? 0 : 1);
    }
    return 0;
}

void ChatQueue::setOptimizeModelSwaps(bool enabled)
{
    m_optimize = enabled;
}

void ChatQueue::onStreamEnded(const QString &conversationId)
{
    if (conversationId != m_runningConversationId)
        return; // not the turn this queue is tracking as "running" — ignore
    m_runningConversationId.clear();
    startNext();
}

void ChatQueue::onStreamErrored(const QString &conversationId, const QString &message)
{
    Q_UNUSED(message);
    onStreamEnded(conversationId); // same "the slot is free again" bookkeeping either way
}

void ChatQueue::onModelUnloaded(const QString &model, bool success)
{
    Q_UNUSED(success);
    // OllamaClient's modelUnloaded is shared with SettingsDialog's own
    // manual "Offload model" button and TrayApplication's tray menu — only
    // react to it here if it's specifically the swap this queue asked for.
    if (!m_waitingForUnload || model != m_pendingUnloadModel)
        return;

    m_waitingForUnload = false;
    const Turn turn = m_turnWaitingForUnload;

    m_runningConversationId = turn.conversationId;
    m_loadedModel = turn.model;
    emit turnStarted(turn.conversationId);
    m_ollamaClient->sendChatMessage(turn.conversationId, turn.model, turn.apiMessages, turn.think, turn.customNumCtx, turn.genOptions, turn.keepAliveSeconds);
    broadcastQueuePositions();
}

int ChatQueue::pickNextIndex() const
{
    if (!m_optimize || m_pending.isEmpty() || m_loadedModel.isEmpty())
        return 0; // FIFO — whatever was queued first, regardless of model

    // Prefer a queued turn matching whichever model is currently loaded, so
    // a swap is never forced while one's already waiting — falls back to
    // FIFO (which will need a swap) once none do.
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending[i].model == m_loadedModel)
            return i;
    }
    return 0;
}

void ChatQueue::startNext()
{
    if (m_pending.isEmpty())
        return;

    const int idx = pickNextIndex();
    const Turn turn = m_pending.takeAt(idx);
    broadcastQueuePositions();

    if (!m_loadedModel.isEmpty() && m_loadedModel != turn.model) {
        // Explicit swap: unload the previous model first, and only send the
        // new turn's request once that's acknowledged (see onModelUnloaded())
        // — avoids a moment where Ollama has to hold both models in VRAM at
        // once on a machine that can't afford that.
        m_waitingForUnload = true;
        m_pendingUnloadModel = m_loadedModel;
        m_turnWaitingForUnload = turn;
        m_ollamaClient->unloadModel(m_loadedModel);
        return;
    }

    m_runningConversationId = turn.conversationId;
    m_loadedModel = turn.model;
    emit turnStarted(turn.conversationId);
    m_ollamaClient->sendChatMessage(turn.conversationId, turn.model, turn.apiMessages, turn.think, turn.customNumCtx, turn.genOptions, turn.keepAliveSeconds);
}

void ChatQueue::broadcastQueuePositions()
{
    int position = m_runningConversationId.isEmpty() ? 0 : 1;
    for (const Turn &t : m_pending) {
        emit queuePositionChanged(t.conversationId, position);
        ++position;
    }
}
