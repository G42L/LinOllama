#pragma once

#include <QObject>
#include <QStringList>
#include <QString>

class OllamaClient;
class RagStore;

// Drives the "Add document(s)…" flow in Settings' Knowledge Base tab:
// extract text -> chunk -> embed (batched, via OllamaClient) -> store (via
// RagStore), one file at a time. Owned at the application level (alongside
// OllamaClient/RagStore, constructed once in MainWindow) rather than by
// SettingsDialog itself, so ingestion keeps running if Settings is closed
// mid-download — SettingsDialog just calls ingestFiles() and connects to
// this class's signals for its progress UI.
//
// Sequential by design: one file, and within it one embedding batch, in
// flight at a time. Ollama can only usefully do one thing at a time anyway
// (same reasoning ChatQueue applies to chat turns), and sequential
// ingestion makes per-file progress reporting straightforward — no
// interleaved progress from multiple documents to reconcile.
class RagIngestionController : public QObject
{
    Q_OBJECT

public:
    RagIngestionController(OllamaClient *ollamaClient, RagStore *ragStore, QObject *parent = nullptr);

    // Appends filePaths to the ingestion queue. Safe to call again while a
    // previous batch is still running — new paths queue up behind it
    // rather than interrupting it. Reads the configured embedding model
    // and chunk size/overlap from QSettings ("rag/embeddingModel",
    // "rag/chunkSizeChars", "rag/chunkOverlapChars") fresh for each file as
    // its turn comes up, rather than snapshotting them here — so a setting
    // changed mid-queue applies to files not yet started.
    void ingestFiles(const QStringList &filePaths);

signals:
    // Fires once as each file starts (chunkIndex/chunkCount both 0 during
    // extraction/chunking, which happens on a worker thread first — see
    // startNextFile()), then again after every embedding batch returns.
    // fileName is just the display name (QFileInfo::fileName()), not the
    // full path.
    void progress(const QString &fileName, int chunkIndex, int chunkCount);
    // Once per queued file, success or failure — errorMessage is empty on
    // success. A failure partway through embedding removes whatever
    // partial RagStore document/chunks that file had already written (see
    // onEmbeddingsFetched()), so a failed file never leaves a half-indexed
    // document behind. Settings' document list should refresh on every one
    // of these, not just once at the end, since files are ingested and
    // become searchable one at a time.
    void fileFinished(const QString &fileName, bool success, const QString &errorMessage);
    // Once the queue (everything passed to ingestFiles() so far) has fully
    // drained. Not emitted again until ingestFiles() is called with more.
    void queueFinished();

private slots:
    // Connected to the shared OllamaClient::embeddingsFetched() — filters
    // by requestId since that signal is also used for retrieval-time query
    // embeddings (see ToolExecutor's search_knowledge_base handling), which
    // this class must ignore rather than mistake for its own batch result.
    void onEmbeddingsFetched(const QString &requestId, const QVector<QVector<float>> &embeddings);

private:
    // Pops the next queued file and kicks off its extraction/chunking on a
    // worker thread (QtConcurrent::run) — emits queueFinished() instead if
    // the queue is empty.
    void startNextFile();
    // Runs on the main thread once extraction+chunking (on the worker
    // thread) completes for m_currentFile. Empty chunks means extraction
    // failed or produced nothing — reports failure and moves on. Otherwise
    // creates the RagStore document row and starts the first embedding
    // batch.
    void onExtractionFinished(const QString &filePath, const QStringList &chunks, const QString &error);
    // Issues the next up-to-kEmbeddingBatchSize-chunk embed request for
    // m_currentChunks, or finishes the current file (success) if every
    // chunk has already been embedded.
    void startNextEmbeddingBatch();

    OllamaClient *m_ollamaClient = nullptr;
    RagStore *m_ragStore = nullptr;

    QStringList m_queue;

    // State for whichever file is currently being extracted/chunked/embedded,
    // valid only while one is in flight (i.e. between startNextFile() popping
    // it and its own fileFinished() firing).
    QString m_currentFile;
    QString m_currentDocumentId;
    QStringList m_currentChunks;
    int m_nextChunkBatchStart = 0;
    QString m_currentEmbeddingRequestId;

    static constexpr int kEmbeddingBatchSize = 32;
};
