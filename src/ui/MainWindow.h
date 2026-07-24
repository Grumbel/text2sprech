#pragma once

#include <QMainWindow>
#include <QVector>

#include "document/Sentence.h"

class DocumentView;
class PiperClient;
class PlaybackController;
class QComboBox;
class QDoubleSpinBox;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void openFile();
    void pasteFromClipboard();
    void togglePlayPause();
    void onSentenceStarted(int sentenceId, int start, int end);
    void onDocumentClicked(int plainTextOffset);
    void onVoiceListReady(const QStringList &voices, const QString &current);
    void onSpeedChanged(double speed);
    void onPlaybackError(const QString &message);

private:
    void setUpUi();
    void setUpShortcuts();
    void loadDocumentText(const QString &plainOrHtml, bool isHtml);
    void reindexSentences();
    int sentenceIndexForOffset(int offset) const;

    DocumentView *m_documentView;
    PiperClient *m_piperClient;
    PlaybackController *m_playback;

    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QAction *m_playPauseAction;

    QVector<Sentence> m_sentences;
};
