// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ExportAudioDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QtEndian>
#include <QVBoxLayout>

#include <cstring>

#include "speech/PiperClient.h"

namespace {

struct WavFormat {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int dataOffset = 0; // start of PCM samples inside the buffer
    int dataSize = 0;
};

bool parseWav(const QByteArray &wav, WavFormat *out)
{
    if (!out || wav.size() < 44) {
        return false;
    }
    if (!(wav[0] == 'R' && wav[1] == 'I' && wav[2] == 'F' && wav[3] == 'F'
          && wav[8] == 'W' && wav[9] == 'A' && wav[10] == 'V' && wav[11] == 'E')) {
        return false;
    }

    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int dataOffset = -1;
    int dataSize = 0;

    int offset = 12;
    while (offset + 8 <= wav.size()) {
        const char *chunkId = wav.constData() + offset;
        const quint32 chunkSize = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(wav.constData() + offset + 4));
        const int dataStart = offset + 8;
        if (chunkId[0] == 'f' && chunkId[1] == 'm' && chunkId[2] == 't' && chunkId[3] == ' ') {
            if (dataStart + 16 <= wav.size()) {
                channels = qFromLittleEndian<quint16>(
                    reinterpret_cast<const uchar *>(wav.constData() + dataStart + 2));
                sampleRate = int(qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar *>(wav.constData() + dataStart + 4)));
                bitsPerSample = qFromLittleEndian<quint16>(
                    reinterpret_cast<const uchar *>(wav.constData() + dataStart + 14));
            }
        } else if (chunkId[0] == 'd' && chunkId[1] == 'a' && chunkId[2] == 't'
                   && chunkId[3] == 'a') {
            dataOffset = dataStart;
            dataSize = int(chunkSize);
            if (dataOffset + dataSize > wav.size()) {
                dataSize = wav.size() - dataOffset;
            }
            break;
        }
        offset = dataStart + int(chunkSize) + (int(chunkSize) & 1);
    }

    if (sampleRate <= 0 || channels <= 0 || bitsPerSample <= 0 || dataOffset < 0 || dataSize < 0) {
        return false;
    }
    out->sampleRate = sampleRate;
    out->channels = channels;
    out->bitsPerSample = bitsPerSample;
    out->dataOffset = dataOffset;
    out->dataSize = dataSize;
    return true;
}

// Minimal 44-byte PCM header; sizes patched when the export finishes.
QByteArray makeWavHeader(int sampleRate, int channels, int bitsPerSample, quint32 dataBytes)
{
    const quint16 blockAlign = quint16(channels * (bitsPerSample / 8));
    const quint32 byteRate = quint32(sampleRate) * blockAlign;
    const quint32 riffSize = 36 + dataBytes;

    QByteArray header(44, '\0');
    auto *p = reinterpret_cast<uchar *>(header.data());
    memcpy(p, "RIFF", 4);
    qToLittleEndian(riffSize, p + 4);
    memcpy(p + 8, "WAVE", 4);
    memcpy(p + 12, "fmt ", 4);
    qToLittleEndian(quint32(16), p + 16); // PCM fmt chunk size
    qToLittleEndian(quint16(1), p + 20);  // PCM
    qToLittleEndian(quint16(channels), p + 22);
    qToLittleEndian(quint32(sampleRate), p + 24);
    qToLittleEndian(byteRate, p + 28);
    qToLittleEndian(blockAlign, p + 32);
    qToLittleEndian(quint16(bitsPerSample), p + 34);
    memcpy(p + 36, "data", 4);
    qToLittleEndian(dataBytes, p + 40);
    return header;
}

} // namespace

ExportAudioDialog::ExportAudioDialog(PiperClient *client, const QVector<Sentence> &sentences,
                                     int startIndex, const QString &voice, double speed,
                                     const QStringList &voices, QWidget *parent)
    : QDialog(parent)
    , m_client(client)
    , m_allSentences(sentences)
    , m_documentStartIndex(qBound(0, startIndex, qMax(0, sentences.size() - 1)))
{
    setWindowTitle(tr("Export Audio"));
    setModal(true);
    setMinimumWidth(520);

    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->addItem(tr("Entire document"), int(RangeMode::EntireDocument));
    m_rangeCombo->addItem(tr("From current sentence to end"), int(RangeMode::FromCurrent));
    if (sentences.isEmpty()) {
        m_rangeCombo->setEnabled(false);
    }

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Choose an output .wav file…"));
    auto *browseBtn = new QPushButton(tr("Browse…"), this);
    connect(browseBtn, &QPushButton::clicked, this, &ExportAudioDialog::browseOutputPath);
    auto *pathRow = new QWidget(this);
    auto *pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    pathLayout->addWidget(m_pathEdit, 1);
    pathLayout->addWidget(browseBtn);

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->addItems(voices);
    if (!voice.isEmpty()) {
        int idx = m_voiceCombo->findText(voice);
        if (idx < 0) {
            m_voiceCombo->addItem(voice);
            idx = m_voiceCombo->findText(voice);
        }
        if (idx >= 0) {
            m_voiceCombo->setCurrentIndex(idx);
        }
    }

    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setRange(0.5, 3.0);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->setDecimals(2);
    m_speedSpin->setSuffix(tr("×"));
    m_speedSpin->setValue(speed >= 0.5 && speed <= 3.0 ? speed : 1.0);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, qMax(1, sentences.size()));
    m_progress->setValue(0);
    m_progress->setTextVisible(true);

    m_statusLabel = new QLabel(tr("Ready."), this);
    m_statusLabel->setWordWrap(true);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(tr("Range:"), m_rangeCombo);
    form->addRow(tr("Output file:"), pathRow);
    form->addRow(tr("Voice:"), m_voiceCombo);
    form->addRow(tr("Speed:"), m_speedSpin);

    m_startBtn = new QPushButton(tr("Start export"), this);
    m_startBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setEnabled(false);
    m_closeBtn = new QPushButton(tr("Close"), this);
    connect(m_startBtn, &QPushButton::clicked, this, &ExportAudioDialog::startExport);
    connect(m_cancelBtn, &QPushButton::clicked, this, &ExportAudioDialog::cancelExport);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_startBtn);
    buttons->addWidget(m_cancelBtn);
    buttons->addStretch(1);
    buttons->addWidget(m_closeBtn);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_progress);
    layout->addWidget(m_statusLabel);
    layout->addLayout(buttons);

    if (m_client) {
        connect(m_client, &PiperClient::audioReady, this, &ExportAudioDialog::onAudioReady);
        connect(m_client, &PiperClient::synthesisError, this, &ExportAudioDialog::onSynthesisError);
    }

    if (sentences.isEmpty()) {
        setStatus(tr("No sentences to export. Open or paste a document first."));
        m_startBtn->setEnabled(false);
    }
}

ExportAudioDialog::~ExportAudioDialog()
{
    abortInFlight();
}

void ExportAudioDialog::closeEvent(QCloseEvent *event)
{
    if (m_running) {
        cancelExport();
    }
    QDialog::closeEvent(event);
}

void ExportAudioDialog::reject()
{
    if (m_running) {
        cancelExport();
    }
    QDialog::reject();
}

void ExportAudioDialog::browseOutputPath()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Audio"), m_pathEdit->text().isEmpty() ? QStringLiteral("export.wav")
                                                               : m_pathEdit->text(),
        tr("WAV audio (*.wav);;All files (*)"));
    if (!path.isEmpty()) {
        m_pathEdit->setText(path);
    }
}

void ExportAudioDialog::setBusy(bool busy)
{
    m_running = busy;
    m_rangeCombo->setEnabled(!busy && !m_allSentences.isEmpty());
    m_pathEdit->setEnabled(!busy);
    m_voiceCombo->setEnabled(!busy);
    m_speedSpin->setEnabled(!busy);
    m_startBtn->setEnabled(!busy && !m_allSentences.isEmpty());
    m_cancelBtn->setEnabled(busy);
    m_closeBtn->setEnabled(!busy);
}

void ExportAudioDialog::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

int ExportAudioDialog::requestIdForLocalIndex(int localIndex) const
{
    return kRequestIdBase + localIndex;
}

int ExportAudioDialog::localIndexForRequestId(int requestId) const
{
    if (requestId < kRequestIdBase) {
        return -1;
    }
    return requestId - kRequestIdBase;
}

void ExportAudioDialog::startExport()
{
    if (m_running || !m_client || m_allSentences.isEmpty()) {
        return;
    }
    if (!m_client->isConnected()) {
        QMessageBox::warning(this, tr("Export Audio"),
                             tr("Not connected to the Piper server."));
        return;
    }

    m_outputPath = m_pathEdit->text().trimmed();
    if (m_outputPath.isEmpty()) {
        QMessageBox::warning(this, tr("Export Audio"), tr("Choose an output file first."));
        return;
    }
    if (!m_outputPath.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)) {
        m_outputPath += QLatin1String(".wav");
        m_pathEdit->setText(m_outputPath);
    }

    const auto mode = RangeMode(m_rangeCombo->currentData().toInt());
    m_rangeBegin = (mode == RangeMode::FromCurrent) ? m_documentStartIndex : 0;
    m_rangeCount = m_allSentences.size() - m_rangeBegin;
    if (m_rangeCount <= 0) {
        QMessageBox::warning(this, tr("Export Audio"), tr("Nothing to export in that range."));
        return;
    }

    // Apply voice/speed for this export (session server state; caller may
    // restore afterward if needed — export is a deliberate user action).
    const QString voice = m_voiceCombo->currentText();
    if (!voice.isEmpty()) {
        m_client->setVoice(voice);
    }
    m_client->setSpeed(m_speedSpin->value());

    m_nextToRequest = 0;
    m_nextToWrite = 0;
    m_inFlight = 0;
    m_completed = 0;
    m_pendingWav.clear();
    m_headerWritten = false;
    m_sampleRate = 0;
    m_channels = 0;
    m_bitsPerSample = 0;
    m_dataBytes = 0;

    delete m_outFile;
    m_outFile = new QFile(m_outputPath, this);
    if (!m_outFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = m_outFile->errorString();
        delete m_outFile;
        m_outFile = nullptr;
        QMessageBox::warning(this, tr("Export Audio"),
                             tr("Could not open output file:\n%1").arg(err));
        return;
    }

    m_progress->setRange(0, m_rangeCount);
    m_progress->setValue(0);
    setBusy(true);
    setStatus(tr("Starting export of %1 sentence(s)…").arg(m_rangeCount));
    pumpRequests();
}

void ExportAudioDialog::cancelExport()
{
    if (!m_running) {
        return;
    }
    abortInFlight();
    if (m_outFile) {
        m_outFile->close();
        m_outFile->remove(); // incomplete file
        delete m_outFile;
        m_outFile = nullptr;
    }
    setBusy(false);
    setStatus(tr("Export cancelled."));
    m_progress->setValue(0);
}

void ExportAudioDialog::abortInFlight()
{
    if (m_client && m_running) {
        // Cancel only our request-id range when possible; stop_all is the
        // protocol primitive and also clears any stray live requests — the
        // main window already stopped playback before opening this dialog.
        m_client->cancelAll();
    }
    m_inFlight = 0;
    m_pendingWav.clear();
    m_running = false;
}

void ExportAudioDialog::pumpRequests()
{
    if (!m_running || !m_client) {
        return;
    }
    while (m_inFlight < kMaxInFlight && m_nextToRequest < m_rangeCount) {
        const int local = m_nextToRequest++;
        const int docIndex = m_rangeBegin + local;
        const Sentence &s = m_allSentences[docIndex];
        m_client->requestSpeak(requestIdForLocalIndex(local), s.text);
        ++m_inFlight;
    }
    if (m_completed < m_rangeCount) {
        setStatus(tr("Synthesizing… %1 / %2 sentences (%3 in flight)")
                      .arg(m_completed)
                      .arg(m_rangeCount)
                      .arg(m_inFlight));
    }
}

void ExportAudioDialog::onAudioReady(int id, int /*sampleRate*/, const QByteArray &wavData)
{
    const int local = localIndexForRequestId(id);
    if (local < 0 || local >= m_rangeCount || !m_running) {
        return; // not ours (live playback id or stale)
    }
    --m_inFlight;
    m_pendingWav.insert(local, wavData);

    // Write any contiguous prefix starting at m_nextToWrite.
    while (m_pendingWav.contains(m_nextToWrite)) {
        const QByteArray wav = m_pendingWav.take(m_nextToWrite);
        WavFormat fmt;
        if (!parseWav(wav, &fmt)) {
            finishFailure(tr("Invalid WAV data for sentence %1.").arg(m_nextToWrite + 1));
            return;
        }
        if (!m_headerWritten) {
            m_sampleRate = fmt.sampleRate;
            m_channels = fmt.channels;
            m_bitsPerSample = fmt.bitsPerSample;
            // Placeholder header; sizes fixed in finishSuccess().
            const QByteArray header =
                makeWavHeader(m_sampleRate, m_channels, m_bitsPerSample, 0);
            if (m_outFile->write(header) != header.size()) {
                finishFailure(tr("Failed writing WAV header: %1").arg(m_outFile->errorString()));
                return;
            }
            m_headerWritten = true;
        } else if (fmt.sampleRate != m_sampleRate || fmt.channels != m_channels
                   || fmt.bitsPerSample != m_bitsPerSample) {
            finishFailure(tr("Sample format changed mid-export (sentence %1). "
                             "All sentences must share the same voice/format.")
                              .arg(m_nextToWrite + 1));
            return;
        }

        const char *pcm = wav.constData() + fmt.dataOffset;
        const qint64 written = m_outFile->write(pcm, fmt.dataSize);
        if (written != fmt.dataSize) {
            finishFailure(tr("Failed writing audio data: %1").arg(m_outFile->errorString()));
            return;
        }
        m_dataBytes += quint32(fmt.dataSize);
        ++m_nextToWrite;
        ++m_completed;
        m_progress->setValue(m_completed);
    }

    if (m_completed >= m_rangeCount) {
        finishSuccess();
        return;
    }
    pumpRequests();
}

void ExportAudioDialog::onSynthesisError(int id, const QString &message)
{
    const int local = localIndexForRequestId(id);
    if (local < 0 || !m_running) {
        return;
    }
    --m_inFlight;
    finishFailure(tr("Synthesis failed for sentence %1: %2").arg(local + 1).arg(message));
}

void ExportAudioDialog::finishSuccess()
{
    if (m_outFile && m_headerWritten) {
        // Patch RIFF and data chunk sizes.
        const QByteArray header =
            makeWavHeader(m_sampleRate, m_channels, m_bitsPerSample, m_dataBytes);
        if (!m_outFile->seek(0) || m_outFile->write(header) != header.size()) {
            finishFailure(tr("Failed finalizing WAV header: %1").arg(m_outFile->errorString()));
            return;
        }
        m_outFile->close();
        delete m_outFile;
        m_outFile = nullptr;
    }

    setBusy(false);
    const QString name = QFileInfo(m_outputPath).fileName();
    setStatus(tr("Done — saved %1 (%2 sentences, %3 KB).")
                  .arg(name)
                  .arg(m_rangeCount)
                  .arg(qMax(1, int((m_dataBytes + 44) / 1024))));
    m_progress->setValue(m_rangeCount);
    QMessageBox::information(this, tr("Export Audio"),
                             tr("Audio exported to:\n%1").arg(m_outputPath));
}

void ExportAudioDialog::finishFailure(const QString &message)
{
    abortInFlight();
    if (m_outFile) {
        m_outFile->close();
        m_outFile->remove();
        delete m_outFile;
        m_outFile = nullptr;
    }
    setBusy(false);
    setStatus(message);
    QMessageBox::warning(this, tr("Export Audio"), message);
}
