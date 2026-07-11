#include "VoiceRecorder.h"

#include <QStandardPaths>
#include <QUuid>
#include <QUrl>
#include <QMediaFormat>

VoiceRecorder::VoiceRecorder(QObject *parent) : QObject(parent)
{
    m_captureSession.setAudioInput(&m_audioInput);
    m_captureSession.setRecorder(&m_recorder);

    QMediaFormat format;
    format.setFileFormat(QMediaFormat::Wave);
    m_recorder.setMediaFormat(format);

    connect(&m_recorder, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString &errorString) {
                emit recordingFailed(errorString);
            });

    // QMediaRecorder::stop() is asynchronous — the file isn't necessarily
    // flushed until the state actually reaches Stopped, which is what this
    // reacts to rather than assuming stop() itself is synchronous.
    // m_outputPath is only non-empty once startRecording() has run, so this
    // can't fire spuriously off the recorder's own initial Stopped state.
    connect(&m_recorder, &QMediaRecorder::recorderStateChanged, this,
            [this](QMediaRecorder::RecorderState state) {
                if (state == QMediaRecorder::StoppedState && !m_outputPath.isEmpty())
                    emit recordingFinished(m_outputPath);
            });
}

void VoiceRecorder::startRecording()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_outputPath = dir + "/ollama-tray-voice-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".wav";
    m_recorder.setOutputLocation(QUrl::fromLocalFile(m_outputPath));
    m_recorder.record();
}

void VoiceRecorder::stopRecording()
{
    if (m_recorder.recorderState() != QMediaRecorder::StoppedState)
        m_recorder.stop();
}

bool VoiceRecorder::isRecording() const
{
    return m_recorder.recorderState() == QMediaRecorder::RecordingState;
}
