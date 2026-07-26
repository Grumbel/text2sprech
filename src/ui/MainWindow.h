#pragma once

#include <QClipboard>
#include <QMainWindow>
#include <QVector>

#include "document/Sentence.h"
#include "DocumentView.h"

class PiperClient;
class PiperServerManager;
class PlaybackController;
class QComboBox;
class QDoubleSpinBox;
class QAction;
class QCloseEvent;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Command-line-configurable startup behavior (see main.cpp for parsing).
    struct Options {
        QString voice;         // empty = use server's default voice
        double speed = 1.0;    // 0.5 - 3.0, matches m_speedSpin's range
        bool autoplay = false; // start playback as soon as a document is loaded
        QString initialFile;   // empty = don't open anything on startup
        // Config-file only for now, no CLI flag. Empty family = leave the
        // widget's default family alone; pointSize <= 0 = leave the
        // default size alone. See DocumentView::setBaseFont().
        QString fontFamily;
        int fontPointSize = -1;
        // View colors from config (invalid QColor = built-in theme default).
        DocumentView::Colors colors;
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
    void onPreparingAudio(int sentenceId);
    void onDocumentClicked(int plainTextOffset);
    void onVoiceListReady(const QStringList &voices, const QString &current);
    void onSpeedChanged(double speed);
    void onPlaybackError(const QString &message);
    // Opens PreferencesDialog seeded from the config-file defaults.
    // Save writes defaults to the config file only; Apply pushes the dialog
    // values into the live session without persisting. This is the only
    // place in the app that writes the config file.
    void openPreferences();

private:
    void setUpUi();
    void setUpShortcuts();
    // Syncs play/pause action icon + text + tooltip with playback state.
    void setPlayingUi(bool playing);
    void loadDocumentText(const QString &plainOrHtml, bool isHtml);
    void reindexSentences();
    int sentenceIndexForOffset(int offset) const;
    void connectToPiperServer(const QString &socketPath);
    void pasteFromSource(QClipboard::Mode mode);
    // Shared by click-to-jump and cursor-key navigation: bounds-checks,
    // highlights immediately (regardless of playback state), updates the
    // position label, and tells playback to seek there.
    void jumpToSentence(int index);
    // Applies whichever startup content source was requested (paste
    // selection/clipboard, or an initial file), once the piper-server
    // connection is actually up. Only ever acts once.
    void loadStartupContentIfNeeded();
    // Persistent "what's happening right now" indicator (left side of the
    // status bar) -- distinct from the transient statusBar()->showMessage()
    // notifications, which briefly cover it and then reveal it again.
    void setActivity(const QString &text);
    // Right-side permanent status bar label: rough total length (words,
    // estimated minutes at the current speed) and current position (words
    // read so far, percent). Recomputed whenever the document, current
    // sentence, or speed changes.
    void refreshInfoLabel();

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
    QLabel *m_activityLabel;  // left/normal status bar area: current activity
    QLabel *m_infoLabel;      // right/permanent status bar area: length + position estimate

    QVector<Sentence> m_sentences;
    bool m_startupContentLoaded = false;

    // Last voice list reported by the server ("ready" message) -- used to
    // populate PreferencesDialog's voice dropdown.
    QStringList m_knownVoices;

    // Rough word-count bookkeeping for the info label. Words, not
    // characters, since "estimated minutes" and "percent through" are both
    // more meaningful in word terms for a TTS reader. Recomputed whenever
    // m_sentences changes (reindexSentences()).
    int m_totalWords = 0;
    QVector<int> m_cumulativeWordsBeforeSentence; // parallel to m_sentences
    int m_currentSentenceIndex = -1; // -1 = no position yet (nothing played/clicked)

    // Startup connection retry: the spawned server needs a moment to import
    // Python/load models before its socket exists, so the first few connect
    // attempts are expected to fail.
    QString m_targetSocketPath;
    int m_connectAttemptsLeft = 0;
};
