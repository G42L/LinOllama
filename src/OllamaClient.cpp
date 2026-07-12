#include "OllamaClient.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

OllamaClient::OllamaClient(QObject *parent) : QObject(parent) {}

void OllamaClient::setBaseUrl(const QUrl &url)
{
    m_baseUrl = url;
}

void OllamaClient::refreshStatus()
{
    QUrl url = m_baseUrl;
    url.setPath("/api/tags");

    QNetworkReply *reply = m_manager.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit reachable(false);
            return;
        }

        emit reachable(true);

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QStringList names;
        for (const QJsonValue &v : doc.object().value("models").toArray()) {
            names << v.toObject().value("name").toString();
        }
        emit modelsListed(names);
    });
}

void OllamaClient::fetchServerVersion()
{
    QUrl url = m_baseUrl;
    url.setPath("/api/version");

    QNetworkReply *reply = m_manager.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit serverVersionFetched(QString());
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        emit serverVersionFetched(doc.object().value("version").toString());
    });
}

struct OllamaClient::ChatStream
{
    QNetworkReply *reply = nullptr;
    QByteArray lineBuffer; // holds any partial (not yet newline-terminated) NDJSON line

    // Accumulate token counts across the NDJSON lines of this stream; Ollama
    // only sends these on the final (done:true) line, but we read them
    // defensively off whichever line has them.
    int pendingPromptEvalCount = 0;
    int pendingEvalCount = 0;
};

void OllamaClient::sendChatMessage(const QString &conversationId, const QString &model,
                                    const QJsonArray &messages, bool think, int customNumCtx,
                                    const GenerationOptions &genOptions)
{
    abortChat(conversationId); // replace this conversation's own previous stream, if any — others are untouched

    QUrl url = m_baseUrl;
    url.setPath("/api/chat");

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["model"] = model;
    body["messages"] = messages;
    body["stream"] = true;
    // Harmless no-op on models without thinking support when true (per
    // Ollama's docs); on models that do support it, this is what makes
    // message.thinking show up in the stream at all.
    body["think"] = think;

    QJsonObject options;
    if (customNumCtx > 0)
        options["num_ctx"] = customNumCtx;
    if (genOptions.enabled) {
        options["temperature"] = genOptions.temperature;
        options["top_p"] = genOptions.topP;
        options["top_k"] = genOptions.topK;
        options["seed"] = genOptions.seed;
        options["num_predict"] = genOptions.numPredict;
        options["repeat_penalty"] = genOptions.repeatPenalty;
        if (!genOptions.stop.isEmpty())
            options["stop"] = QJsonArray::fromStringList(genOptions.stop);
    }
    if (!options.isEmpty())
        body["options"] = options;

    auto *stream = new ChatStream;
    stream->reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_chatStreams[conversationId] = stream;

    // Both lambdas capture `stream` directly (not a hash lookup by
    // conversationId) so they keep working correctly even if a later
    // sendChatMessage() for the same conversationId replaces the hash entry
    // — each stream's own reply only ever talks to its own ChatStream.
    connect(stream->reply, &QNetworkReply::readyRead, this, [this, conversationId, stream]() {
        // Ollama streams one JSON object per line (NDJSON). A readyRead
        // firing doesn't guarantee a whole line arrived, so we buffer any
        // trailing partial line and only parse complete ones.
        stream->lineBuffer += stream->reply->readAll();

        int newlineIdx;
        while ((newlineIdx = stream->lineBuffer.indexOf('\n')) != -1) {
            const QByteArray line = stream->lineBuffer.left(newlineIdx).trimmed();
            stream->lineBuffer.remove(0, newlineIdx + 1);
            if (line.isEmpty())
                continue;

            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject())
                continue;

            const QJsonObject obj = doc.object();
            if (obj.contains("error")) {
                emit chatError(conversationId, obj.value("error").toString());
                continue;
            }

            // Only present on the final (done:true) line in practice, but
            // read them off whichever line has them rather than assuming.
            if (obj.contains("prompt_eval_count"))
                stream->pendingPromptEvalCount = obj.value("prompt_eval_count").toInt();
            if (obj.contains("eval_count"))
                stream->pendingEvalCount = obj.value("eval_count").toInt();

            const QJsonObject messageObj = obj.value("message").toObject();

            // Thinking-capable models stream message.thinking first, then
            // switch to message.content once the actual answer starts — the
            // two are mutually exclusive per line in practice, but we don't
            // assume that and just emit whichever is non-empty.
            const QString thinkingToken = messageObj.value("thinking").toString();
            if (!thinkingToken.isEmpty())
                emit chatThinkingDelta(conversationId, thinkingToken);

            const QString token = messageObj.value("content").toString();
            if (!token.isEmpty())
                emit chatDelta(conversationId, token);

            if (obj.value("done").toBool(false)) {
                // Emitted before chatDone() deliberately — ChatWidget's
                // onChatUsage() refines its live tok/s estimate using the
                // still-present StreamState, which onChatDone() (right
                // after) then removes.
                emit chatUsage(conversationId, stream->pendingPromptEvalCount, stream->pendingEvalCount);
                emit chatDone(conversationId);
            }
        }
    });

    connect(stream->reply, &QNetworkReply::finished, this, [this, conversationId, stream]() {
        QNetworkReply *reply = stream->reply;
        if (reply->error() != QNetworkReply::NoError
            && reply->error() != QNetworkReply::OperationCanceledError) {
            emit chatError(conversationId, reply->errorString());
        }
        reply->deleteLater();

        // Only drop the hash entry if it's still pointing at *this* stream —
        // a newer sendChatMessage() for the same conversationId may already
        // have replaced it (via the abortChat() at the top of this
        // function), in which case that newer entry must be left alone.
        if (m_chatStreams.value(conversationId) == stream)
            m_chatStreams.remove(conversationId);
        delete stream;
    });
}

void OllamaClient::abortChat(const QString &conversationId)
{
    ChatStream *stream = m_chatStreams.value(conversationId);
    if (!stream)
        return;

    // Removed here so a stream started for the same conversationId right
    // after this call isn't confused with the one being aborted — the
    // finished() lambda above still owns cleanup (reply->deleteLater() +
    // delete stream) once abort() makes it fire.
    m_chatStreams.remove(conversationId);
    stream->reply->abort();
}

void OllamaClient::fetchModelContextLength(const QString &model)
{
    QUrl url = m_baseUrl;
    url.setPath("/api/show");

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body{{"model", model}};
    QNetworkReply *reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit modelContextLengthFetched(model, 0);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject obj = doc.object();

        // model_info keys are architecture-prefixed, e.g. "llama.context_length"
        // or "qwen2.context_length" — there's no fixed key name, so scan for
        // whichever one this model exposes.
        int contextLength = 0;
        const QJsonObject modelInfo = obj.value("model_info").toObject();
        for (auto it = modelInfo.constBegin(); it != modelInfo.constEnd(); ++it) {
            if (it.key().endsWith(".context_length")) {
                contextLength = it.value().toInt();
                break;
            }
        }

        // Fallback: older Ollama versions (or models without that model_info
        // key) still often surface it as a Modelfile "num_ctx" parameter.
        if (contextLength == 0) {
            const QString parameters = obj.value("parameters").toString();
            static const QRegularExpression numCtxPattern("num_ctx\\s+(\\d+)");
            const QRegularExpressionMatch match = numCtxPattern.match(parameters);
            if (match.hasMatch())
                contextLength = match.captured(1).toInt();
        }

        emit modelContextLengthFetched(model, contextLength);
    });
}

void OllamaClient::fetchLoadedModels()
{
    QUrl url = m_baseUrl;
    url.setPath("/api/ps");

    QNetworkReply *reply = m_manager.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit loadedModelsListed({});
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QVector<LoadedModelInfo> models;
        for (const QJsonValue &v : doc.object().value("models").toArray()) {
            const QJsonObject obj = v.toObject();
            LoadedModelInfo info;
            info.name = obj.value("name").toString();
            info.sizeVramBytes = static_cast<quint64>(obj.value("size_vram").toDouble());
            models.append(info);
        }
        emit loadedModelsListed(models);
    });
}

void OllamaClient::unloadModel(const QString &model)
{
    QUrl url = m_baseUrl;
    url.setPath("/api/chat");

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // The documented way to force an immediate unload: a chat request with
    // no messages and keep_alive: 0. There's no dedicated "/api/unload"
    // endpoint, so this is the mechanism Ollama itself expects for it.
    QJsonObject body;
    body["model"] = model;
    body["messages"] = QJsonArray{};
    body["keep_alive"] = 0;
    body["stream"] = false;

    QNetworkReply *reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        emit modelUnloaded(model, ok);
    });
}

struct OllamaClient::PullStream
{
    QNetworkReply *reply = nullptr;
    QByteArray lineBuffer;
};

void OllamaClient::pullModel(const QString &model)
{
    cancelPull(model); // replace this model's own previous pull, if any — others are untouched

    QUrl url = m_baseUrl;
    url.setPath("/api/pull");

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body{{"model", model}, {"stream", true}};

    auto *stream = new PullStream;
    stream->reply = m_manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_pullStreams[model] = stream;

    // Both lambdas capture `stream` directly, not a hash lookup by model —
    // same reasoning as sendChatMessage()'s ChatStream lambdas: keeps
    // working correctly even if a later pullModel() for the same model
    // reference replaces the hash entry first.
    connect(stream->reply, &QNetworkReply::readyRead, this, [this, model, stream]() {
        stream->lineBuffer += stream->reply->readAll();

        int newlineIdx;
        while ((newlineIdx = stream->lineBuffer.indexOf('\n')) != -1) {
            const QByteArray line = stream->lineBuffer.left(newlineIdx).trimmed();
            stream->lineBuffer.remove(0, newlineIdx + 1);
            if (line.isEmpty())
                continue;

            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject())
                continue;
            const QJsonObject obj = doc.object();

            if (obj.contains("error")) {
                emit modelPullFinished(model, false, obj.value("error").toString());
                continue;
            }

            emit modelPullProgress(model, obj.value("status").toString(),
                                    static_cast<qint64>(obj.value("completed").toDouble()),
                                    static_cast<qint64>(obj.value("total").toDouble()));
        }
    });

    connect(stream->reply, &QNetworkReply::finished, this, [this, model, stream]() {
        QNetworkReply *reply = stream->reply;
        const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
        if (reply->error() != QNetworkReply::NoError && !cancelled)
            emit modelPullFinished(model, false, reply->errorString());
        else if (!cancelled)
            emit modelPullFinished(model, true, QString());
        reply->deleteLater();

        // Same guard as sendChatMessage()'s ChatStream cleanup — only drop
        // the hash entry if it's still pointing at *this* stream.
        if (m_pullStreams.value(model) == stream)
            m_pullStreams.remove(model);
        delete stream;
    });
}

void OllamaClient::cancelPull(const QString &model)
{
    PullStream *stream = m_pullStreams.value(model);
    if (!stream)
        return;
    m_pullStreams.remove(model); // see abortChat()'s identical reasoning
    stream->reply->abort();
}

void OllamaClient::deleteModel(const QString &model)
{
    QUrl url = m_baseUrl;
    url.setPath("/api/delete");

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // QNetworkAccessManager has no deleteResource() overload that accepts a
    // request body, and /api/delete requires one (the model name) — sendCustomRequest()
    // is the general escape hatch for exactly this kind of "verb Qt doesn't
    // have a dedicated method for" case.
    QJsonObject body{{"model", model}};
    QNetworkReply *reply = m_manager.sendCustomRequest(
        request, "DELETE", QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(reply, &QNetworkReply::finished, this, [this, reply, model]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        emit modelDeleted(model, ok, ok ? QString() : reply->errorString());
    });
}
