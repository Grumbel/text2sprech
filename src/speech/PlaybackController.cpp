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

    // Resume mid-sentence after pause(). Reloading the WAV (playCurrentIfReady)
    // would restart the segment from the beginning -- that's what stop() is for.
    if (m_paused) {
        m_paused = false;
        m_player.play();
        return;
    }

    playCurrentIfReady();
}

void PlaybackController::pause()
{
    if (!m_playing && m_player.playbackState() != QMediaPlayer::PlayingState) {
        return;
    }
    m_playing = false;
    // Only mark resumable if audio is actually flowing; pausing while still
    // waiting for synthesis should just cancel the "playing" intent.
    if (m_player.playbackState() == QMediaPlayer::PlayingState
        || m_player.playbackState() == QMediaPlayer::PausedState) {
        m_paused = true;
        m_player.pause();
    } else {
        m_paused = false;
    }
}

void PlaybackController::stop()
{
    m_playing = false;
    m_paused = false;
    m_waitingForAudio = false;
    m_player.stop();
    // Drop the current media so the next play() reloads from the start.
    m_player.setSource(QUrl());
    m_currentAudioBuffer.reset();
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
            m_paused = false;
            emit playbackFinished();
        }
        return;
    }
    bool wasPlaying = m_playing;
    // Seeking always starts the target sentence from the beginning.
    m_paused = false;
    m_player.stop();
    m_player.setSource(QUrl());
    m_currentAudioBuffer.reset();
    m_currentIndex = index;
    if (wasPlaying) {
        play();
    } else {
        requestLookahead();
    }
}

void PlaybackController::setSpeed(double speed)
{
    // Same cancel/clear/re-request pattern as setVoice: length_scale is
    // baked into the WAV, so cached lookahead audio would keep the old
    // speed. The sentence already playing is left alone.
    m_client->cancelAll();
    m_client->setSpeed(speed);
    m_player.setPlaybackRate(1.0); // never resample; avoid pitch distortion
    m_audioCache.clear();
    if (m_playing) {
        requestLookahead();
    }
}

void PlaybackController::setVoice(const QString &voice)
{
    // Order matters: cancel server-side in-flight synthesis for the *old*
    // voice first (asyncio.CancelledError there means no stale audio
    // message ever arrives for those requests), then update the session's
    // voice, then drop anything we already cached under the old voice, and
    // finally re-request what we still need so it comes back in the new
    // voice.
    m_client->cancelAll();
    m_client->setVoice(voice);
    m_audioCache.clear();
    if (m_playing) {
        requestLookahead();
    }
}

void PlaybackController::requestLookahead()
{
    if (m_currentIndex < 0) {
        return;
    }
    int end = std::min(m_currentIndex + m_lookahead, static_cast<int>(m_sentences.size()) - 1);
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
        m_paused = false;
        emit preparingAudio(s.id);
        return;
    }
    m_waitingForAudio = false;
    m_paused = false;

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
    m_paused = false;
    if (m_currentIndex >= m_sentences.size()) {
        m_playing = false;
        m_player.stop();
        m_player.setSource(QUrl());
        m_currentAudioBuffer.reset();
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
