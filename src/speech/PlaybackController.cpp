#include "PlaybackController.h"

#include <QAudioOutput>

PlaybackController::PlaybackController(PiperClient *client, QObject *parent)
    : QObject(parent), m_client(client)
{
    auto *audioOutput = new QAudioOutput(&m_player);
    m_player.setAudioOutput(audioOutput);

    connect(m_client, &PiperClient::audioReady, this, &PlaybackController::onAudioReady);
    connect(m_client, &PiperClient::synthesisError, this, &PlaybackController::onSynthesisError);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &PlaybackController::onMediaStatusChanged);
}

void PlaybackController::loadSentences(const QVector<Sentence> &sentences)
{
    stop();
    m_sentences = sentences;
    m_audioCache.clear();
    m_currentIndex = sentences.isEmpty() ? -1 : 0;
}

bool PlaybackController::isPlaying() const
{
    return m_playing;
}

void PlaybackController::play()
{
    if (m_currentIndex < 0 || m_sentences.isEmpty()) {
        return;
    }
    m_playing = true;
    requestLookahead();
    playCurrentIfReady();
}

void PlaybackController::pause()
{
    m_playing = false;
    m_player.pause();
}

void PlaybackController::stop()
{
    m_playing = false;
    m_waitingForAudio = false;
    m_player.stop();
    m_client->cancelAll();
}

void PlaybackController::next()
{
    if (m_currentIndex < 0) {
        return;
    }
    seekToSentence(m_currentIndex + 1);
}

void PlaybackController::previous()
{
    if (m_currentIndex < 0) {
        return;
    }
    seekToSentence(m_currentIndex - 1);
}

void PlaybackController::seekToSentence(int index)
{
    if (index < 0 || index >= m_sentences.size()) {
        if (index >= m_sentences.size()) {
            m_playing = false;
            emit playbackFinished();
        }
        return;
    }
    bool wasPlaying = m_playing;
    m_player.stop();
    m_currentIndex = index;
    if (wasPlaying) {
        play();
    } else {
        requestLookahead();
    }
}

void PlaybackController::setSpeed(double speed)
{
    m_speed = speed;
    m_client->setSpeed(speed);
    m_player.setPlaybackRate(1.0); // speed is baked into synthesis (length_scale), not sped-up playback,
                                   // to avoid pitch distortion from naive resampling.
}

void PlaybackController::requestLookahead()
{
    if (m_currentIndex < 0) {
        return;
    }
    int end = std::min(m_currentIndex + m_lookahead, m_sentences.size() - 1);
    for (int i = m_currentIndex; i <= end; ++i) {
        const Sentence &s = m_sentences[i];
        if (!m_audioCache.contains(s.id)) {
            m_client->requestSpeak(s.id, s.text);
        }
    }
}

void PlaybackController::playCurrentIfReady()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_sentences.size()) {
        return;
    }
    const Sentence &s = m_sentences[m_currentIndex];
    auto it = m_audioCache.find(s.id);
    if (it == m_audioCache.end()) {
        m_waitingForAudio = true; // onAudioReady will resume playback when it arrives
        return;
    }
    m_waitingForAudio = false;

    m_currentAudioBuffer = std::make_unique<QBuffer>();
    m_currentAudioBuffer->setData(it.value());
    m_currentAudioBuffer->open(QIODevice::ReadOnly);

    m_player.setSourceDevice(m_currentAudioBuffer.get(), QUrl("wav-audio://sentence"));
    m_player.play();

    emit sentenceStarted(s.id, s.start, s.end);
    requestLookahead();
}

void PlaybackController::advanceToNext()
{
    m_audioCache.remove(m_sentences[m_currentIndex].id);
    ++m_currentIndex;
    if (m_currentIndex >= m_sentences.size()) {
        m_playing = false;
        emit playbackFinished();
        return;
    }
    playCurrentIfReady();
}

void PlaybackController::onAudioReady(int id, int /*sampleRate*/, const QByteArray &wav)
{
    m_audioCache.insert(id, wav);
    if (m_playing && m_waitingForAudio && m_currentIndex >= 0
        && m_currentIndex < m_sentences.size() && m_sentences[m_currentIndex].id == id) {
        playCurrentIfReady();
    }
}

void PlaybackController::onSynthesisError(int id, const QString &message)
{
    emit errorOccurred(QStringLiteral("Synthesis failed for sentence %1: %2").arg(id).arg(message));
    // Skip the broken sentence rather than stalling playback entirely.
    if (m_playing && m_currentIndex >= 0 && m_currentIndex < m_sentences.size()
        && m_sentences[m_currentIndex].id == id) {
        advanceToNext();
    }
}

void PlaybackController::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::EndOfMedia && m_playing) {
        advanceToNext();
    }
}
