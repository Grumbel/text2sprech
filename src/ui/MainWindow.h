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
    // Command-line-configurable startup behavior (see main.cpp for parsing).
    struct Options {
        QString voice;         // empty = use server's default voice
        double speed = 1.0;    // 0.5 - 3.0, matches m_speedSpin's range
        bool autoplay = false; // start playback as soon as a document is loaded
        QString initialFile;   // empty = don't open anything on startup
    };

    explicit MainWindow(const Options &options, QWidget *parent = nullptr);

    // Loads a file given directly (e.g. from a CLI positional argument)
    // rather than through the Open… file dialog.
    void openFilePath(const QString &path);

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

    Options m_options;

    DocumentView *m_documentView;
    PiperClient *m_piperClient;
    PlaybackController *m_playback;

    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QAction *m_playPauseAction;

    QVector<Sentence> m_sentences;
    bool m_initialFileLoaded = false;
};
