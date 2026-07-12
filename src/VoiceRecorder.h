#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QAudioSource>
#include <QAudioFormat>
#include <QAudioDevice>

class QIODevice;

// Captures microphone audio for as long as startRecording()/stopRecording()
// bracket — driven by ChatWidget's mic button's pressed()/released()
// signals for push-to-talk — and writes it out as a 16 kHz mono 16-bit
// WAV file, which is what whisper.cpp's own WAV reader hard-requires.
//
// This deliberately does NOT use QMediaRecorder/QMediaCaptureSession (the
// "normal" Qt Multimedia way to record to a file). That was tried first and
// went through two failure modes in a row on this app's GStreamer backend:
// first it wrote files at whatever sample rate the device's native format
// happened to be (rejected by whisper-cli: "must be 16 kHz"), and after
// forcing an explicit sample rate/channel count on the recorder, it started
// producing files whisper-cli couldn't even parse as WAV at all ("failed to
// read WAV file"). Rather than keep fighting an opaque encoder pipeline,
// this instead pulls raw PCM straight from a QAudioSource — the same
// mechanism that already reliably drove the live level meter — decodes it
// to mono float samples, resamples that to exactly 16 kHz with simple
// linear interpolation, and writes a plain PCM WAV file by hand. That also
// means the level meter and the recorded file are now, for the first time,
// literally the same captured audio rather than two independent streams
// that could disagree with each other.
class VoiceRecorder : public QObject
{
    Q_OBJECT

public:
    explicit VoiceRecorder(QObject *parent = nullptr);

    // liveMode true additionally slices the recording into short chunks as
    // it goes (see onCaptureReadyRead()'s silence-detection logic) and
    // reports each one via liveChunkReady() instead of just accumulating
    // silently until stopRecording() — used for live transcription (see
    // ChatWidget). false (the default) is this class's original push-to-
    // talk-only behavior: nothing is reported until stopRecording() hands
    // back the whole recording as one file.
    void startRecording(bool liveMode = false);
    void stopRecording();

    bool isRecording() const;

    // Re-reads "voice/audioInputDeviceId" from QSettings and applies it —
    // an empty/unmatched saved id falls back to
    // QMediaDevices::defaultAudioInput(). Called once at construction and
    // again live whenever SettingsDialog's microphone combo changes (see
    // ChatWidget::refreshAudioInputDevice()).
    void refreshAudioInputDevice();

    // Re-reads "voice/meterSmoothingPercent" from QSettings (0..100, default
    // 50) and re-derives m_attackRate/m_releaseRate from it — see their own
    // comment for the mapping. Called once at construction and again live
    // whenever SettingsDialog's meter-smoothing slider changes (see
    // ChatWidget::refreshMeterSmoothing()).
    void refreshMeterSmoothing();

signals:
    // filePath is a temp .wav file the receiver owns (delete it once done).
    void recordingFinished(const QString &filePath);
    void recordingFailed(const QString &errorMessage);

    // Live input level while recording, normalized to 0.0..1.0 (peak
    // amplitude, VU-meter-smoothed — see onCaptureReadyRead()) — computed
    // from the exact same buffers that get written to the file, so this is
    // now a reliable proxy for "the recording is actually capturing
    // something," not just "the device itself is live." Emits exactly one
    // 0.0 when capture stops, so a listener never gets stuck on a stale level.
    void audioLevelChanged(qreal level);

    // Live-mode only (see startRecording()) — one WAV-encoded chunk, ready
    // to hand to WhisperManager::transcribeChunkLive(). isFinalChunk is set
    // on the one emitted from stopRecording() covering whatever tail audio
    // hadn't hit a chunk boundary yet (possibly empty, if the last boundary
    // landed exactly at the button release) — never on the others.
    void liveChunkReady(const QByteArray &wavData, bool isFinalChunk);

private slots:
    void onCaptureReadyRead();

private:
    // Directory to write the next recording's temp file into — /dev/shm if
    // it exists and is writable, else QStandardPaths::TempLocation.
    static QString pickOutputDir();

    // Builds a minimal 16-bit PCM WAV file's bytes (44-byte header + raw
    // samples) in memory — Qt has no built-in WAV writer, and this format
    // is simple enough not to need one. Shared by writeWavFile() (push-to-
    // talk's whole-recording file) and flushLiveChunk() (live mode's
    // in-memory chunks, which have nothing to gain from a disk round trip
    // for what's usually a couple seconds of audio).
    static QByteArray buildWavBytes(const QByteArray &pcm16Data, int sampleRate, int channels);
    static bool writeWavFile(const QString &path, const QByteArray &pcm16Data,
                              int sampleRate, int channels);
    // Resamples whatever's accumulated in m_recordedMono since the last
    // flush to 16 kHz mono 16-bit PCM and returns it as WAV bytes, clearing
    // m_recordedMono and the silence-tracking state for the next chunk.
    // Empty (zero-length QByteArray) if nothing had accumulated — still
    // meaningful for isFinalChunk=true (an empty final chunk just means the
    // last real chunk already covered everything up to the button release).
    QByteArray flushLiveChunk(const QAudioFormat &format);

    QAudioDevice m_device;
    QAudioSource *m_source = nullptr;
    QIODevice *m_ioDevice = nullptr; // owned by m_source, not by this class

    // Mono float samples at the device's own native rate. In push-to-talk
    // mode (m_liveMode == false), accumulated for the whole recording and
    // only resampled to 16 kHz once, in stopRecording(). In live mode,
    // accumulated only since the *last chunk boundary* — flushLiveChunk()
    // clears it every time a chunk is cut, so it never holds more than one
    // chunk's worth of audio at once.
    QVector<float> m_recordedMono;
    bool m_recording = false;
    bool m_liveMode = false;

    // How long (in seconds, native sample rate) the most recent run of
    // near-silent audio has lasted — reset to 0 the moment a buffer isn't
    // quiet, incremented by that buffer's duration when it is. Live mode
    // cuts a chunk once this crosses kLiveSilenceCutSeconds *and* the
    // chunk's already at least kLiveChunkMinSeconds long (so a quiet
    // opening beat doesn't itself trigger a near-empty chunk), or
    // unconditionally once the chunk hits kLiveChunkMaxSeconds regardless
    // of silence (so a long run-on sentence with no pause still gets
    // flushed periodically instead of never transcribing until release).
    qreal m_liveSilenceSeconds = 0.0;
    // Whether any buffer since the last flush actually cleared the silence
    // threshold — a chunk boundary only really flushes (and gets sent off
    // for transcription at all) if this is true. Without it, a quiet beat
    // right after the mic button is pressed (before the person has
    // actually started talking) can by itself already satisfy both the min
    // length and the silence-cut duration, flushing an all-silence "chunk"
    // that whisper then transcribes as the literal text "[BLANK_AUDIO]"
    // instead of just... nothing. Reset in flushLiveChunk(); an all-silence
    // stretch that gets skipped this way just keeps accumulating (silently
    // discarding old silent samples isn't needed — see onCaptureReadyRead())
    // until real speech finally shows up.
    bool m_liveHadSpeechSinceFlush = false;

    // Same ballistics as before: fast rise toward a louder peak, slow decay
    // back down, so the meter has visible inertia instead of jittering with
    // every buffer. Reset at the start of every recording.
    qreal m_smoothedLevel = 0.0;

    // How much of the gap to the new peak gets closed per buffer — 1.0
    // would snap instantly (no smoothing at all), smaller values ease
    // toward it more gradually. Attack (rising) is always kept faster than
    // release (falling) so the meter still feels responsive to a sudden
    // louder sound; both are scaled together by the same Settings slider —
    // see refreshMeterSmoothing(). Defaults (0.6/0.15) match this feature's
    // original hardcoded values, at the slider's default (50%) position.
    qreal m_attackRate = 0.6;
    qreal m_releaseRate = 0.15;
};
