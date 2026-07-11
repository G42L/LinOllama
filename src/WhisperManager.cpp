#include "WhisperManager.h"

#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>

namespace {
// download-ggml-model.sh's own URL scheme (see upstream whisper.cpp) —
// resolves through a redirect to the actual CDN, hence NoLessSafeRedirectPolicy below.
QString modelDownloadUrl(const QString &modelId)
{
    return QString("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-%1.bin").arg(modelId);
}
}

WhisperManager::WhisperManager(QObject *parent)
    : QObject(parent)
{
    redetect();
}

QVector<WhisperModelInfo> WhisperManager::catalog()
{
    // Matches the MODEL_DATA table this feature was speced against —
    // disk/mem/speed/accuracy are indicative, not measured on this machine.
    return {
        {"tiny",           "75 MiB",  "~273 MB", "any",     "⚡⚡⚡⚡⚡", "⭐⭐",     "Quick tests"},
        {"tiny.en",        "75 MiB",  "~273 MB", "english", "⚡⚡⚡⚡⚡", "⭐⭐",     "Quick tests"},
        {"base",           "142 MiB", "~388 MB", "any",     "⚡⚡⚡⚡",  "⭐⭐⭐",    "Simple transcriptions"},
        {"base.en",        "142 MiB", "~388 MB", "english", "⚡⚡⚡⚡",  "⭐⭐⭐",    "Simple transcriptions"},
        {"small",          "466 MiB", "~852 MB", "any",     "⚡⚡⚡",   "⭐⭐⭐⭐",   "Balanced"},
        {"small.en",       "466 MiB", "~852 MB", "english", "⚡⚡⚡",   "⭐⭐⭐⭐",   "Balanced"},
        {"medium",         "1.5 GiB", "~2.1 GB", "any",     "⚡⚡",    "⭐⭐⭐⭐⭐", "Recommended"},
        {"medium.en",      "1.5 GiB", "~2.1 GB", "english", "⚡⚡",    "⭐⭐⭐⭐⭐", "Recommended"},
        {"large-v3",       "2.9 GiB", "~3.9 GB", "any",     "⚡",     "⭐⭐⭐⭐⭐", "Maximum accuracy"},
        {"large-v3-turbo", "1.5 GiB", "~2.1 GB", "any",     "⚡",     "⭐⭐⭐⭐⭐", "Maximum accuracy"},
    };
}

void WhisperManager::redetect()
{
    QSettings settings;

    m_binaryPath.clear();
    const QString savedBinary = settings.value("whisper/binaryPath").toString();
    if (!savedBinary.isEmpty() && QFileInfo(savedBinary).isExecutable()) {
        m_binaryPath = savedBinary;
    } else {
        const QStringList candidates = {
            QDir::homePath() + "/whisper.cpp/build/bin/whisper-cli",
            QStandardPaths::findExecutable("whisper-cli"),
            "/usr/local/bin/whisper-cli",
            "/usr/bin/whisper-cli",
        };
        for (const QString &candidate : candidates) {
            if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable()) {
                m_binaryPath = candidate;
                break;
            }
        }
    }

    m_modelsDir.clear();
    const QString savedModelsDir = settings.value("whisper/modelsDir").toString();
    if (!savedModelsDir.isEmpty() && QDir(savedModelsDir).exists()) {
        m_modelsDir = savedModelsDir;
    } else {
        QStringList candidates = {QDir::homePath() + "/whisper.cpp/models"};
        if (!m_binaryPath.isEmpty()) {
            // .../whisper.cpp/build/bin/whisper-cli -> .../whisper.cpp/models
            QDir binDir = QFileInfo(m_binaryPath).dir();
            candidates.prepend(binDir.filePath("../../models"));
        }
        for (const QString &candidate : candidates) {
            QDir dir(candidate);
            if (dir.exists()) {
                m_modelsDir = dir.canonicalPath();
                break;
            }
        }
    }

    emit modelsChanged();
}

bool WhisperManager::isBinaryAvailable() const
{
    return !m_binaryPath.isEmpty() && QFileInfo(m_binaryPath).isExecutable();
}

void WhisperManager::setBinaryPath(const QString &path)
{
    QSettings settings;
    if (path.isEmpty())
        settings.remove("whisper/binaryPath");
    else
        settings.setValue("whisper/binaryPath", path);
    redetect();
}

void WhisperManager::setModelsDir(const QString &path)
{
    QSettings settings;
    if (path.isEmpty())
        settings.remove("whisper/modelsDir");
    else
        settings.setValue("whisper/modelsDir", path);
    redetect();
}

QStringList WhisperManager::installedModels() const
{
    QStringList result;
    if (m_modelsDir.isEmpty())
        return result;

    const QDir dir(m_modelsDir);
    const QStringList files = dir.entryList({"ggml-*.bin"}, QDir::Files);
    for (const QString &fileName : files) {
        QString id = fileName;
        id.remove(0, QString("ggml-").length());
        id.chop(QString(".bin").length());
        result.append(id);
    }
    return result;
}

QString WhisperManager::selectedModel()
{
    QSettings settings;
    QString id = settings.value("whisper/selectedModel").toString();
    if (!id.isEmpty())
        return id;

    id = pickDefaultModel(installedModels());
    if (!id.isEmpty())
        settings.setValue("whisper/selectedModel", id);
    return id;
}

void WhisperManager::setSelectedModel(const QString &modelId)
{
    QSettings settings;
    settings.setValue("whisper/selectedModel", modelId);
}

QString WhisperManager::pickDefaultModel(const QStringList &installed)
{
    static const QStringList kOrder = {
        "medium", "large-v3", "large-v3-turbo", "small", "base", "tiny",
    };
    for (const QString &id : kOrder) {
        if (installed.contains(id))
            return id;
    }
    return QString();
}

bool WhisperManager::isDownloading(const QString &modelId) const
{
    return m_downloads.contains(modelId);
}

void WhisperManager::ensureModelsDirSet()
{
    if (!m_modelsDir.isEmpty())
        return;

    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/whisper-models";
    QDir().mkpath(fallback);
    m_modelsDir = fallback;

    QSettings settings;
    settings.setValue("whisper/modelsDir", fallback);
}

void WhisperManager::downloadModel(const QString &modelId)
{
    if (m_downloads.contains(modelId))
        return;

    ensureModelsDirSet();
    QDir().mkpath(m_modelsDir);

    const QString finalPath = m_modelsDir + "/ggml-" + modelId + ".bin";
    const QString partPath = finalPath + ".part";

    auto *file = new QFile(partPath, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFinished(modelId, false, "Couldn't write to " + partPath);
        delete file;
        return;
    }

    QNetworkRequest request{QUrl(modelDownloadUrl(modelId))};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network.get(request);

    DownloadState state;
    state.reply = reply;
    state.file = file;
    m_downloads.insert(modelId, state);

    connect(reply, &QNetworkReply::readyRead, this, [this, modelId, reply, file]() {
        Q_UNUSED(modelId);
        file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, modelId](qint64 received, qint64 total) {
        emit downloadProgress(modelId, received, total);
    });
    connect(reply, &QNetworkReply::finished, this, [this, modelId, finalPath, partPath]() {
        DownloadState finishedState = m_downloads.take(modelId);
        QNetworkReply *finishedReply = finishedState.reply;
        QFile *finishedFile = finishedState.file;

        finishedFile->write(finishedReply->readAll());
        finishedFile->close();

        const bool ok = finishedReply->error() == QNetworkReply::NoError;
        if (ok) {
            QFile::remove(finalPath); // in case of a stale leftover
            finishedFile->rename(finalPath);
        } else {
            finishedFile->remove();
        }

        const QString error = ok ? QString() : finishedReply->errorString();
        finishedReply->deleteLater();
        finishedFile->deleteLater();

        emit downloadFinished(modelId, ok, error);
        if (ok)
            emit modelsChanged();
    });
}

void WhisperManager::cancelDownload(const QString &modelId)
{
    auto it = m_downloads.constFind(modelId);
    if (it == m_downloads.constEnd())
        return;
    it->reply->abort(); // finished() still fires, and does all the cleanup
}

bool WhisperManager::isEnglishOnly(const QString &modelId)
{
    return modelId.endsWith(".en");
}

QString WhisperManager::findModelPath(const QString &modelId) const
{
    if (m_modelsDir.isEmpty() || modelId.isEmpty())
        return QString();
    return m_modelsDir + "/ggml-" + modelId + ".bin";
}

void WhisperManager::transcribe(const QString &wavPath)
{
    if (m_transcribeProcess) {
        emit transcriptionFinished(QString(), false, "Still transcribing the previous recording.");
        return;
    }

    if (!isBinaryAvailable()) {
        emit transcriptionFinished(QString(), false,
            "Whisper isn't set up yet — configure it in Settings.");
        return;
    }

    const QString modelId = selectedModel();
    if (modelId.isEmpty()) {
        emit transcriptionFinished(QString(), false,
            "No Whisper model selected — pick one in Settings.");
        return;
    }

    const QString modelPath = findModelPath(modelId);
    if (!QFileInfo::exists(modelPath)) {
        emit transcriptionFinished(QString(), false,
            "Selected Whisper model (" + modelId + ") isn't downloaded — check Settings.");
        return;
    }

    QStringList args = {
        "-m", modelPath,
        "-f", wavPath,
        "-nt", // no timestamps — just the text
        "-l", isEnglishOnly(modelId) ? "en" : "auto",
    };

    m_transcribeStdoutBuffer.clear();
    m_transcribeWavPath = wavPath;

    m_transcribeProcess = new QProcess(this);
    connect(m_transcribeProcess, &QProcess::readyReadStandardOutput,
            this, &WhisperManager::onTranscribeReadyReadStandardOutput);
    connect(m_transcribeProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WhisperManager::onTranscribeProcessFinished);
    m_transcribeProcess->setProgram(m_binaryPath);
    m_transcribeProcess->setArguments(args);
    m_transcribeProcess->start();
}

QString WhisperManager::cleanTranscript(const QByteArray &rawStdout)
{
    // whisper-cli (run with -nt) flushes each segment's text as soon as it's
    // decoded rather than buffering until exit — see cli.cpp's
    // whisper_print_segment_callback, which fflush(stdout)s per segment —
    // so re-parsing the accumulated bytes on every readyRead is what turns
    // that into live incremental text instead of one lump at the end.
    QStringList lines = QString::fromUtf8(rawStdout).split('\n');
    for (QString &line : lines)
        line = line.trimmed();
    lines.removeAll(QString());
    return lines.join(' ').trimmed();
}

QString WhisperManager::extractDiagnosticLine(const QString &stdErr)
{
    // whisper-cli always prints a system_info line and, at the very end, a
    // whisper_print_timings block to stderr — regardless of whether
    // anything actually went wrong — so blindly grabbing the last line
    // (the previous approach) surfaced totally benign output like
    // "whisper_print_timings: total time = 674.97 ms" as if it were an
    // error. Real problems (a rejected WAV file, a model that failed to
    // load, etc.) print their own line containing one of these words
    // instead, so look for that specifically.
    const QStringList lines = stdErr.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (line.isEmpty())
            continue;
        const QString lower = line.toLower();
        if (lower.contains("error") || lower.contains("must be")
            || lower.contains("failed") || lower.contains("warning")) {
            return line;
        }
    }
    return QString();
}

void WhisperManager::onTranscribeReadyReadStandardOutput()
{
    if (!m_transcribeProcess)
        return;
    m_transcribeStdoutBuffer += m_transcribeProcess->readAllStandardOutput();
    const QString partial = cleanTranscript(m_transcribeStdoutBuffer);
    if (!partial.isEmpty())
        emit transcriptionProgress(partial);
}

void WhisperManager::onTranscribeProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *process = m_transcribeProcess;
    m_transcribeProcess = nullptr;

    // Whatever's left unread (readyReadStandardOutput doesn't guarantee
    // every last byte arrived before finished()) still needs folding in
    // before computing the final text.
    m_transcribeStdoutBuffer += process->readAllStandardOutput();
    const QString stdErr = QString::fromUtf8(process->readAllStandardError());
    process->deleteLater();

    const QString text = cleanTranscript(m_transcribeStdoutBuffer);
    m_transcribeStdoutBuffer.clear(); // nothing here should outlive this one turn — see the member's own comment

    const qint64 wavBytes = QFileInfo::exists(m_transcribeWavPath) ? QFileInfo(m_transcribeWavPath).size() : -1;
    m_transcribeWavPath.clear();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString diagnostic = extractDiagnosticLine(stdErr);
        const QString detail = diagnostic.isEmpty()
            ? QString("whisper-cli exited with code %1").arg(exitCode)
            : diagnostic;
        emit transcriptionFinished(QString(), false, detail);
        return;
    }

    if (text.isEmpty()) {
        // Exit code 0 with empty stdout isn't only "genuine silence" —
        // whisper-cli also exits 0 after silently *skipping* a file it
        // couldn't read at all (wrong sample rate/bit depth/channel count —
        // see read_wav() upstream), which prints its actual reason to
        // stderr rather than failing the process (see
        // extractDiagnosticLine()'s own comment for why the stderr tail
        // isn't used directly — it's always whisper-cli's own timing
        // summary, not an error). Falling back to the recorded file's size
        // separates "genuinely just silence" (a normal-sized file, tens of
        // KB+) from "the recording itself produced basically nothing"
        // (barely more than a WAV header, ~44 bytes) when there's no
        // clearer diagnostic line to report.
        const QString diagnostic = extractDiagnosticLine(stdErr);
        QString detail = diagnostic;
        if (detail.isEmpty()) {
            detail = (wavBytes >= 0 && wavBytes < 1024)
                ? QString("Didn't catch any speech — the recording itself was only %1 bytes, "
                          "which suggests nothing was actually captured.").arg(wavBytes)
                : "Didn't catch any speech in that recording.";
        }
        emit transcriptionFinished(QString(), false, detail);
        return;
    }

    emit transcriptionFinished(text, true, QString());
}
