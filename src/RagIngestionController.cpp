#include "RagIngestionController.h"
#include "RagStore.h"
#include "OllamaClient.h"
#include "DocumentTextExtractor.h"
#include "TextChunker.h"

#include <QFileInfo>
#include <QSettings>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

RagIngestionController::RagIngestionController(OllamaClient *ollamaClient, RagStore *ragStore, QObject *parent)
    : QObject(parent), m_ollamaClient(ollamaClient), m_ragStore(ragStore)
{
    connect(m_ollamaClient, &OllamaClient::embeddingsFetched,
            this, &RagIngestionController::onEmbeddingsFetched);
}

void RagIngestionController::ingestFiles(const QStringList &filePaths)
{
    const bool wasIdle = m_queue.isEmpty() && m_currentFile.isEmpty();
    m_queue.append(filePaths);
    if (wasIdle)
        startNextFile();
}

void RagIngestionController::startNextFile()
{
    if (m_queue.isEmpty()) {
        emit queueFinished();
        return;
    }

    m_currentFile = m_queue.takeFirst();
    m_currentChunks.clear();
    m_nextChunkBatchStart = 0;
    m_currentDocumentId.clear();

    emit progress(QFileInfo(m_currentFile).fileName(), 0, 0);

    const QString filePath = m_currentFile;
    const int chunkSize = QSettings().value("rag/chunkSizeChars", 1500).toInt();
    const int overlap = QSettings().value("rag/chunkOverlapChars", 200).toInt();

    using ExtractResult = QPair<QStringList, QString>;
    auto *watcher = new QFutureWatcher<ExtractResult>(this);
    connect(watcher, &QFutureWatcher<ExtractResult>::finished, this, [this, watcher, filePath]() {
        const ExtractResult result = watcher->result();
        watcher->deleteLater();
        onExtractionFinished(filePath, result.first, result.second);
    });

    QFuture<ExtractResult> future = QtConcurrent::run([filePath, chunkSize, overlap]() -> ExtractResult {
        QString error;
        const QString text = DocumentTextExtractor::extractText(filePath, &error);
        if (text.isEmpty())
            return {QStringList(), error.isEmpty() ? QStringLiteral("No extractable text found.") : error};

        const QStringList chunks = TextChunker::chunkText(text, chunkSize, overlap);
        if (chunks.isEmpty())
            return {QStringList(), QStringLiteral("No extractable text found.")};

        return {chunks, QString()};
    });
    watcher->setFuture(future);
}

void RagIngestionController::onExtractionFinished(const QString &filePath, const QStringList &chunks, const QString &error)
{
    const QString displayName = QFileInfo(filePath).fileName();

    if (chunks.isEmpty()) {
        emit fileFinished(displayName, false, error);
        startNextFile();
        return;
    }

    m_currentChunks = chunks;
    m_currentDocumentId = m_ragStore->addDocument(displayName, filePath);
    if (m_currentDocumentId.isEmpty()) {
        emit fileFinished(displayName, false, "Couldn't add this document to the knowledge base (database error).");
        startNextFile();
        return;
    }

    emit progress(displayName, 0, m_currentChunks.size());
    startNextEmbeddingBatch();
}

void RagIngestionController::startNextEmbeddingBatch()
{
    const QString displayName = QFileInfo(m_currentFile).fileName();

    if (m_nextChunkBatchStart >= m_currentChunks.size()) {
        emit fileFinished(displayName, true, QString());
        startNextFile();
        return;
    }

    const QString embeddingModel = QSettings().value("rag/embeddingModel").toString();
    if (embeddingModel.isEmpty()) {
        m_ragStore->removeDocument(m_currentDocumentId);
        emit fileFinished(displayName, false, "No embedding model configured — pick one in Settings.");
        startNextFile();
        return;
    }

    const int end = qMin(m_nextChunkBatchStart + kEmbeddingBatchSize, m_currentChunks.size());
    QStringList batchTexts;
    batchTexts.reserve(end - m_nextChunkBatchStart);
    for (int i = m_nextChunkBatchStart; i < end; ++i)
        batchTexts << m_currentChunks[i];

    m_currentEmbeddingRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_ollamaClient->requestEmbeddings(m_currentEmbeddingRequestId, embeddingModel, batchTexts);
}

void RagIngestionController::onEmbeddingsFetched(const QString &requestId, const QVector<QVector<float>> &embeddings)
{
    if (requestId != m_currentEmbeddingRequestId)
        return; // not our batch — e.g. a retrieval-time query embedding from ToolExecutor

    const QString displayName = QFileInfo(m_currentFile).fileName();

    if (embeddings.isEmpty()) {
        // Partial ingestion — don't leave a half-embedded, permanently
        // incomplete document sitting in the knowledge base.
        m_ragStore->removeDocument(m_currentDocumentId);
        emit fileFinished(displayName, false, "Embedding request failed (check that Ollama is reachable).");
        startNextFile();
        return;
    }

    for (int i = 0; i < embeddings.size(); ++i) {
        const int chunkIndex = m_nextChunkBatchStart + i;
        m_ragStore->addChunk(m_currentDocumentId, chunkIndex, m_currentChunks[chunkIndex], embeddings[i]);
    }

    m_nextChunkBatchStart += embeddings.size();
    emit progress(displayName, m_nextChunkBatchStart, m_currentChunks.size());
    startNextEmbeddingBatch();
}
