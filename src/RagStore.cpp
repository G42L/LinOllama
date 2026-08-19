#include "RagStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {

// Embeddings are stored as raw float32 blobs (little-endian, N*4 bytes) —
// far smaller than a JSON/text encoding of the same floats and avoids any
// float->string->float precision churn on the way in/out. Every machine
// this app targets is little-endian (x86_64/ARM64 desktop Linux), so no
// byte-swapping is done — same assumption Qt's own QDataStream default
// makes on these platforms.
QByteArray embeddingToBlob(const QVector<float> &embedding)
{
    return QByteArray(reinterpret_cast<const char *>(embedding.constData()),
                       embedding.size() * static_cast<int>(sizeof(float)));
}

QVector<float> blobToEmbedding(const QByteArray &blob)
{
    const int count = blob.size() / static_cast<int>(sizeof(float));
    QVector<float> embedding(count);
    memcpy(embedding.data(), blob.constData(), count * sizeof(float));
    return embedding;
}

float cosineSimilarity(const QVector<float> &a, const QVector<float> &b)
{
    if (a.size() != b.size() || a.isEmpty())
        return 0.0f;

    double dot = 0.0, normA = 0.0, normB = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        dot += double(a[i]) * double(b[i]);
        normA += double(a[i]) * double(a[i]);
        normB += double(b[i]) * double(b[i]);
    }
    if (normA <= 0.0 || normB <= 0.0)
        return 0.0f;
    return static_cast<float>(dot / (std::sqrt(normA) * std::sqrt(normB)));
}

} // namespace

RagStore::RagStore(QObject *parent)
    : QObject(parent)
    // A unique connection name per instance — QSqlDatabase's connections are
    // process-wide, named registrations (not tied to the QSqlDatabase value
    // itself), so two RagStore instances (e.g. one owned by MainWindow for
    // live tool-call queries, one opened by RagIngestionController on a
    // worker thread for ingestion) must not collide on the same name.
    , m_connectionName("RagStore_" + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

RagStore::~RagStore()
{
    if (m_open) {
        m_db.close();
        m_db = QSqlDatabase(); // drops this object's reference before removeDatabase() below
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

QString RagStore::databasePath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/knowledge_base.sqlite";
}

bool RagStore::open()
{
    if (m_open)
        return true;

    QDir().mkpath(QFileInfo(databasePath()).absolutePath());

    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    m_db.setDatabaseName(databasePath());
    if (!m_db.open()) {
        qWarning() << "RagStore: couldn't open" << databasePath() << "-" << m_db.lastError().text();
        return false;
    }

    QSqlQuery pragma(m_db);
    // SQLite defaults this off per-connection — without it, ON DELETE
    // CASCADE below is silently ignored and removeDocument() would leak
    // orphaned chunk rows.
    pragma.exec("PRAGMA foreign_keys = ON");

    QSqlQuery schema(m_db);
    schema.exec(
        "CREATE TABLE IF NOT EXISTS documents ("
        "  id TEXT PRIMARY KEY,"
        "  file_name TEXT NOT NULL,"
        "  source_path TEXT,"
        "  ingested_at INTEGER NOT NULL"
        ")");
    schema.exec(
        "CREATE TABLE IF NOT EXISTS chunks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  document_id TEXT NOT NULL REFERENCES documents(id) ON DELETE CASCADE,"
        "  chunk_index INTEGER NOT NULL,"
        "  text TEXT NOT NULL,"
        "  embedding BLOB NOT NULL"
        ")");
    schema.exec("CREATE INDEX IF NOT EXISTS idx_chunks_document_id ON chunks(document_id)");

    m_open = true;
    return true;
}

QVector<RagStore::DocumentInfo> RagStore::listDocuments() const
{
    QVector<DocumentInfo> result;
    if (!m_open)
        return result;

    QSqlQuery query(m_db);
    query.exec(
        "SELECT d.id, d.file_name, d.source_path, d.ingested_at, COUNT(c.id) "
        "FROM documents d LEFT JOIN chunks c ON c.document_id = d.id "
        "GROUP BY d.id "
        "ORDER BY d.ingested_at DESC");
    while (query.next()) {
        DocumentInfo info;
        info.id = query.value(0).toString();
        info.fileName = query.value(1).toString();
        info.sourcePath = query.value(2).toString();
        info.ingestedAt = QDateTime::fromSecsSinceEpoch(query.value(3).toLongLong());
        info.chunkCount = query.value(4).toInt();
        result.append(info);
    }
    return result;
}

bool RagStore::isEmpty() const
{
    if (!m_open)
        return true;
    QSqlQuery query(m_db);
    query.exec("SELECT COUNT(*) FROM documents");
    return !query.next() || query.value(0).toInt() == 0;
}

QString RagStore::addDocument(const QString &fileName, const QString &sourcePath)
{
    if (!m_open)
        return QString();

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery query(m_db);
    query.prepare("INSERT INTO documents (id, file_name, source_path, ingested_at) VALUES (?, ?, ?, ?)");
    query.addBindValue(id);
    query.addBindValue(fileName);
    query.addBindValue(sourcePath);
    query.addBindValue(QDateTime::currentDateTime().toSecsSinceEpoch());
    if (!query.exec()) {
        qWarning() << "RagStore::addDocument failed:" << query.lastError().text();
        return QString();
    }
    return id;
}

void RagStore::addChunk(const QString &documentId, int chunkIndex, const QString &text, const QVector<float> &embedding)
{
    if (!m_open)
        return;

    QSqlQuery query(m_db);
    query.prepare("INSERT INTO chunks (document_id, chunk_index, text, embedding) VALUES (?, ?, ?, ?)");
    query.addBindValue(documentId);
    query.addBindValue(chunkIndex);
    query.addBindValue(text);
    query.addBindValue(embeddingToBlob(embedding));
    if (!query.exec())
        qWarning() << "RagStore::addChunk failed:" << query.lastError().text();
}

void RagStore::removeDocument(const QString &documentId)
{
    if (!m_open)
        return;
    QSqlQuery query(m_db);
    query.prepare("DELETE FROM documents WHERE id = ?");
    query.addBindValue(documentId);
    query.exec();
}

QVector<RagStore::ScoredChunk> RagStore::topKSimilar(const QVector<float> &queryEmbedding, int topK) const
{
    QVector<ScoredChunk> results;
    if (!m_open || queryEmbedding.isEmpty() || topK <= 0)
        return results;

    QSqlQuery query(m_db);
    query.exec(
        "SELECT c.document_id, d.file_name, c.chunk_index, c.text, c.embedding "
        "FROM chunks c JOIN documents d ON d.id = c.document_id");

    QVector<ScoredChunk> all;
    while (query.next()) {
        ScoredChunk chunk;
        chunk.documentId = query.value(0).toString();
        chunk.documentName = query.value(1).toString();
        chunk.chunkIndex = query.value(2).toInt();
        chunk.text = query.value(3).toString();
        const QVector<float> embedding = blobToEmbedding(query.value(4).toByteArray());
        chunk.score = cosineSimilarity(queryEmbedding, embedding);
        all.append(chunk);
    }

    const int count = std::min(qsizetype(topK), all.size());
    std::partial_sort(all.begin(), all.begin() + count, all.end(),
                       [](const ScoredChunk &a, const ScoredChunk &b) { return a.score > b.score; });
    for (int i = 0; i < count; ++i)
        results.append(all[i]);
    return results;
}

qint64 RagStore::databaseSizeBytes() const
{
    if (!m_open)
        return 0;
    return QFileInfo(databasePath()).size();
}
