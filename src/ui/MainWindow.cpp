#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QTextStream>
#include <QToolBar>
#include <QStandardPaths>
#include <algorithm>

#include "DocumentView.h"
#include "document/SentenceSplitter.h"
#include "speech/PiperClient.h"
#include "speech/PlaybackController.h"

namespace {
// Must match piper_server/server.py's default exactly (--socket flag /
// READER_PIPER_SOCKET env var) -- previously this used $XDG_RUNTIME_DIR,
// which is set to /run/user/<uid> on most systemd desktops and so almost
// never matched the server's /tmp default, leaving the client trying to
// connect to a socket the server was never listening on.
QString defaultSocketPath()
{
    QString fromEnv = QString::fromLocal8Bit(qgetenv("READER_PIPER_SOCKET"));
    if (!fromEnv.isEmpty()) {
        return fromEnv;
    }
    return QStringLiteral("/tmp/reader-piper.sock");
}
} // namespace

MainWindow::MainWindow(const Options &options, QWidget *parent)
    : QMainWindow(parent), m_options(options)
{
    m_documentView = new DocumentView(this);
    setCentralWidget(m_documentView);

    m_piperClient = new PiperClient(this);
    m_playback = new PlaybackController(m_piperClient, this);

    setUpUi();
    setUpShortcuts();

    connect(m_piperClient, &PiperClient::ready, this, &MainWindow::onVoiceListReady);
    connect(m_piperClient, &PiperClient::ready, this, [this] {
        // Deferred (rather than opened immediately in main()) because the
        // socket connect is asynchronous -- opening it before the event
        // loop has even started would mean autoplay tries to speak before
        // we're actually connected. This connection only ever acts once.
        if (!m_initialFileLoaded && !m_options.initialFile.isEmpty()) {
            m_initialFileLoaded = true;
            openFilePath(m_options.initialFile);
        }
    });
    connect(m_piperClient, &PiperClient::connectionError, this, [this](const QString &msg) {
        qWarning().noquote() << "Piper connection error:" << msg;
        statusBar()->showMessage(tr("Piper connection error: %1").arg(msg), 5000);
    });
    connect(m_playback, &PlaybackController::sentenceStarted, this, &MainWindow::onSentenceStarted);
    connect(m_playback, &PlaybackController::errorOccurred, this, &MainWindow::onPlaybackError);
    connect(m_playback, &PlaybackController::playbackFinished, this, [this] {
        m_playPauseAction->setText(tr("Play"));
        statusBar()->showMessage(tr("Finished"), 3000);
    });
    connect(m_documentView, &DocumentView::positionClicked, this, &MainWindow::onDocumentClicked);

    const QString socketPath = defaultSocketPath();
    qInfo().noquote() << "Connecting to piper-server at" << socketPath;
    m_piperClient->connectToServer(socketPath);

    resize(900, 700);
    setWindowTitle(tr("Reader"));
}

void MainWindow::setUpUi()
{
    auto *toolbar = addToolBar(tr("Controls"));
    toolbar->setMovable(false);

    auto *openAction = toolbar->addAction(tr("Open…"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

    auto *pasteAction = toolbar->addAction(tr("Paste"));
    connect(pasteAction, &QAction::triggered, this, &MainWindow::pasteFromClipboard);

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
    });

    toolbar->addSeparator();

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setMinimumWidth(200);
    connect(m_voiceCombo, &QComboBox::currentTextChanged, m_piperClient, &PiperClient::setVoice);
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

    statusBar()->showMessage(tr("Connecting to piper-server…"));
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

void MainWindow::pasteFromClipboard()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (mime->hasHtml()) {
        loadDocumentText(mime->html(), true);
    } else if (mime->hasText()) {
        loadDocumentText(mime->text(), false);
    } else {
        statusBar()->showMessage(tr("Clipboard has no text content"), 3000);
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
    } else {
        m_playback->play();
        m_playPauseAction->setText(tr("Pause"));
    }
}

void MainWindow::onSentenceStarted(int /*sentenceId*/, int start, int end)
{
    m_documentView->highlightRange(start, end);
}

void MainWindow::onDocumentClicked(int plainTextOffset)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    int index = sentenceIndexForOffset(plainTextOffset);
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
        m_piperClient->setVoice(selected);
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
}

void MainWindow::onPlaybackError(const QString &message)
{
    qWarning().noquote() << message;
    statusBar()->showMessage(message, 5000);
}
