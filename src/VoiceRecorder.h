#pragma once

#include <QObject>
#include <QString>
#include <QMediaCaptureSession>
#include <QAudioInput>
#include <QMediaRecorder>

// Captures microphone audio to a temp WAV file for as long as
// startRecording()/stopRecording() bracket — driven by ChatWidget's mic
// button's pressed()/released() signals for push-to-talk.
//
// This only produces the audio file. Turning it into text is a separate,
// not-yet-implemented step (see ChatWidget::transcribeAudio) — Ollama
// itself has no speech-to-text capability, so that'll need a real engine
// (Whisper) wired in as a follow-up. The plumbing here (record → stop →
// hand off a file path) is what that step plugs into.
class VoiceRecorder : public QObject
{
    Q_OBJECT

public:
    explicit VoiceRecorder(QObject *parent = nullptr);

    void startRecording();
    void stopRecording();

    bool isRecording() const;

signals:
    // filePath is a temp .wav file the receiver owns (delete it once done).
    void recordingFinished(const QString &filePath);
    void recordingFailed(const QString &errorMessage);

private:
    QMediaCaptureSession m_captureSession;
    QAudioInput m_audioInput;
    QMediaRecorder m_recorder;
    QString m_outputPath;
};
