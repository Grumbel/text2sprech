#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QStandardPaths>
#include <algorithm>

#include "DocumentView.h"
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
    m_documentView->setInitialZoomSteps(m_options.textZoomSteps);
    setCentralWidget(m_documentView);

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
        m_playPauseAction->setText(tr("Play"));
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
    auto *toolbar = addToolBar(tr("Controls"));
    toolbar->setMovable(false);

    auto *openAction = toolbar->addAction(tr("Open…"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    auto *pasteClipboardAction = toolbar->addAction(tr("Paste Clipboard"));
    connect(pasteClipboardAction, &QAction::triggered, this, [this] { pasteFromSource(QClipboard::Clipboard); });

    auto *pasteSelectionAction = toolbar->addAction(tr("Paste Selection"));
    if (!QApplication::clipboard()->supportsSelection()) {
        pasteSelectionAction->setEnabled(false);
        pasteSelectionAction->setToolTip(tr("Primary selection isn't supported on this platform"));
    }
    connect(pasteSelectionAction, &QAction::triggered, this, [this] { pasteFromSource(QClipboard::Selection); });

    toolbar->addSeparator();

    auto *prevAction = toolbar->addAction(tr("⏮"));
    connect(prevAction, &QAction::triggered, this, [this] { m_playback->previous(); });

    m_playPauseAction = toolbar->addAction(tr("Play"));
    connect(m_playPauseAction, &QAction::triggered, this, &MainWindow::togglePlayPause);

    auto *nextAction = toolbar->addAction(tr("⏭"));
    connect(nextAction, &QAction::triggered, this, [this] { m_playback->next(); });

    auto *stopAction = toolbar->addAction(tr("Stop"));
    connect(stopAction, &QAction::triggered, this, [this] {
        m_playback->stop();
        m_documentView->clearHighlight();
        m_playPauseAction->setText(tr("Play"));
        setActivity(tr("Stopped"));
    });

    toolbar->addSeparator();

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setMinimumWidth(200);
    connect(m_voiceCombo, &QComboBox::currentTextChanged, this, [this](const QString &voice) {
        m_playback->setVoice(voice);
    });
    toolbar->addWidget(m_voiceCombo);

    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setRange(0.5, 3.0);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->blockSignals(true);
    m_speedSpin->setValue(m_options.speed);
    m_speedSpin->blockSignals(false);
    m_speedSpin->setPrefix(tr("Speed: "));
    m_speedSpin->setSuffix(tr("x"));
    connect(m_speedSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::onSpeedChanged);
    toolbar->addWidget(m_speedSpin);

    toolbar->addSeparator();

    auto *decreaseTextSizeAction = toolbar->addAction(tr("A-"));
    decreaseTextSizeAction->setToolTip(tr("Decrease text size (Ctrl+-)"));
    connect(decreaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::decreaseTextSize);

    auto *resetTextSizeAction = toolbar->addAction(tr("Reset size"));
    resetTextSizeAction->setToolTip(tr("Reset text size (Ctrl+0)"));
    connect(resetTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::resetTextSize);

    auto *increaseTextSizeAction = toolbar->addAction(tr("A+"));
    increaseTextSizeAction->setToolTip(tr("Increase text size (Ctrl++)"));
    connect(increaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::increaseTextSize);

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
}

void MainWindow::setUpShortcuts()
{
    auto *playPause = new QAction(this);
    playPause->setShortcut(Qt::Key_Space);
    connect(playPause, &QAction::triggered, this, &MainWindow::togglePlayPause);
    addAction(playPause);

    auto *prev = new QAction(this);
    prev->setShortcut(Qt::Key_Left);
    connect(prev, &QAction::triggered, this, [this] { m_playback->previous(); });
    addAction(prev);

    auto *next = new QAction(this);
    next->setShortcut(Qt::Key_Right);
    connect(next, &QAction::triggered, this, [this] { m_playback->next(); });
    addAction(next);

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

    auto *resetTextSize = new QAction(this);
    resetTextSize->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(resetTextSize, &QAction::triggered, m_documentView, &DocumentView::resetTextSize);
    addAction(resetTextSize);
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Open failed"), file.errorString());
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    QString contents = stream.readAll();

    bool isHtml = QFileInfo(path).suffix().compare("html", Qt::CaseInsensitive) == 0
        || QFileInfo(path).suffix().compare("htm", Qt::CaseInsensitive) == 0;
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

    if (isHtml) {
        m_documentView->loadHtml(content);
    } else {
        m_documentView->loadPlainText(content);
    }
    reindexSentences();
    if (m_options.autoplay && !m_sentences.isEmpty()) {
        m_playback->play();
        m_playPauseAction->setText(tr("Pause"));
    } else {
        m_playPauseAction->setText(tr("Play"));
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
        m_playPauseAction->setText(tr("Play"));
        setActivity(tr("Paused"));
    } else {
        m_playback->play();
        m_playPauseAction->setText(tr("Pause"));
    }
}

void MainWindow::onSentenceStarted(int sentenceId, int start, int end)
{
    m_documentView->highlightRange(start, end);
    m_currentSentenceIndex = sentenceId; // ids are assigned sequentially == index into m_sentences
    setActivity(tr("Playing"));
    refreshInfoLabel();
}

void MainWindow::onPreparingAudio(int /*sentenceId*/)
{
    setActivity(tr("Preparing audio…"));
}

void MainWindow::onDocumentClicked(int plainTextOffset)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    int index = sentenceIndexForOffset(plainTextOffset);
    const Sentence &sentence = m_sentences[index];
    // Immediate visual feedback regardless of playback state -- if we're
    // paused/stopped, seekToSentence() below won't emit sentenceStarted
    // (that only fires once audio actually starts), so without this the
    // click would appear to do nothing until Play is pressed.
    m_documentView->highlightRange(sentence.start, sentence.end);
    m_currentSentenceIndex = index;
    refreshInfoLabel();
    m_playback->seekToSentence(index);
}

void MainWindow::onVoiceListReady(const QStringList &voices, const QString &current)
{
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

void MainWindow::setActivity(const QString &text)
{
    m_activityLabel->setText(text);
}

void MainWindow::refreshInfoLabel()
{
    if (m_sentences.isEmpty()) {
        m_infoLabel->setText(tr("No document loaded"));
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
