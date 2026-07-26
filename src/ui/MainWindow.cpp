#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QSizePolicy>
#include <QSize>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <algorithm>

#include "DocumentView.h"
#include "PreferencesDialog.h"
#include "document/SentenceSplitter.h"
#include "speech/PiperClient.h"
#include "speech/PiperServerManager.h"
#include "speech/PlaybackController.h"

namespace {
// Rough word count -- good enough for a length/position estimate, not
// meant to be precise (matches the "can be rough" spirit of the estimate).
int countWords(const QString &text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return text.split(whitespace, Qt::SkipEmptyParts).size();
}

// Typical spoken-word rate at 1.0x speed, used only to turn a word count
// into a rough minutes estimate -- Piper's actual rate varies by voice.
constexpr double kWordsPerMinuteBase = 150.0;
} // namespace

MainWindow::MainWindow(const Options &options, QWidget *parent)
    : QMainWindow(parent), m_options(options)
{
    m_documentView = new DocumentView(this);
    QFont initialFont = m_documentView->baseFont();
    if (!m_options.fontFamily.isEmpty()) {
        initialFont.setFamily(m_options.fontFamily);
    }
    if (m_options.fontPointSize > 0) {
        initialFont.setPointSize(m_options.fontPointSize);
    }
    m_documentView->setBaseFont(initialFont);
    m_documentView->setColors(m_options.colors);

    m_piperClient = new PiperClient(this);
    m_playback = new PlaybackController(m_piperClient, this);

    setUpUi();
    setUpShortcuts();

    connect(m_piperClient, &PiperClient::ready, this, &MainWindow::onVoiceListReady);
    connect(m_piperClient, &PiperClient::ready, this, [this] {
        m_connectAttemptsLeft = 0; // successfully connected; stop treating further errors as retryable
        setActivity(tr("Ready"));
        // Deferred (rather than opened immediately in main()) because the
        // socket connect is asynchronous -- opening it before the event
        // loop has even started would mean autoplay tries to speak before
        // we're actually connected.
        loadStartupContentIfNeeded();
    });
    connect(m_piperClient, &PiperClient::connectionError, this, [this](const QString &msg) {
        // A freshly-spawned piper-server needs a moment (Python startup,
        // potentially loading voice models) before its socket exists, so
        // the first several connect attempts failing is expected, not an
        // error -- retry quietly rather than alarming the user immediately.
        if (m_connectAttemptsLeft > 0) {
            --m_connectAttemptsLeft;
            QTimer::singleShot(200, this, [this] { m_piperClient->connectToServer(m_targetSocketPath); });
            return;
        }
        qWarning().noquote() << "Piper connection error:" << msg;
        setActivity(tr("Piper connection failed"));
        statusBar()->showMessage(tr("Piper connection error: %1").arg(msg), 5000);
    });
    connect(m_playback, &PlaybackController::sentenceStarted, this, &MainWindow::onSentenceStarted);
    connect(m_playback, &PlaybackController::preparingAudio, this, &MainWindow::onPreparingAudio);
    connect(m_playback, &PlaybackController::errorOccurred, this, &MainWindow::onPlaybackError);
    connect(m_playback, &PlaybackController::playbackFinished, this, [this] {
        setPlayingUi(false);
        m_documentView->setHighlightState(DocumentView::HighlightState::Stopped);
        setActivity(tr("Finished"));
        statusBar()->showMessage(tr("Finished"), 3000);
    });
    connect(m_documentView, &DocumentView::positionClicked, this, &MainWindow::onDocumentClicked);

    QString socketPath;
    if (!m_options.piperSocket.isEmpty()) {
        // User asked to attach to a server they're managing themselves --
        // we must never spawn or kill a process in this case.
        socketPath = m_options.piperSocket;
        setActivity(tr("Connecting to piper-server…"));
        qInfo().noquote() << "Using externally-managed piper-server at" << socketPath;
    } else {
        setActivity(tr("Starting piper-server…"));
        m_piperServerManager = new PiperServerManager(this);
        connect(m_piperServerManager, &PiperServerManager::serverOutput, this, [](const QString &text) {
            qDebug().noquote() << "[piper-server]" << text.trimmed();
        });
        connect(m_piperServerManager, &PiperServerManager::failedToStart, this, [this](const QString &reason) {
            qWarning().noquote() << reason;
            setActivity(tr("Piper server unavailable"));
            statusBar()->showMessage(reason, 8000);
        });
        socketPath = m_piperServerManager->startWithUniqueSocket();
        if (!socketPath.isEmpty()) {
            qInfo().noquote() << "Spawned piper-server on" << socketPath;
        }
    }

    if (!socketPath.isEmpty()) {
        connectToPiperServer(socketPath);
    }

    refreshInfoLabel(); // initializes to "No document loaded"

    resize(900, 700);
    setWindowIcon(QIcon(QStringLiteral(":/icons/text2sprech.png")));
    setWindowTitle(tr("text2sprech"));
}

void MainWindow::connectToPiperServer(const QString &socketPath)
{
    m_targetSocketPath = socketPath;
    // ~6s of retries at 200ms apart -- generous enough to cover typical
    // Python startup and voice-model loading without hanging indefinitely
    // if something is actually wrong.
    m_connectAttemptsLeft = 30;
    m_piperClient->connectToServer(socketPath);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_playback->stop();
    if (m_piperServerManager) {
        // Only torn down when we spawned it ourselves; an externally
        // provided --piper-socket server is left running for whoever
        // started it.
        m_piperServerManager->stop();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::setUpUi()
{
    // Central column: document on top, media-player transport bar below.
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_documentView, /*stretch=*/1);

    // --- Top toolbar: document → speech → reading aids → settings ----------
    auto *toolbar = addToolBar(tr("Controls"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(18, 18));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto themeIcon = [](const QString &name, const QIcon &fallback) {
        const QIcon icon = QIcon::fromTheme(name);
        return icon.isNull() ? fallback : icon;
    };

    // Document: open + paste (clipboard is the common case; selection is a
    // submenu entry so the bar stays short on platforms that support both).
    auto *openAction = toolbar->addAction(
        themeIcon(QStringLiteral("document-open"), style()->standardIcon(QStyle::SP_DialogOpenButton)),
        tr("Open"));
    openAction->setToolTip(tr("Open a text or HTML file"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);
    addAction(openAction); // so the shortcut works when the toolbar action isn't focused

    auto *pasteClipboardAction = new QAction(
        themeIcon(QStringLiteral("edit-paste"), style()->standardIcon(QStyle::SP_DialogResetButton)),
        tr("Paste"), this);
    pasteClipboardAction->setToolTip(tr("Paste from the clipboard (Ctrl+V)"));
    connect(pasteClipboardAction, &QAction::triggered, this, [this] {
        pasteFromSource(QClipboard::Clipboard);
    });

    auto *pasteSelectionAction = new QAction(tr("Paste selection"), this);
    pasteSelectionAction->setToolTip(tr("Paste the primary selection (middle-click)"));
    if (!QApplication::clipboard()->supportsSelection()) {
        pasteSelectionAction->setEnabled(false);
        pasteSelectionAction->setToolTip(tr("Primary selection isn't supported on this platform"));
    }
    connect(pasteSelectionAction, &QAction::triggered, this, [this] {
        pasteFromSource(QClipboard::Selection);
    });

    auto *pasteMenu = new QMenu(this);
    pasteMenu->addAction(pasteClipboardAction);
    pasteMenu->addAction(pasteSelectionAction);
    auto *pasteButton = new QToolButton(toolbar);
    pasteButton->setDefaultAction(pasteClipboardAction);
    pasteButton->setMenu(pasteMenu);
    pasteButton->setPopupMode(QToolButton::MenuButtonPopup);
    pasteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pasteButton->setIconSize(toolbar->iconSize());
    toolbar->addWidget(pasteButton);

    toolbar->addSeparator();

    // Speech: voice + speed in one compact cluster with a single label each.
    auto *voiceLabel = new QLabel(tr("Voice"), toolbar);
    voiceLabel->setContentsMargins(4, 0, 2, 0);
    toolbar->addWidget(voiceLabel);
    m_voiceCombo = new QComboBox(toolbar);
    m_voiceCombo->setMinimumWidth(160);
    m_voiceCombo->setMaximumWidth(280);
    m_voiceCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_voiceCombo->setToolTip(tr("TTS voice for this session"));
    connect(m_voiceCombo, &QComboBox::currentTextChanged, this, [this](const QString &voice) {
        m_playback->setVoice(voice);
    });
    toolbar->addWidget(m_voiceCombo);

    auto *speedLabel = new QLabel(tr("Speed"), toolbar);
    speedLabel->setContentsMargins(8, 0, 2, 0);
    toolbar->addWidget(speedLabel);
    m_speedSpin = new QDoubleSpinBox(toolbar);
    m_speedSpin->setRange(0.5, 3.0);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->setDecimals(2);
    m_speedSpin->setSuffix(tr("×"));
    m_speedSpin->setMinimumWidth(72);
    m_speedSpin->setToolTip(tr("Playback speed (also + / − keys)"));
    m_speedSpin->blockSignals(true);
    m_speedSpin->setValue(m_options.speed);
    m_speedSpin->blockSignals(false);
    connect(m_speedSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::onSpeedChanged);
    toolbar->addWidget(m_speedSpin);

    toolbar->addSeparator();

    // Reading: text size quick adjust (exact size lives in Preferences).
    auto *decreaseTextSizeAction = toolbar->addAction(tr("A−"));
    decreaseTextSizeAction->setToolTip(tr("Decrease text size (Ctrl+−)"));
    connect(decreaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::decreaseTextSize);

    auto *increaseTextSizeAction = toolbar->addAction(tr("A+"));
    increaseTextSizeAction->setToolTip(tr("Increase text size (Ctrl++)"));
    connect(increaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::increaseTextSize);

    toolbar->addSeparator();

    auto *preferencesAction = toolbar->addAction(
        themeIcon(QStringLiteral("preferences-system"), style()->standardIcon(QStyle::SP_FileDialogDetailedView)),
        tr("Preferences"));
    preferencesAction->setToolTip(tr("Startup defaults (voice, speed, font, colors)"));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::openPreferences);

    connect(m_documentView, &DocumentView::previousSentenceRequested, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex - 1);
    });
    connect(m_documentView, &DocumentView::nextSentenceRequested, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex + 1);
    });
    connect(m_documentView, &DocumentView::pasteRequested, this, [this](QClipboard::Mode mode) {
        pasteFromSource(mode);
    });

    // --- Bottom transport bar (media-player style) ---
    auto *playerBar = new QWidget(central);
    playerBar->setObjectName(QStringLiteral("playerBar"));
    auto *playerLayout = new QHBoxLayout(playerBar);
    playerLayout->setContentsMargins(8, 6, 8, 6);
    playerLayout->setSpacing(4);

    const QSize iconSize(28, 28);

    auto makeTransportButton = [this, playerBar, iconSize](QStyle::StandardPixmap sp, const QString &tip) {
        auto *btn = new QToolButton(playerBar);
        btn->setIcon(style()->standardIcon(sp));
        btn->setIconSize(iconSize);
        btn->setToolTip(tip);
        btn->setAutoRaise(true);
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btn->setFocusPolicy(Qt::NoFocus);
        return btn;
    };

    auto *prevBtn = makeTransportButton(QStyle::SP_MediaSkipBackward, tr("Previous sentence (Left)"));
    connect(prevBtn, &QToolButton::clicked, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex - 1);
    });

    m_playPauseAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    m_playPauseAction->setToolTip(tr("Play (Space)"));
    connect(m_playPauseAction, &QAction::triggered, this, &MainWindow::togglePlayPause);
    auto *playPauseBtn = new QToolButton(playerBar);
    playPauseBtn->setDefaultAction(m_playPauseAction);
    playPauseBtn->setIconSize(QSize(36, 36));
    playPauseBtn->setAutoRaise(true);
    playPauseBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    playPauseBtn->setFocusPolicy(Qt::NoFocus);

    auto *stopBtn = makeTransportButton(QStyle::SP_MediaStop, tr("Stop"));
    connect(stopBtn, &QToolButton::clicked, this, [this] {
        m_playback->stop();
        // Keep the current sentence visible in the muted Stopped color
        // (same as pause / finished) rather than clearing the highlight.
        m_documentView->setHighlightState(DocumentView::HighlightState::Stopped);
        setPlayingUi(false);
        setActivity(tr("Stopped"));
    });

    auto *nextBtn = makeTransportButton(QStyle::SP_MediaSkipForward, tr("Next sentence (Right)"));
    connect(nextBtn, &QToolButton::clicked, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex + 1);
    });

    // Center the transport cluster like a regular media player.
    playerLayout->addStretch(1);
    playerLayout->addWidget(prevBtn);
    playerLayout->addWidget(playPauseBtn);
    playerLayout->addWidget(stopBtn);
    playerLayout->addWidget(nextBtn);
    playerLayout->addStretch(1);

    centralLayout->addWidget(playerBar);
    setCentralWidget(central);

    // Left/normal area: current activity ("Preparing audio…", "Playing",
    // etc.) -- temporarily hidden by statusBar()->showMessage() popups and
    // revealed again once those expire, which is standard QStatusBar
    // behavior and exactly what we want here (a transient notification
    // briefly covering the steady-state indicator).
    m_activityLabel = new QLabel(this);
    statusBar()->addWidget(m_activityLabel);

    // Right/permanent area: length + position estimate, always visible.
    m_infoLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_infoLabel);

    // Visible immediately while the piper-server connects; replaced if
    // startup paste/file loads, otherwise stays until the user opens/pastes.
    showWelcomeMessage();
}

void MainWindow::setUpShortcuts()
{
    auto *playPause = new QAction(this);
    playPause->setShortcut(Qt::Key_Space);
    connect(playPause, &QAction::triggered, this, &MainWindow::togglePlayPause);
    addAction(playPause);

    auto *speedUp = new QAction(this);
    speedUp->setShortcut(Qt::Key_Plus);
    connect(speedUp, &QAction::triggered, this, [this] { m_speedSpin->stepUp(); });
    addAction(speedUp);

    auto *speedDown = new QAction(this);
    speedDown->setShortcut(Qt::Key_Minus);
    connect(speedDown, &QAction::triggered, this, [this] { m_speedSpin->stepDown(); });
    addAction(speedDown);

    auto *increaseTextSize = new QAction(this);
    increaseTextSize->setShortcut(QKeySequence::ZoomIn);
    connect(increaseTextSize, &QAction::triggered, m_documentView, &DocumentView::increaseTextSize);
    addAction(increaseTextSize);

    auto *decreaseTextSize = new QAction(this);
    decreaseTextSize->setShortcut(QKeySequence::ZoomOut);
    connect(decreaseTextSize, &QAction::triggered, m_documentView, &DocumentView::decreaseTextSize);
    addAction(decreaseTextSize);
}

void MainWindow::openFile()
{
    QString path = QFileDialog::getOpenFileName(
        this, tr("Open Document"), QString(), tr("Text/HTML files (*.txt *.html *.htm);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    openFilePath(path);
}

void MainWindow::openFilePath(const QString &path)
{
    QString contents;
    bool isHtml = false;

    if (path == QLatin1String("-")) {
        // Pseudo-path: read the document from stdin (e.g. `cat book.txt | text2sprech -`).
        // Must happen once at startup before anything else consumes stdin.
        QTextStream stream(stdin, QIODevice::ReadOnly);
        stream.setEncoding(QStringConverter::Utf8);
        contents = stream.readAll();
        const QString head = contents.trimmed().left(64).toLower();
        isHtml = head.startsWith(QLatin1String("<!doctype html"))
            || head.startsWith(QLatin1String("<html"));
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Open failed"), file.errorString());
            return;
        }
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf8);
        contents = stream.readAll();
        isHtml = QFileInfo(path).suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0
            || QFileInfo(path).suffix().compare(QLatin1String("htm"), Qt::CaseInsensitive) == 0;
    }

    loadDocumentText(contents, isHtml);
}

void MainWindow::pasteFromSource(QClipboard::Mode mode)
{
    QClipboard *clipboard = QApplication::clipboard();
    if (mode == QClipboard::Selection && !clipboard->supportsSelection()) {
        // Primary selection is an X11/Wayland-with-support concept; it
        // doesn't exist on Windows or macOS.
        const QString msg = tr("This platform doesn't support the primary selection (--paste-selection).");
        qWarning().noquote() << msg;
        statusBar()->showMessage(msg, 5000);
        return;
    }
    const QMimeData *mime = clipboard->mimeData(mode);
    if (mime->hasHtml()) {
        loadDocumentText(mime->html(), true);
    } else if (mime->hasText()) {
        loadDocumentText(mime->text(), false);
    } else {
        statusBar()->showMessage(tr("Nothing to paste from that source"), 3000);
    }
}

void MainWindow::showWelcomeMessage()
{
    // Lightweight HTML so the empty state explains what to do without
    // looking like a loaded document (no sentence index, not speakable).
    const QString html = tr(
        "<div style='margin: 2.5em 2em; line-height: 1.55;'>"
        "<h2 style='font-weight: 600; margin-bottom: 0.4em;'>%1</h2>"
        "<p style='opacity: 0.9; margin-top: 0;'>%2</p>"
        "<ul style='margin-top: 1.2em; padding-left: 1.4em;'>"
        "<li style='margin-bottom: 0.55em;'>%3</li>"
        "<li style='margin-bottom: 0.55em;'>%4</li>"
        "<li style='margin-bottom: 0.55em;'>%5</li>"
        "<li style='margin-bottom: 0.55em;'>%6</li>"
        "<li style='margin-bottom: 0.55em;'>%7</li>"
        "</ul>"
        "<p style='opacity: 0.75; margin-top: 1.6em;'>%8</p>"
        "</div>")
        .arg(tr("text2sprech"),
             tr("A local text-to-speech reader with live sentence highlighting."),
             tr("<b>Open</b> a text or HTML file, or <b>Paste</b> from the clipboard "
                "(Ctrl+V). On Linux you can also paste the primary selection "
                "(middle-click or the Paste menu)."),
             tr("Press <b>Play</b> (Space) to hear the document. The current sentence "
                "is highlighted as it is spoken."),
             tr("Click any sentence, or use the <b>Left</b>/<b>Right</b> arrow keys, "
                "to jump. Previous / Next on the transport bar do the same."),
             tr("Choose <b>Voice</b> and <b>Speed</b> on the toolbar for this session. "
                "<b>Preferences</b> sets defaults for the next launch (and can Apply "
                "them now)."),
             tr("Playback controls sit at the bottom: previous, play/pause, stop, next."),
             tr("Speech stays on your machine via Piper — nothing is sent to the cloud."));

    m_showingWelcome = true;
    m_playback->stop();
    m_documentView->clearHighlight();
    m_documentView->loadHtml(html);
    // Do not index or offer the welcome copy for speech.
    m_sentences.clear();
    m_playback->loadSentences({});
    m_documentView->setSentenceBoundaries({});
    m_totalWords = 0;
    m_cumulativeWordsBeforeSentence.clear();
    m_currentSentenceIndex = -1;
    setPlayingUi(false);
    refreshInfoLabel();
}

void MainWindow::loadStartupContentIfNeeded()
{
    if (m_startupContentLoaded) {
        return;
    }
    m_startupContentLoaded = true;

    if (m_options.pasteSelection && m_options.pasteClipboard) {
        qWarning().noquote() << "Both --paste-selection and --paste-clipboard given; using --paste-selection";
    }

    if (m_options.pasteSelection) {
        pasteFromSource(QClipboard::Selection);
    } else if (m_options.pasteClipboard) {
        pasteFromSource(QClipboard::Clipboard);
    } else if (!m_options.initialFile.isEmpty()) {
        openFilePath(m_options.initialFile);
    }
}

void MainWindow::loadDocumentText(const QString &content, bool isHtml)
{
    m_playback->stop();
    m_documentView->clearHighlight();
    m_showingWelcome = false;

    if (isHtml) {
        m_documentView->loadHtml(content);
    } else {
        m_documentView->loadPlainText(content);
    }
    reindexSentences();
    if (m_options.autoplay && !m_sentences.isEmpty()) {
        m_playback->play();
        setPlayingUi(true);
    } else {
        setPlayingUi(false);
    }
    statusBar()->showMessage(tr("Loaded %1 sentences").arg(m_sentences.size()), 3000);
}

void MainWindow::reindexSentences()
{
    // Split on the *rendered* plain text so offsets line up with what
    // DocumentView::highlightRange() expects -- important for the HTML
    // case, where source and rendered text differ.
    QString plainText = m_documentView->document()->toPlainText();
    m_sentences = SentenceSplitter::split(plainText, /*startId=*/0);
    m_playback->loadSentences(m_sentences);
    m_documentView->setSentenceBoundaries(m_sentences);

    m_cumulativeWordsBeforeSentence.clear();
    m_cumulativeWordsBeforeSentence.reserve(m_sentences.size());
    int running = 0;
    for (const auto &s : m_sentences) {
        m_cumulativeWordsBeforeSentence.append(running);
        running += countWords(s.text);
    }
    m_totalWords = running;
    m_currentSentenceIndex = -1; // no position yet in the freshly (re)loaded document
    refreshInfoLabel();
}

int MainWindow::sentenceIndexForOffset(int offset) const
{
    auto it = std::upper_bound(
        m_sentences.begin(), m_sentences.end(), offset,
        [](int off, const Sentence &s) { return off < s.end; });
    return it == m_sentences.end() ? static_cast<int>(m_sentences.size()) - 1
                                    : static_cast<int>(it - m_sentences.begin());
}

void MainWindow::togglePlayPause()
{
    if (m_playback->isPlaying()) {
        m_playback->pause();
        setPlayingUi(false);
        // Keep the current sentence visible, but switch to the muted
        // "stopped/selected" color so it no longer looks actively speaking.
        m_documentView->setHighlightState(DocumentView::HighlightState::Stopped);
        setActivity(tr("Paused"));
    } else {
        m_playback->play();
        setPlayingUi(true);
        // play() will emit preparingAudio / sentenceStarted, which recolor
        // the highlight; no need to force a state here.
    }
}

void MainWindow::setPlayingUi(bool playing)
{
    if (playing) {
        m_playPauseAction->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
        m_playPauseAction->setText(tr("Pause"));
        m_playPauseAction->setToolTip(tr("Pause (Space)"));
    } else {
        m_playPauseAction->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        m_playPauseAction->setText(tr("Play"));
        m_playPauseAction->setToolTip(tr("Play (Space)"));
    }
}

void MainWindow::onSentenceStarted(int sentenceId, int start, int end)
{
    m_documentView->highlightRange(start, end, DocumentView::HighlightState::Playing);
    m_currentSentenceIndex = sentenceId; // ids are assigned sequentially == index into m_sentences
    setActivity(tr("Playing"));
    refreshInfoLabel();
}

void MainWindow::onPreparingAudio(int sentenceId)
{
    if (sentenceId >= 0 && sentenceId < m_sentences.size()) {
        const Sentence &s = m_sentences[sentenceId];
        m_documentView->highlightRange(s.start, s.end, DocumentView::HighlightState::Preparing);
        m_currentSentenceIndex = sentenceId;
        refreshInfoLabel();
    } else {
        m_documentView->setHighlightState(DocumentView::HighlightState::Preparing);
    }
    setActivity(tr("Preparing audio…"));
}

void MainWindow::onDocumentClicked(int plainTextOffset)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    jumpToSentence(sentenceIndexForOffset(plainTextOffset));
}

void MainWindow::jumpToSentence(int index)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(m_sentences.size()) - 1);
    const Sentence &sentence = m_sentences[index];
    // Immediate visual feedback regardless of playback state -- if we're
    // paused/stopped, seekToSentence() below won't emit sentenceStarted
    // (that only fires once audio actually starts), so without this a
    // click or arrow-key press would appear to do nothing until Play.
    // Color: Preparing if playback is already running (synth will follow),
    // otherwise the muted Stopped/selected state.
    const auto state = m_playback->isPlaying()
        ? DocumentView::HighlightState::Preparing
        : DocumentView::HighlightState::Stopped;
    m_documentView->highlightRange(sentence.start, sentence.end, state);
    m_currentSentenceIndex = index;
    refreshInfoLabel();
    m_playback->seekToSentence(index);
}

void MainWindow::onVoiceListReady(const QStringList &voices, const QString &current)
{
    m_knownVoices = voices;

    QString selected = current;
    if (!m_options.voice.isEmpty()) {
        if (voices.contains(m_options.voice)) {
            selected = m_options.voice;
        } else {
            qWarning().noquote() << "Requested --voice" << m_options.voice
                                  << "not available on server; using default" << current;
        }
    }

    m_voiceCombo->blockSignals(true);
    m_voiceCombo->clear();
    m_voiceCombo->addItems(voices);
    int idx = voices.indexOf(selected);
    if (idx >= 0) {
        m_voiceCombo->setCurrentIndex(idx);
    }
    m_voiceCombo->blockSignals(false);

    if (selected != current) {
        m_playback->setVoice(selected);
    }
    // Speed was set on the spin box during setUpUi() before we were
    // connected, so the actual synth-side speed was never sent -- send it
    // now that the connection is live.
    m_playback->setSpeed(m_options.speed);

    statusBar()->showMessage(tr("Connected to piper-server (%1 voices)").arg(voices.size()), 3000);
}

void MainWindow::onSpeedChanged(double speed)
{
    m_playback->setSpeed(speed);
    refreshInfoLabel(); // estimated minutes scales with speed
}

void MainWindow::onPlaybackError(const QString &message)
{
    qWarning().noquote() << message;
    statusBar()->showMessage(message, 5000);
}

void MainWindow::openPreferences()
{
    // Seed from saved startup defaults (config file), not the live session.
    QSettings settings;

    auto readString = [&settings](const QString &key) {
        return settings.value(key).toString();
    };
    auto readColor = [&settings](const QString &key) {
        const QString s = settings.value(key).toString();
        if (s.isEmpty()) {
            return QColor();
        }
        QColor c(s);
        return c.isValid() ? c : QColor();
    };
    auto groupHasPrefs = [&settings] {
        return settings.contains(QStringLiteral("voice")) || settings.contains(QStringLiteral("speed"))
            || settings.contains(QStringLiteral("fontFamily"))
            || settings.contains(QStringLiteral("fontPointSize"))
            || settings.contains(QStringLiteral("colorText"))
            || settings.contains(QStringLiteral("colorBackground"))
            || settings.contains(QStringLiteral("colorPlaying"))
            || settings.contains(QStringLiteral("colorPreparing"))
            || settings.contains(QStringLiteral("colorStopped"))
            || settings.contains(QStringLiteral("colorHover"));
    };

    QString defaultVoice;
    double defaultSpeed = 1.0;
    QString defaultFamily;
    int defaultPointSize = 12;
    DocumentView::Colors defaultColors;

    auto readGroup = [&] {
        defaultVoice = readString(QStringLiteral("voice"));
        bool speedOk = false;
        double s = settings.value(QStringLiteral("speed"), 1.0).toDouble(&speedOk);
        defaultSpeed = (speedOk && s >= 0.5 && s <= 3.0) ? s : 1.0;
        defaultFamily = readString(QStringLiteral("fontFamily"));
        bool sizeOk = false;
        int ps = settings.value(QStringLiteral("fontPointSize"), 12).toInt(&sizeOk);
        defaultPointSize = (sizeOk && ps >= 6 && ps <= 72) ? ps : 12;
        defaultColors.text = readColor(QStringLiteral("colorText"));
        defaultColors.background = readColor(QStringLiteral("colorBackground"));
        defaultColors.playing = readColor(QStringLiteral("colorPlaying"));
        defaultColors.preparing = readColor(QStringLiteral("colorPreparing"));
        defaultColors.stopped = readColor(QStringLiteral("colorStopped"));
        defaultColors.hover = readColor(QStringLiteral("colorHover"));
    };

    settings.beginGroup(QStringLiteral("startup"));
    const bool haveStartup = groupHasPrefs();
    if (haveStartup) {
        readGroup();
    }
    settings.endGroup();
    if (!haveStartup) {
        settings.beginGroup(QStringLiteral("general"));
        if (groupHasPrefs()) {
            readGroup();
        }
        settings.endGroup();
    }

    QFont defaultFont;
    if (!defaultFamily.isEmpty()) {
        defaultFont.setFamily(defaultFamily);
    }
    defaultFont.setPointSize(defaultPointSize);

    PreferencesDialog dialog(m_knownVoices, defaultVoice, defaultSpeed, defaultFont, defaultColors, this);

    auto applyToSession = [this, &dialog] {
        if (!dialog.voice().isEmpty()) {
            m_voiceCombo->setCurrentText(dialog.voice());
        }
        m_speedSpin->setValue(dialog.speed());
        m_documentView->setBaseFont(dialog.font());
        m_documentView->setColors(dialog.colors());
        statusBar()->showMessage(tr("Preferences applied to this session"), 3000);
    };
    connect(&dialog, &PreferencesDialog::applyRequested, this, applyToSession);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QFont chosenFont = dialog.font();
    const DocumentView::Colors chosenColors = dialog.colors();
    auto storeColor = [&settings](const QString &key, const QColor &c) {
        if (c.isValid()) {
            settings.setValue(key, c.name(QColor::HexArgb));
        } else {
            settings.remove(key);
        }
    };

    settings.beginGroup(QStringLiteral("startup"));
    settings.setValue(QStringLiteral("voice"), dialog.voice());
    settings.setValue(QStringLiteral("speed"), dialog.speed());
    settings.setValue(QStringLiteral("fontFamily"), chosenFont.family());
    settings.setValue(QStringLiteral("fontPointSize"), chosenFont.pointSize());
    storeColor(QStringLiteral("colorText"), chosenColors.text);
    storeColor(QStringLiteral("colorBackground"), chosenColors.background);
    storeColor(QStringLiteral("colorPlaying"), chosenColors.playing);
    storeColor(QStringLiteral("colorPreparing"), chosenColors.preparing);
    storeColor(QStringLiteral("colorStopped"), chosenColors.stopped);
    storeColor(QStringLiteral("colorHover"), chosenColors.hover);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("general"));
    settings.remove(QString());
    settings.endGroup();
    settings.sync();

    statusBar()->showMessage(tr("Preferences saved (apply on next start)"), 3000);
}

void MainWindow::setActivity(const QString &text)
{
    m_activityLabel->setText(text);
}

void MainWindow::refreshInfoLabel()
{
    if (m_sentences.isEmpty()) {
        m_infoLabel->setText(m_showingWelcome ? tr("Welcome") : tr("No document loaded"));
        return;
    }

    const double speed = qMax(0.1, m_speedSpin->value());
    const int estimatedMinutes = static_cast<int>((m_totalWords / (kWordsPerMinuteBase * speed)) + 0.5);
    QString text = tr("~%1 words, ~%2 min").arg(m_totalWords).arg(qMax(0, estimatedMinutes));

    if (m_currentSentenceIndex >= 0 && m_currentSentenceIndex < m_cumulativeWordsBeforeSentence.size()) {
        const int wordsSoFar = m_cumulativeWordsBeforeSentence[m_currentSentenceIndex];
        const int percent = m_totalWords > 0 ? static_cast<int>(100.0 * wordsSoFar / m_totalWords) : 0;
        text += tr("  |  word %1 (%2%)").arg(wordsSoFar).arg(percent);
    }

    m_infoLabel->setText(text);
}
