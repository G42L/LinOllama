#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <QNetworkAccessManager>
#include <QProcess>

class QNetworkReply;
class QFile;

// One catalog entry's static description (see WhisperManager::catalog()) —
// not tied to whether it's actually installed, that's a separate runtime
// check against modelsDir().
struct WhisperModelInfo
{
    QString id;          // e.g. "medium", "medium.en" — also the ggml-<id>.bin file stem
    QString diskSize;     // "1.5 GiB"
    QString memEstimate;  // "~2.1 GB"
    QString language;     // "any" | "english"
    QString speed;        // star rating, e.g. "⚡⚡"
    QString accuracy;     // star rating, e.g. "⭐⭐⭐"
    QString usage;         // short blurb, e.g. "Recommended"
};

// Local speech-to-text via whisper.cpp's `whisper-cli` binary, shelled out to
// per push-to-talk recording — the same "detect an external process, drive
// it via QProcess" pattern ServerController uses for Ollama itself. A single
// shared instance (see main.cpp) since Settings' model list/download UI and
// ChatWidget's transcription both need to see the same install/selection,
// and only one transcription can usefully run at a time anyway (push-to-talk
// is inherently serial).
class WhisperManager : public QObject
{
    Q_OBJECT

public:
    explicit WhisperManager(QObject *parent = nullptr);

    // The fixed set of models Settings offers to download — a hand-picked
    // subset of everything whisper.cpp's own download script knows about
    // (see models/download-ggml-model.sh upstream), skipping quantized and
    // diarization variants to keep the picker simple.
    static QVector<WhisperModelInfo> catalog();

    // Re-runs autodetection of the binary and models directory from
    // scratch (saved QSettings paths first, then common install locations)
    // and re-scans for installed models. Called once at construction and
    // again whenever Settings changes either path.
    void redetect();

    QString binaryPath() const { return m_binaryPath; }
    QString modelsDir() const { return m_modelsDir; }
    bool isBinaryAvailable() const;

    // Empty path resets to autodetection. Both persist to QSettings
    // ("whisper/binaryPath"/"whisper/modelsDir") and re-scan immediately.
    void setBinaryPath(const QString &path);
    void setModelsDir(const QString &path);

    // ids (catalog or not) found as ggml-<id>.bin under modelsDir().
    QStringList installedModels() const;

    // "whisper/selectedModel" in QSettings. If nothing has ever been picked
    // (empty) and at least one model is installed, auto-picks via
    // pickDefaultModel() and persists that choice so it sticks across
    // launches from then on, same as an explicit user choice would.
    QString selectedModel();
    void setSelectedModel(const QString &modelId);

    // "medium" if installed, else the best available among (in order)
    // large-v3, large-v3-turbo, small, base, tiny — deliberately never an
    // ".en" variant, since those are English-only and shouldn't be picked
    // automatically over a multilingual model of similar quality. Returns
    // empty if none of these are installed.
    static QString pickDefaultModel(const QStringList &installed);

    bool isDownloading(const QString &modelId) const;
    void downloadModel(const QString &modelId);
    void cancelDownload(const QString &modelId);

    bool isTranscribing() const { return m_transcribeProcess != nullptr; }
    // Runs whisper-cli against wavPath using the currently selected model.
    // Always emits transcriptionFinished() exactly once, asynchronously —
    // including for "can't even start" cases (not configured, already busy,
    // model file missing), so callers can rely on the signal alone rather
    // than also checking a return value.
    void transcribe(const QString &wavPath);

signals:
    // Fires after any change to what's actually on disk (download finished,
    // models directory changed) — Settings' model table listens live.
    void modelsChanged();
    void downloadProgress(const QString &modelId, qint64 received, qint64 total);
    void downloadFinished(const QString &modelId, bool success, const QString &error);
    void transcriptionFinished(const QString &text, bool success, const QString &error);
    // Fires every time whisper-cli flushes more segment text (it does so
    // per-segment, not just at exit — see cli.cpp's whisper_print_segment_callback),
    // with the best-effort transcript so far, so a caller can show live
    // feedback instead of a blank box until the whole recording is done.
    void transcriptionProgress(const QString &partialText);

private slots:
    void onTranscribeReadyReadStandardOutput();
    void onTranscribeProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString findModelPath(const QString &modelId) const;
    static bool isEnglishOnly(const QString &modelId);
    // Trims each line, drops empties, joins what's left with a single space —
    // shared by the live progress signal and the final one so mid-stream
    // text and the final text are formatted identically.
    static QString cleanTranscript(const QByteArray &rawStdout);
    // Picks out an actual diagnostic line from whisper-cli's stderr (one
    // containing "error"/"must be"/"failed"/"warning"), ignoring the
    // system_info/timing lines it always prints regardless of outcome —
    // see onTranscribeProcessFinished()'s own comment for why the naive
    // "just take the last line" approach was actively misleading. Empty if
    // nothing that looks like a real diagnostic is present.
    static QString extractDiagnosticLine(const QString &stdErr);
    // Picks and persists a writable default (~/whisper.cpp/models if it
    // already exists, else an app-local data directory) when neither a
    // saved setting nor autodetection found an existing directory — called
    // lazily, only once a download actually needs somewhere to land.
    void ensureModelsDirSet();

    QString m_binaryPath;
    QString m_modelsDir;

    QNetworkAccessManager m_network;
    struct DownloadState
    {
        QNetworkReply *reply = nullptr;
        QFile *file = nullptr;
    };
    QHash<QString, DownloadState> m_downloads; // keyed by modelId

    QProcess *m_transcribeProcess = nullptr;
    // Raw stdout bytes seen so far for the in-flight transcription, re-parsed
    // (not just appended pre-formatted) on every readyRead so a segment that
    // arrives split across two reads is still cleaned up correctly. Reset at
    // the start of each transcribe() call, so it never grows across
    // recordings — nothing here outlives a single push-to-talk turn.
    QByteArray m_transcribeStdoutBuffer;
    // The wavPath passed to the in-flight transcribe() call, kept only so
    // onTranscribeProcessFinished() can report the file's size alongside a
    // "didn't catch any speech" result — a near-header-only file (tens of
    // bytes) points at the recording itself being the problem, whereas a
    // normal-sized one that still transcribes empty points at genuine
    // silence instead. Cleared alongside m_transcribeStdoutBuffer.
    QString m_transcribeWavPath;
};
