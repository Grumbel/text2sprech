// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QHash>
#include <QString>
#include <QVector>

#include "document/Sentence.h"

class PiperClient;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QFile;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

// Modal dialog: synthesize sentences via the shared PiperClient and write a
// single concatenated WAV. Long documents take a while, so progress and
// cancel are first-class. Caller must stop live playback and suspend
// PlaybackController before exec() so speak responses are not dual-consumed.
class ExportAudioDialog : public QDialog {
    Q_OBJECT
public:
    // sentences: document order; startIndex is used when range is "from here".
    // voice/speed are session defaults (user can still change speed in the dialog).
    ExportAudioDialog(PiperClient *client, const QVector<Sentence> &sentences, int startIndex,
                      const QString &voice, double speed, const QStringList &voices,
                      QWidget *parent = nullptr);
    ~ExportAudioDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private slots:
    void browseOutputPath();
    void startExport();
    void cancelExport();
    void onAudioReady(int id, int sampleRate, const QByteArray &wavData);
    void onSynthesisError(int id, const QString &message);

private:
    enum class RangeMode { EntireDocument = 0, FromCurrent = 1 };

    void setBusy(bool busy);
    void setStatus(const QString &text);
    void pumpRequests();
    void finishSuccess();
    void finishFailure(const QString &message);
    void abortInFlight();
    // Request ids are offset so they never collide with live playback sentence ids.
    int requestIdForLocalIndex(int localIndex) const;
    int localIndexForRequestId(int requestId) const;

    PiperClient *m_client = nullptr; // not owned
    QVector<Sentence> m_allSentences;
    int m_documentStartIndex = 0;

    QComboBox *m_rangeCombo = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QComboBox *m_voiceCombo = nullptr;
    QDoubleSpinBox *m_speedSpin = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;

    bool m_running = false;
    int m_rangeBegin = 0; // index into m_allSentences
    int m_rangeCount = 0;
    int m_nextToRequest = 0;  // local 0..rangeCount
    int m_nextToWrite = 0;    // local index of next sequential write
    int m_inFlight = 0;
    int m_completed = 0;
    static constexpr int kMaxInFlight = 3;
    static constexpr int kRequestIdBase = 10'000'000;

    QString m_outputPath;
    QHash<int, QByteArray> m_pendingWav; // local index → full WAV
    // Streaming write state after the first sentence lands.
    QFile *m_outFile = nullptr;
    bool m_headerWritten = false;
    int m_sampleRate = 0;
    int m_channels = 0;
    int m_bitsPerSample = 0;
    quint32 m_dataBytes = 0;
};
