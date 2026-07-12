#include "VoiceRecorder.h"

#include <QStandardPaths>
#include <QUuid>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QMediaDevices>
#include <QIODevice>
#include <QDataStream>
#include <QtMath>
#include <QDebug>
#include <algorithm>

namespace {
// whisper.cpp's own WAV reader (examples/common.cpp's read_wav) hard-
// requires exactly this — see VoiceRecorder.h's class comment for the
// history of why this is done with a hand-rolled resample/writer instead
// of QMediaRecorder.
constexpr int kTargetSampleRate = 16000;
constexpr int kTargetChannels = 1;

// Decodes one raw buffer into mono float samples in -1.0..1.0. For
// multi-channel input, downmixes by taking whichever channel has the
// larger magnitude in each frame, NOT by averaging them — averaging (a
// "true" linear mixdown) silently partially cancels the signal whenever
// the channels are even slightly out of phase with each other, which is
// common on dual-capsule mic arrays (e.g. some laptops' built-in stereo
// mic). That was the actual cause of a real regression here: the level
// meter (and the recorded audio itself, since both go through this same
// function) dropped to near-zero on a device whose two channels apparently
// don't sum constructively, despite the mic clearly being live. Picking
// the louder channel per frame sidesteps phase cancellation entirely.
QVector<float> decodeToMonoFloat(const QByteArray &data, const QAudioFormat &format)
{
    const int channels = qMax(1, format.channelCount());
    QVector<float> mono;

    auto pickLouder = [](double a, double b) {
        return qAbs(a) >= qAbs(b) ? a : b;
    };

    switch (format.sampleFormat()) {
    case QAudioFormat::Int16: {
        const auto *samples = reinterpret_cast<const qint16 *>(data.constData());
        const int frames = data.size() / (2 * channels);
        mono.reserve(frames);
        for (int f = 0; f < frames; ++f) {
            double best = 0.0;
            for (int c = 0; c < channels; ++c)
                best = pickLouder(best, samples[f * channels + c] / 32768.0);
            mono.append(static_cast<float>(best));
        }
        break;
    }
    case QAudioFormat::Int32: {
        const auto *samples = reinterpret_cast<const qint32 *>(data.constData());
        const int frames = data.size() / (4 * channels);
        mono.reserve(frames);
        for (int f = 0; f < frames; ++f) {
            double best = 0.0;
            for (int c = 0; c < channels; ++c)
                best = pickLouder(best, samples[f * channels + c] / 2147483648.0);
            mono.append(static_cast<float>(best));
        }
        break;
    }
    case QAudioFormat::Float: {
        const auto *samples = reinterpret_cast<const float *>(data.constData());
        const int frames = data.size() / (4 * channels);
        mono.reserve(frames);
        for (int f = 0; f < frames; ++f) {
            double best = 0.0;
            for (int c = 0; c < channels; ++c)
                best = pickLouder(best, samples[f * channels + c]);
            mono.append(static_cast<float>(best));
        }
        break;
    }
    case QAudioFormat::UInt8:
    default: {
        const auto *samples = reinterpret_cast<const quint8 *>(data.constData());
        const int frames = data.size() / channels;
        mono.reserve(frames);
        for (int f = 0; f < frames; ++f) {
            double best = 0.0;
            for (int c = 0; c < channels; ++c)
                best = pickLouder(best, (static_cast<int>(samples[f * channels + c]) - 128) / 128.0);
            mono.append(static_cast<float>(best));
        }
        break;
    }
    }

    return mono;
}

// Linear-interpolation resampler — not audiophile-grade, but whisper
// resamples/downmixes internally anyway and speech content survives linear
// interpolation just fine. Keeps this dependency-free (no extra library).
QVector<float> resampleLinear(const QVector<float> &samples, int srcRate, int dstRate)
{
    if (srcRate == dstRate || samples.isEmpty())
        return samples;

    const double duration = double(samples.size()) / double(srcRate);
    const int dstLen = qMax(1, static_cast<int>(std::round(duration * dstRate)));
    QVector<float> out(dstLen);

    for (int i = 0; i < dstLen; ++i) {
        const double srcPos = samples.size() > 1
            ? (double(i) / double(qMax(1, dstLen - 1))) * (samples.size() - 1)
            : 0.0;
        const int idx0 = static_cast<int>(srcPos);
        const int idx1 = qMin(idx0 + 1, samples.size() - 1);
        const double frac = srcPos - idx0;
        out[i] = static_cast<float>(samples[idx0] * (1.0 - frac) + samples[idx1] * frac);
    }
    return out;
}
}

VoiceRecorder::VoiceRecorder(QObject *parent) : QObject(parent)
{
    refreshAudioInputDevice();
    refreshMeterSmoothing();
}

void VoiceRecorder::refreshAudioInputDevice()
{
    const QByteArray savedId = QSettings().value("voice/audioInputDeviceId").toByteArray();

    QAudioDevice chosen;
    if (!savedId.isEmpty()) {
        const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
        for (const QAudioDevice &device : devices) {
            if (device.id() == savedId) {
                chosen = device;
                break;
            }
        }
        // A saved id that no longer matches anything (device unplugged,
        // renamed, etc.) silently falls through to the default below rather
        // than leaving recording pointed at a device that no longer exists.
    }
    if (chosen.isNull())
        chosen = QMediaDevices::defaultAudioInput();

    m_device = chosen;
}

void VoiceRecorder::refreshMeterSmoothing()
{
    const int percent = qBound(0, QSettings().value("voice/meterSmoothingPercent", 50).toInt(), 100);

    // Linear from each rate's "sharp" end (percent=0) to its "smooth" end
    // (percent=100), passing exactly through the original hardcoded values
    // (0.6/0.15) at percent=50 — so the slider's default position reproduces
    // this feature's behavior before the setting existed, sharper to the
    // left of center and smoother to the right.
    const qreal t = (50 - percent) / 50.0; // +1.0 at 0%, 0.0 at 50%, -1.0 at 100%
    m_attackRate = qBound(0.05, 0.6 + t * 0.3, 0.95);
    m_releaseRate = qBound(0.02, 0.15 + t * 0.13, 0.5);
}

QString VoiceRecorder::pickOutputDir()
{
    const QFileInfo shm("/dev/shm");
    if (shm.isDir() && shm.isWritable())
        return "/dev/shm";
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
}

void VoiceRecorder::startRecording(bool liveMode)
{
    if (m_source)
        return; // already recording — startRecording()/stopRecording() bracket, they don't nest

    if (m_device.isNull()) {
        emit recordingFailed("No audio input device available.");
        return;
    }

    // Requested directly at the device's native channel count — most
    // backends (this app runs on PipeWire) happily resample/downmix
    // whatever's requested, and decodeToMonoFloat()/resampleLinear() above
    // handle it generically either way, so there's no hard requirement that
    // the device grant this exactly.
    QAudioFormat format = m_device.preferredFormat();
    format.setSampleFormat(QAudioFormat::Int16);
    if (!m_device.isFormatSupported(format))
        format = m_device.preferredFormat();

    m_recordedMono.clear();
    m_smoothedLevel = 0.0;
    m_liveSilenceSeconds = 0.0;
    m_liveMode = liveMode;
    m_recording = true;

    m_source = new QAudioSource(m_device, format, this);
    m_ioDevice = m_source->start();
    if (!m_ioDevice) {
        m_source->deleteLater();
        m_source = nullptr;
        m_recording = false;
        emit recordingFailed("Couldn't open the selected audio input device.");
        return;
    }

    connect(m_ioDevice, &QIODevice::readyRead, this, &VoiceRecorder::onCaptureReadyRead);
}

void VoiceRecorder::stopRecording()
{
    if (!m_source)
        return;

    const QAudioFormat format = m_source->format();

    m_source->stop();
    m_source->deleteLater();
    m_source = nullptr;
    m_ioDevice = nullptr;
    m_recording = false;
    m_smoothedLevel = 0.0;
    emit audioLevelChanged(0.0);

    if (m_liveMode) {
        // Whatever's left since the last chunk boundary (possibly nothing,
        // if one landed right at release) — the caller's cue that this
        // recording's live stream of chunks is now complete.
        const QByteArray wav = flushLiveChunk(format);
        emit liveChunkReady(wav, /*isFinalChunk=*/true);
        return;
    }

    if (m_recordedMono.isEmpty()) {
        emit recordingFailed("No audio was captured.");
        return;
    }

    const QVector<float> resampled = resampleLinear(m_recordedMono, format.sampleRate(), kTargetSampleRate);
    m_recordedMono.clear();

    QByteArray pcm16;
    pcm16.resize(resampled.size() * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm16.data());
    for (int i = 0; i < resampled.size(); ++i) {
        const float clamped = std::clamp(resampled[i], -1.0f, 1.0f);
        out[i] = static_cast<qint16>(clamped * 32767.0f);
    }

    const QString path = pickOutputDir() + "/ollama-tray-voice-"
        + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".wav";
    if (!writeWavFile(path, pcm16, kTargetSampleRate, kTargetChannels)) {
        emit recordingFailed("Couldn't write the recorded audio to disk.");
        return;
    }

    emit recordingFinished(path);
}

bool VoiceRecorder::isRecording() const
{
    return m_recording;
}

void VoiceRecorder::onCaptureReadyRead()
{
    if (!m_ioDevice || !m_source)
        return;

    const QByteArray data = m_ioDevice->readAll();
    if (data.isEmpty())
        return;

    const QVector<float> mono = decodeToMonoFloat(data, m_source->format());
    if (mono.isEmpty())
        return;

    m_recordedMono += mono;

    qreal peak = 0.0;
    for (float sample : mono)
        peak = qMax(peak, static_cast<qreal>(qAbs(sample)));
    peak = qBound(0.0, peak, 1.0);

    // Fast attack, slow release — see m_attackRate/m_releaseRate's own
    // comment (adjustable via Settings' meter-smoothing slider).
    const qreal rate = (peak > m_smoothedLevel) ? m_attackRate : m_releaseRate;
    m_smoothedLevel += (peak - m_smoothedLevel) * rate;

    emit audioLevelChanged(m_smoothedLevel);

    if (!m_liveMode)
        return;

    // Deliberately simple energy-threshold VAD rather than anything
    // spectral — cutting live chunks at an actual pause avoids the word-
    // boundary garbling a fixed-time-window cut would cause (whisper
    // decoding half a word at the very edge of a chunk), and speech is
    // reliably louder than the gaps between sentences/phrases even with a
    // fairly conservative threshold like this one.
    static constexpr qreal kLiveSilenceThreshold = 0.02;
    static constexpr qreal kLiveSilenceCutSeconds = 0.35; // how long a pause has to last before it counts as a boundary
    static constexpr qreal kLiveChunkMinSeconds = 1.5;    // never cut a chunk shorter than this, even at a real pause
    static constexpr qreal kLiveChunkMaxSeconds = 8.0;    // cut here regardless of silence — caps how long a run-on sentence can go untranscribed

    const qreal bufferDurationSeconds = double(mono.size()) / double(m_source->format().sampleRate());
    m_liveSilenceSeconds = (peak < kLiveSilenceThreshold) ? (m_liveSilenceSeconds + bufferDurationSeconds) : 0.0;

    const qreal chunkDurationSeconds = double(m_recordedMono.size()) / double(m_source->format().sampleRate());
    const bool hitSilenceBoundary = chunkDurationSeconds >= kLiveChunkMinSeconds
        && m_liveSilenceSeconds >= kLiveSilenceCutSeconds;
    const bool hitHardCap = chunkDurationSeconds >= kLiveChunkMaxSeconds;

    if (hitSilenceBoundary || hitHardCap) {
        const QByteArray wav = flushLiveChunk(m_source->format());
        if (!wav.isEmpty())
            emit liveChunkReady(wav, /*isFinalChunk=*/false);
    }
}

QByteArray VoiceRecorder::buildWavBytes(const QByteArray &pcm16Data, int sampleRate, int channels)
{
    const quint32 dataSize = static_cast<quint32>(pcm16Data.size());
    const quint16 bitsPerSample = 16;
    const quint16 blockAlign = static_cast<quint16>(channels * bitsPerSample / 8);
    const quint32 byteRate = static_cast<quint32>(sampleRate) * blockAlign;

    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);

    out.writeRawData("RIFF", 4);
    out << static_cast<quint32>(36 + dataSize);
    out.writeRawData("WAVE", 4);

    out.writeRawData("fmt ", 4);
    out << static_cast<quint32>(16); // fmt chunk size
    out << static_cast<quint16>(1);  // PCM
    out << static_cast<quint16>(channels);
    out << static_cast<quint32>(sampleRate);
    out << byteRate;
    out << blockAlign;
    out << bitsPerSample;

    out.writeRawData("data", 4);
    out << dataSize;
    out.writeRawData(pcm16Data.constData(), pcm16Data.size());

    return wav;
}

bool VoiceRecorder::writeWavFile(const QString &path, const QByteArray &pcm16Data,
                                  int sampleRate, int channels)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(buildWavBytes(pcm16Data, sampleRate, channels));
    return file.error() == QFile::NoError;
}

QByteArray VoiceRecorder::flushLiveChunk(const QAudioFormat &format)
{
    m_liveSilenceSeconds = 0.0;

    if (m_recordedMono.isEmpty())
        return QByteArray();

    const QVector<float> resampled = resampleLinear(m_recordedMono, format.sampleRate(), kTargetSampleRate);
    m_recordedMono.clear();

    QByteArray pcm16;
    pcm16.resize(resampled.size() * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm16.data());
    for (int i = 0; i < resampled.size(); ++i) {
        const float clamped = std::clamp(resampled[i], -1.0f, 1.0f);
        out[i] = static_cast<qint16>(clamped * 32767.0f);
    }

    return buildWavBytes(pcm16, kTargetSampleRate, kTargetChannels);
}
