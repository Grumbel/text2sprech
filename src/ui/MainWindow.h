// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QClipboard>
#include <QMainWindow>
#include <QVector>

#include "document/Sentence.h"
#include "DocumentView.h"

class PiperClient;
class PiperServerManager;
class PlaybackController;
struct PiperServerInfo;
class QComboBox;
class QDoubleSpinBox;
class QAction;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QSlider;
class QToolButton;
class QWidget;

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
        // Extra text margin size (px) when the toolbar Margins toggle is on.
        // Config-file only; the toggle itself is session state.
        int extraMarginSize = DocumentView::kDefaultExtraMarginSize;
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
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void openFile();
    void togglePlayPause();
    void onSentenceStarted(int sentenceId, int start, int end);
    void onPreparingAudio(int sentenceId);
    void onDocumentClicked(int plainTextOffset);
    void onServerReady(const PiperServerInfo &info);
    void onSpeedChanged(double speed);
    void onPlaybackError(const QString &message);
    // Opens PreferencesDialog seeded from the config-file defaults.
    // Save writes those defaults; recent-files and per-document positions
    // are written separately under their own groups.
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
    void jumpToSentence(int index, double fractionWithin = 0.0);
    // Paints fine-grained progress inside the current sentence from
    // m_currentSentenceIndex + m_intraSentenceFraction.
    void applyPositionHighlight(DocumentView::HighlightState state);
    // Applies whichever startup content source was requested (paste
    // selection/clipboard, or an initial file), once the piper-server
    // connection is actually up. Only ever acts once.
    void loadStartupContentIfNeeded();
    // Placeholder copy shown until the user opens or pastes a document.
    void showWelcomeMessage();
    // Persistent "what's happening right now" indicator (left side of the
    // status bar) -- distinct from the transient statusBar()->showMessage()
    // notifications, which briefly cover it and then reveal it again.
    // A colored status dot sits beside the text (green = ready/playing,
    // amber = working, gray = idle, red = error).
    enum class ActivityKind {
        Ready,    // connected / idle and usable
        Active,   // playing or preparing audio
        Idle,     // paused / stopped / finished / connecting
        Warning,  // no audio / degraded
        Error,    // connection or synthesis failure
    };
    void setActivity(const QString &text, ActivityKind kind = ActivityKind::Idle);
    // Updates the player-bar progress strip: seek slider, elapsed/remaining
    // time estimates, and sentence counter. Recomputed when the document,
    // current sentence, or speed changes.
    void refreshProgress();
    // Maps a progress-unit offset to the sentence that contains it.
    int sentenceIndexForProgressUnit(int unit) const;
    // Resolve a seek-bar unit to (sentence index, 0..1 fraction within it).
    void progressUnitToPosition(int unit, int *sentenceIndex, double *fraction) const;
    // Preview position from the seek bar (labels + fine highlight) without seeking audio.
    void previewProgressUnit(int unit);
    // User interacted with the progress slider (click or release after drag).
    void onProgressSliderCommitted(int progressUnit);
    // Live QMediaPlayer position → intra-sentence fraction for the seek bar.
    void onAudioPositionChanged(qint64 positionMs, qint64 durationMs);
    // Sub-word resolution for the seek slider (units per word). Word count
    // alone only jumps at sentence boundaries; this lets the thumb move
    // smoothly inside a segment and seek mid-sentence.
    static constexpr int kProgressUnitsPerWord = 100;
    // Persistent warning strip under the toolbar (no Piper / no models /
    // connection failure). Empty message hides it.
    void setAudioWarning(const QString &message);
    void setPlaybackEnabled(bool enabled);

    // Find-in-document bar (Ctrl+F).
    void showFindBar();
    void hideFindBar();
    void findNext();
    void findPrevious();
    // direction: true = forward, false = backward. Starts from the current
    // cursor; wraps once if nothing is found to the end/start.
    bool findInDocument(bool forward);

    // --- Recent files + per-document reading position -------------------------
    // Canonical path of the file currently loaded, or empty for paste /
    // welcome / stdin. Positions and the recent list only apply when set.
    void setCurrentDocumentPath(const QString &path);
    void saveCurrentPosition();
    // After reindex: jump to the saved sentence if one exists for this path.
    void restoreSavedPosition();
    void addToRecentFiles(const QString &path);
    void rebuildRecentMenu();
    static QStringList loadRecentFiles();
    static void storeRecentFiles(const QStringList &paths);
    // QSettings-safe key for a filesystem path (paths contain '/').
    static QString positionSettingsKey(const QString &path);
    static int loadSavedSentenceIndex(const QString &path);
    static void storeSavedSentenceIndex(const QString &path, int sentenceIndex);
    // Accept local file URLs from a drop (window or document view).
    void openDroppedUrls(const QList<QUrl> &urls);

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
    QAction *m_marginsAction = nullptr; // checkable session toggle for extra text margins
    QAction *m_findAction = nullptr;    // checkable toggle for the find bar
    QAction *m_openAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    QLabel *m_warningBanner = nullptr; // hidden unless Piper/audio is unusable
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLabel *m_findStatusLabel = nullptr;
    QLabel *m_statusDot = nullptr; // colored circle in the status bar
    QLabel *m_activityLabel = nullptr; // status-bar activity text beside the dot
    QSlider *m_progressSlider;   // word-based position through the document
    QLabel *m_elapsedLabel;      // estimated listening time so far
    QLabel *m_remainingLabel;    // estimated listening time left (−m:ss)
    QSlider *m_volumeSlider = nullptr; // 0–100 → QAudioOutput linear gain
    QToolButton *m_muteButton = nullptr;
    bool m_realAudioAvailable = false;

    QVector<Sentence> m_sentences;
    bool m_startupContentLoaded = false;
    bool m_showingWelcome = false;
    // Absolute canonical path when the document came from a real file;
    // empty for paste, welcome, or stdin ("-").
    QString m_currentDocumentPath;

    // Last voice list reported by the server ("ready" message) -- used to
    // populate PreferencesDialog's voice dropdown.
    QStringList m_knownVoices;

    // Word-count bookkeeping for the progress strip (elapsed/remaining and
    // slider range). Recomputed whenever m_sentences changes.
    int m_totalWords = 0;
    QVector<int> m_cumulativeWordsBeforeSentence; // parallel to m_sentences
    int m_currentSentenceIndex = -1; // -1 = no position yet (nothing played/clicked)
    // 0..1 progress through the current sentence's audio (from QMediaPlayer).
    double m_intraSentenceFraction = 0.0;
    bool m_progressSliderPressed = false; // suppress programmatic feedback while dragging
    // Skip persist while programmatically restoring a saved index on open.
    bool m_restoringPosition = false;

    // Startup connection retry: the spawned server needs a moment to import
    // Python/load models before its socket exists, so the first few connect
    // attempts are expected to fail.
    QString m_targetSocketPath;
    int m_connectAttemptsLeft = 0;
};
