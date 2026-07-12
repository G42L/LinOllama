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
    // Explicitly stops whisper-server (if running) rather than relying on
    // Qt's parent-child auto-deletion — see stopLiveServer()'s own comment;
    // this is what makes sure the OS process is actually killed, not just
    // left orphaned, when the app quits mid-live-transcription.
    ~WhisperManager() override;

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

    // --- Live transcription (whisper-server) -------------------------------
    // whisper-cli (above) reloads the whole model on every single
    // invocation, which is fine for one push-to-talk recording but far too
    // slow to re-run every few seconds while someone is still talking.
    // whisper-server (a separate binary from the same whisper.cpp checkout,
    // built from its examples/server) loads the model once and stays warm,
    // answering HTTP requests — this section manages that process and talks
    // to it, entirely separately from the whisper-cli path above.

    // "whisper/serverBinaryPath" in QSettings — same override/autodetect
    // pattern as binaryPath()/setBinaryPath(), just a different binary name
    // ("whisper-server", falling back to the older "server" target name),
    // searched in the same directory as binaryPath() plus the usual PATH/
    // common-install-location candidates.
    QString serverBinaryPath() const { return m_serverBinaryPath; }
    void setServerBinaryPath(const QString &path);
    bool isServerBinaryAvailable() const;

    bool isLiveServerRunning() const { return m_serverProcess != nullptr; }
    // Starts whisper-server against the currently selected model if it
    // isn't already running with that exact model, listening on a fixed
    // local-only port. A no-op (beyond immediately re-emitting
    // liveServerStateChanged(true) if already up) when it's already running
    // with the right model. Readiness is confirmed by polling the HTTP port
    // rather than watching stdout/stderr for a "ready" line — whisper.cpp's
    // exact log wording isn't a stable contract to depend on, but the port
    // actually answering is.
    void ensureLiveServerRunning();
    void stopLiveServer();

    // Sends one already-WAV-encoded audio chunk to the running server's
    // /inference endpoint. isFinalChunk is passed straight through to
    // liveChunkTranscribed() so the caller can tell "just another
    // mid-utterance chunk" apart from "recording just stopped, this is the
    // last one" without separate bookkeeping of its own. Calls queue
    // internally (sent one at a time, in submission order) rather than
    // firing concurrently — see sendNextQueuedChunk()'s own comment for why
    // that matters for keeping transcribed text in order, and for why this
    // never blocks the caller (VoiceRecorder's own audio capture is never
    // held up by transcription falling behind).
    void transcribeChunkLive(const QByteArray &wavData, bool isFinalChunk);

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

    // running is false both while it's still starting up and after it's
    // stopped/failed — error is empty in the (also-false) "still starting"
    // case, and non-empty for a genuine failure to start/come up in time.
    void liveServerStateChanged(bool running, const QString &error);
    // One per transcribeChunkLive() call, always eventually — see that
    // method's own comment on ordering. success false covers both an HTTP-
    // level failure and the server not being up at all when the chunk was
    // submitted.
    void liveChunkTranscribed(const QString &text, bool isFinalChunk, bool success, const QString &error);

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

    // Same "search a fixed candidate list" shape as redetect()'s own
    // binaryPath detection, just for the server binary — factored out since
    // redetect() needs to call it too (server autodetection depends on
    // where binaryPath ended up, so it has to happen after that's known).
    QString detectServerBinary() const;
    // Polls http://127.0.0.1:<port>/ every 300ms (up to ~15s) until it gets
    // any HTTP response at all, rather than parsing whisper-server's
    // stdout/stderr for a "ready" line — see ensureLiveServerRunning()'s own
    // comment for why. Only ever running while m_serverProcess is the one
    // it started polling for; stopLiveServer() implicitly cancels it by
    // clearing m_serverProcess, which this checks on every attempt.
    void pollServerReadiness(int attempt);
    // Sends whichever chunk is at the front of m_chunkQueue, if the server
    // is ready and nothing else is already in flight — called after every
    // transcribeChunkLive() and again once each request finishes, so the
    // queue keeps draining one at a time without the caller having to drive
    // it. One-at-a-time (rather than firing every queued chunk concurrently)
    // is what keeps liveChunkTranscribed() results arriving in the same
    // order the audio was recorded in — concurrent requests could finish in
    // any order and scramble the transcript.
    void sendNextQueuedChunk();

    QString m_binaryPath;
    QString m_modelsDir;
    QString m_serverBinaryPath;

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

    // --- Live transcription (whisper-server) state --------------------------
    QProcess *m_serverProcess = nullptr;
    // True only once pollServerReadiness() has actually seen a response —
    // m_serverProcess alone just means "the OS process exists," not "the
    // HTTP server inside it is accepting requests yet."
    bool m_serverReady = false;
    // Which model m_serverProcess was launched with — ensureLiveServerRunning()
    // restarts the process if selectedModel() no longer matches this,
    // since whisper-server (like whisper-cli) only ever loads one model,
    // fixed at its own startup.
    QString m_liveServerModel;
    static constexpr quint16 kLiveServerPort = 8781; // arbitrary, local-only

    struct QueuedChunk
    {
        QByteArray wavData;
        bool isFinalChunk = false;
    };
    QVector<QueuedChunk> m_chunkQueue;
    QNetworkReply *m_activeChunkReply = nullptr;
};
