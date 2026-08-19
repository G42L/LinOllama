#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QSqlDatabase>

// Owns the app's document knowledge base: one global SQLite database
// (Qt6::Sql, sqlite driver) shared across every conversation — see the
// RAG plan's own "global vs per-conversation" note for why one shared
// store rather than one per conversation. Holds ingested documents' text
// chunks and their embedding vectors, and answers "which chunks are most
// similar to this query embedding" via a brute-force cosine-similarity
// scan — a real linear scan over every stored chunk, not an approximate/
// indexed search, which is fine at the realistic scale of one person's own
// document set (thousands, not millions, of chunks; see topKSimilar()'s
// own comment).
//
// Not thread-safe — QSqlDatabase connections are only valid on the thread
// that created them. Only ever touched from the main thread in practice:
// RagIngestionController offloads text extraction/chunking to a worker
// thread (see QtConcurrent::run in its own .cpp), but embedding requests
// and every RagStore call happen back on the main thread once each batch's
// result arrives via OllamaClient's signals.
class RagStore : public QObject
{
    Q_OBJECT

public:
    explicit RagStore(QObject *parent = nullptr);
    ~RagStore() override;

    // Opens (creating if needed) the database at
    // AppDataLocation/knowledge_base.sqlite and ensures the schema exists.
    // Safe to call more than once (a no-op if already open). Returns false
    // on a genuine failure to open/migrate — callers should treat the store
    // as unusable in that case rather than proceeding.
    bool open();

    struct DocumentInfo
    {
        QString id;
        QString fileName;
        QString sourcePath;
        int chunkCount = 0;
        QDateTime ingestedAt;
    };
    // Ordered by ingestedAt descending (most recently added first) — what
    // the Settings document list shows.
    QVector<DocumentInfo> listDocuments() const;
    bool isEmpty() const; // true if no documents have been ingested at all — see ChatWidget's tool-gating

    // Creates a new documents row and returns its id (a fresh QUuid,
    // stringified) — chunks are added afterward, one at a time, via
    // addChunk(). Splitting document creation from chunk insertion (rather
    // than one bulk call) matches how RagIngestionController actually
    // produces them: text extraction and chunking happen up front, but
    // embeddings arrive incrementally, one OllamaClient::requestEmbeddings()
    // batch at a time.
    QString addDocument(const QString &fileName, const QString &sourcePath);
    // embedding.size() should match every other chunk's — not enforced
    // here (this class doesn't know the configured embedding model's
    // dimensionality), but topKSimilar() will silently produce meaningless
    // scores against a mismatched-dimension query if it isn't.
    void addChunk(const QString &documentId, int chunkIndex, const QString &text, const QVector<float> &embedding);
    // Deletes the document and every chunk that belongs to it (ON DELETE
    // CASCADE). A no-op if documentId doesn't exist.
    void removeDocument(const QString &documentId);

    struct ScoredChunk
    {
        QString documentId;
        QString documentName;
        int chunkIndex = 0;
        QString text;
        float score = 0.0f; // cosine similarity, [-1, 1] in theory, realistically (0, 1] for real text embeddings
    };
    // Brute-force cosine similarity of queryEmbedding against every stored
    // chunk's embedding, returning the topK highest-scoring chunks in
    // descending score order. Empty if the store has no chunks at all.
    // "Brute-force" here really does mean a full table scan every call —
    // deliberate, not a placeholder for a future index: see this class's
    // own header comment for why that's an acceptable tradeoff at this
    // app's scale.
    QVector<ScoredChunk> topKSimilar(const QVector<float> &queryEmbedding, int topK) const;

    // Total on-disk size of the database file, for an optional "X MB used"
    // label in Settings — 0 if the store isn't open yet.
    qint64 databaseSizeBytes() const;

private:
    static QString databasePath();

    QSqlDatabase m_db;
    QString m_connectionName; // unique per-instance name — see .cpp constructor comment
    bool m_open = false;
};
