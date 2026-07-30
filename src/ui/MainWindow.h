// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QClipboard>
#include <QMainWindow>
#include <QSettings>
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
class QEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QSlider;
class QShortcut;
class QToolBar;
class QToolButton;
class QWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Command-line-configurable startup behavior (see main.cpp for parsing).
    struct Options {
        QString voice;         // empty = use server's default voice
        double speed = 1.0;    // PlaybackController::kMinSpeed … kMaxSpeed
        bool autoplay = false; // start playback as soon as a document is loaded
        QString initialFile;   // empty = don't open anything on startup
        // Empty family = leave the widget's default family alone; pointSize
        // <= 0 = leave the default size alone. See DocumentView::setBaseFont().
        // Overridable via CLI (--font-family / --font-size) or config.
        QString fontFamily;
        int fontPointSize = -1;
        // View colors from config or CLI (invalid QColor = built-in theme default).
        DocumentView::Colors colors;
        // Extra text margin (percent of view width, each side) when the
        // toolbar Margins toggle is on. CLI --margin / config; the toggle
        // itself is session state.
        int extraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
        double lineSpacing = DocumentView::kDefaultLineSpacing;
        // If non-empty, QSettings for startup defaults and Preferences Save
        // use this .ini path (--config) instead of the default location.
        QString configPath;
        // Presentation / teleprompter defaults (config [presentation] group).
        // Invalid color channels resolve to DocumentView::presentationDefaults().
        QString presentationFontFamily; // empty → keep reading/session family
        int presentationFontPointSize = DocumentView::kPresentationFontPointSize;
        int presentationExtraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
        double presentationLineSpacing = DocumentView::kDefaultPresentationLineSpacing;
        DocumentView::Colors presentationColors;
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
    void changeEvent(QEvent *event) override;
    // Space play/pause (except real text fields), Enter on speed, click chrome.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void openFile();
    // Classic Save As…: write the current document plain text to a user-chosen path.
    void saveDocumentAs();
    // Synthesize sentences to a single WAV (progress dialog; suspends playback).
    void exportAudio();
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
    // Shows the About dialog (project description, URL, license, copyright).
    void openAbout();

private:
    void setUpUi();
    void setUpShortcuts();
    // Return keyboard focus to the document so Space / arrows work again.
    void focusDocumentView();
    // True when Space should insert a character (find field, editable text)
    // rather than toggle playback.
    static bool widgetNeedsLiteralSpace(QWidget *widget);
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

    // QSettings scoped to m_options.configPath when --config was given,
    // otherwise the default organization .ini under ~/.config/text2sprech/.
    QSettings makeSettings() const;
    // Word + UTF-8 byte counts for the status-bar right side (after reindex).
    void refreshDocumentStats();

    // --- Presentation / teleprompter mode ------------------------------------
    // Fullscreen document with presentation colors + larger font; chrome
    // (menu, toolbar, player, status) is hidden. Tab or the context menu
    // can reveal chrome temporarily; Escape / the action exits fully.
    void setPresentationMode(bool on);
    void applyPresentationChromeVisibility();
    void togglePresentationChrome();
    void showDocumentContextMenu(const QPoint &globalPos);
    void updatePresentationActionIcon();

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
    QAction *m_findAction = nullptr;    // checkable toggle for the find bar
    QAction *m_presentationAction = nullptr; // checkable: presentation / fullscreen
    QShortcut *m_presentationTabShortcut = nullptr; // Tab toggles chrome (any focus)
    QShortcut *m_presentationEscShortcut = nullptr; // Esc exits presentation
    QAction *m_openAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    QToolBar *m_toolBar = nullptr;
    QWidget *m_playerBar = nullptr;
    QLabel *m_warningBanner = nullptr; // hidden unless Piper/audio is unusable
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLabel *m_findStatusLabel = nullptr;
    QLabel *m_statusDot = nullptr; // colored circle in the status bar
    QLabel *m_activityLabel = nullptr; // status-bar activity text beside the dot
    QLabel *m_documentStatsLabel = nullptr; // right side: "N words · M bytes"
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
    // True when the loaded document was HTML (file/paste/welcome). Save As
    // writes this form rather than stripping to plain text.
    bool m_documentIsHtml = false;
    // Bytes we last loaded into the view: sanitized semantic HTML, or plain
    // text. Used by Save As so markup survives (toPlainText would strip it).
    QString m_documentSource;

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

    // Presentation mode state. When active, document uses presentation
    // colors + larger font and the window is fullscreen; chrome may still
    // be shown temporarily (Tab / context menu).
    bool m_presentationMode = false;
    bool m_presentationChromeVisible = false;
    DocumentView::Colors m_savedColors;
    QFont m_savedFont;
    int m_savedMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    double m_savedLineSpacing = DocumentView::kDefaultLineSpacing;
    // Live presentation settings (seeded from config; Preferences can update).
    QString m_presentationFontFamily; // empty → use saved/session family
    int m_presentationFontPointSize = DocumentView::kPresentationFontPointSize;
    int m_presentationExtraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    double m_presentationLineSpacing = DocumentView::kDefaultPresentationLineSpacing;
    DocumentView::Colors m_presentationColors; // invalid → presentationDefaults()
    // True while we are programmatically leaving fullscreen so changeEvent
    // does not treat the window-state change as an external exit.
    bool m_leavingPresentation = false;

    // Merge user presentation color overrides with built-in teleprompter defaults.
    static DocumentView::Colors resolvedPresentationColors(const DocumentView::Colors &overrides);
    // Apply current m_presentation* settings to the document view (while in mode).
    void applyLivePresentationAppearance();
};
