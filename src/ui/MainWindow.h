#pragma once

#include <QClipboard>
#include <QMainWindow>
#include <QVector>

#include "document/Sentence.h"

class DocumentView;
class PiperClient;
class PiperServerManager;
class PlaybackController;
class QComboBox;
class QDoubleSpinBox;
class QAction;
class QCloseEvent;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Command-line-configurable startup behavior (see main.cpp for parsing).
    struct Options {
        QString voice;         // empty = use server's default voice
        double speed = 1.0;    // 0.5 - 3.0, matches m_speedSpin's range
        bool autoplay = false; // start playback as soon as a document is loaded
        QString initialFile;   // empty = don't open anything on startup
        // Net DocumentView zoom steps to apply on startup (see
        // DocumentView::increaseTextSize/decreaseTextSize) -- config-file
        // only for now, no CLI flag.
        int textZoomSteps = 0;
        // At most one of these "start with X" sources is used, in this
        // priority order: pasteSelection, pasteClipboard, initialFile.
        bool pasteClipboard = false;
        bool pasteSelection = false;
        // If set, connect to an already-running piper-server at this socket
        // instead of spawning our own -- we never start or stop a server we
        // didn't launch ourselves.
        QString piperSocket;
    };

    explicit MainWindow(const Options &options, QWidget *parent = nullptr);

    // Loads a file given directly (e.g. from a CLI positional argument)
    // rather than through the Open… file dialog.
    void openFilePath(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFile();
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
    void connectToPiperServer(const QString &socketPath);
    void pasteFromSource(QClipboard::Mode mode);
    // Applies whichever startup content source was requested (paste
    // selection/clipboard, or an initial file), once the piper-server
    // connection is actually up. Only ever acts once.
    void loadStartupContentIfNeeded();

    Options m_options;

    DocumentView *m_documentView;
    PiperClient *m_piperClient;
    // Null when m_options.piperSocket was given -- we're attaching to
    // someone else's server and must not manage its lifecycle.
    PiperServerManager *m_piperServerManager = nullptr;
    PlaybackController *m_playback;

    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QAction *m_playPauseAction;

    QVector<Sentence> m_sentences;
    bool m_startupContentLoaded = false;

    // Startup connection retry: the spawned server needs a moment to import
    // Python/load models before its socket exists, so the first few connect
    // attempts are expected to fail.
    QString m_targetSocketPath;
    int m_connectAttemptsLeft = 0;
};
