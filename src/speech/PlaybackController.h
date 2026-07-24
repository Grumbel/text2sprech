#pragma once

#include <QBuffer>
#include <QHash>
#include <QMediaPlayer>
#include <QObject>
#include <QVector>
#include <memory>

#include "PiperClient.h"
#include "document/Sentence.h"

// Drives sentence-by-sentence playback:
//  - keeps `lookahead` sentences' worth of audio requested ahead of playback
//    so there's minimal gap between one sentence finishing and the next
//    starting (see PROTOCOL.md: requests are pipelined by id).
//  - plays each sentence's WAV via QMediaPlayer from an in-memory QBuffer.
//  - emits sentenceStarted(id) so DocumentView can highlight it.
//
// Not responsible for: talking to the socket directly (that's PiperClient),
// or rendering (that's DocumentView). Kept this way so playback logic is
// unit-testable without a real Piper server -- feed it a fake PiperClient
// signal sequence and assert on sentenceStarted order.
class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(PiperClient *client, QObject *parent = nullptr);

    void loadSentences(const QVector<Sentence> &sentences);
    void play();
    void pause();
    void stop();
    void next();
    void previous();
    void seekToSentence(int sentenceIndex);
    void setSpeed(double speed); // forwarded to PiperClient::setSpeed + QMediaPlayer playback rate
    void setLookahead(int count) { m_lookahead = count; }

    bool isPlaying() const;

signals:
    void sentenceStarted(int sentenceId, int start, int end);
    void playbackFinished();
    void errorOccurred(QString message);

private slots:
    void onAudioReady(int id, int sampleRate, const QByteArray &wav);
    void onSynthesisError(int id, const QString &message);
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);

private:
    void requestLookahead();
    void playCurrentIfReady();
    void advanceToNext();

    PiperClient *m_client; // not owned
    QMediaPlayer m_player;
    std::unique_ptr<QBuffer> m_currentAudioBuffer;

    QVector<Sentence> m_sentences;
    int m_currentIndex = -1;
    double m_speed = 1.0;
    int m_lookahead = 2;

    // audio for sentences we've already requested but not yet played
    QHash<int, QByteArray> m_audioCache; // key: sentence id (== index into m_sentences for v1)
    bool m_waitingForAudio = false;
    bool m_playing = false;
};
