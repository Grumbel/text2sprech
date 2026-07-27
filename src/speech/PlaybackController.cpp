// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlaybackController.h"

#include <QAudioOutput>
#include <QtGlobal>

PlaybackController::PlaybackController(PiperClient *client, QObject *parent)
    : QObject(parent), m_client(client)
{
    m_audioOutput = new QAudioOutput(&m_player);
    m_audioOutput->setVolume(1.0f);
    m_player.setAudioOutput(m_audioOutput);

    connect(m_client, &PiperClient::audioReady, this, &PlaybackController::onAudioReady);
    connect(m_client, &PiperClient::synthesisError, this, &PlaybackController::onSynthesisError);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, &PlaybackController::onMediaStatusChanged);
    connect(&m_player, &QMediaPlayer::positionChanged, this, &PlaybackController::onPlayerPositionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, &PlaybackController::onPlayerDurationChanged);
}

qint64 PlaybackController::currentPositionMs() const
{
    return m_player.position();
}

qint64 PlaybackController::currentDurationMs() const
{
    return m_player.duration();
}

void PlaybackController::setVolume(float volume)
{
    if (!m_audioOutput) {
        return;
    }
    m_audioOutput->setVolume(qBound(0.0f, volume, 1.0f));
}

float PlaybackController::volume() const
{
    return m_audioOutput ? m_audioOutput->volume() : 1.0f;
}

void PlaybackController::setMuted(bool muted)
{
    if (m_audioOutput) {
        m_audioOutput->setMuted(muted);
    }
}

bool PlaybackController::isMuted() const
{
    return m_audioOutput && m_audioOutput->isMuted();
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
    m_pendingSeekFraction = 0.0;
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

void PlaybackController::seekToSentence(int index, double fractionWithin)
{
    if (index < 0 || index >= m_sentences.size()) {
        if (index >= m_sentences.size()) {
            m_playing = false;
            m_paused = false;
            m_pendingSeekFraction = 0.0;
            emit playbackFinished();
        }
        return;
    }
    bool wasPlaying = m_playing;
    m_paused = false;
    m_pendingSeekFraction = qBound(0.0, fractionWithin, 1.0);
    m_player.stop();
    m_player.setSource(QUrl());
    m_currentAudioBuffer.reset();
    m_currentIndex = index;
    if (wasPlaying) {
        play();
    } else {
        requestLookahead();
        // Not playing: still load this sentence so a later play() can resume
        // mid-segment if the user seeked with a fraction. Audio is requested
        // via lookahead; position applies on the next playCurrentIfReady.
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
    applyPendingSeekIfPossible();

    emit sentenceStarted(s.id, s.start, s.end);
    // Report the post-seek position so the UI does not flash to 0 when
    // starting mid-sentence.
    emit audioPositionChanged(m_player.position(), m_player.duration());
    requestLookahead();
}

void PlaybackController::applyPendingSeekIfPossible()
{
    if (m_pendingSeekFraction <= 0.0) {
        return;
    }
    const qint64 duration = m_player.duration();
    if (duration <= 0) {
        return; // durationChanged will retry
    }
    const qint64 target = static_cast<qint64>(m_pendingSeekFraction * double(duration));
    m_player.setPosition(qBound(qint64(0), target, duration));
    m_pendingSeekFraction = 0.0;
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
        m_pendingSeekFraction = 0.0;
        advanceToNext();
        return;
    }
    // Some backends only report a usable duration after buffering.
    if (status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::LoadedMedia) {
        applyPendingSeekIfPossible();
    }
}

void PlaybackController::onPlayerPositionChanged(qint64 position)
{
    emit audioPositionChanged(position, m_player.duration());
}

void PlaybackController::onPlayerDurationChanged(qint64 duration)
{
    applyPendingSeekIfPossible();
    emit audioPositionChanged(m_player.position(), duration);
}
