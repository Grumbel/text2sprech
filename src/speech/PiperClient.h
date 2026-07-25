#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>

// Client for the text2sprech<->piper-server protocol described in PROTOCOL.md:
// 4-byte big-endian length prefix + UTF-8 JSON, over a Unix domain socket
// (QLocalSocket maps to AF_UNIX on Linux/macOS).
//
// This class only speaks the protocol; it doesn't own playback ordering or
// the sentence queue -- that's PlaybackController's job. Keeping the two
// separate means the wire protocol can be tested (see tests/) without a
// QApplication event loop pulling in Qt Multimedia.
class PiperClient : public QObject {
    Q_OBJECT
public:
    explicit PiperClient(QObject *parent = nullptr);

    void connectToServer(const QString &socketPath);
    bool isConnected() const;

    void requestSpeak(int id, const QString &text);
    void cancel(int id);
    void cancelAll();
    void setVoice(const QString &voice);
    void setSpeed(double speed);
    void requestVoiceList();

signals:
    void ready(const QStringList &voices, const QString &currentVoice);
    // sampleRate in Hz, wavData is a complete RIFF/WAV byte buffer for one sentence
    void audioReady(int id, int sampleRate, const QByteArray &wavData);
    void synthesisError(int id, const QString &message);
    void voiceListReceived(const QStringList &voices);
    void connectionError(const QString &message);
    void disconnected();

private slots:
    void onReadyRead();
    void onSocketError(QLocalSocket::LocalSocketError error);
    void onDisconnected();

private:
    void sendMessage(const QJsonObject &msg);
    void dispatchMessage(const QJsonObject &msg);
    // Tries to pull one complete length-prefixed frame out of m_recvBuffer.
    // Returns std::nullopt if a full frame isn't buffered yet.
    std::optional<QByteArray> tryExtractFrame();

    QLocalSocket m_socket;
    QByteArray m_recvBuffer;
    quint32 m_pendingFrameLength = 0;
    bool m_havePendingLength = false;
};
