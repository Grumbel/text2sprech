// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTextCursor>
#include <QTextDocument>
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
#include <QStyleOptionSlider>
#include <QSlider>
#include <QMouseEvent>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

#include "DocumentView.h"
#include "PreferencesDialog.h"
#include "document/HtmlSanitizer.h"
#include "document/SentenceSplitter.h"
#include "speech/PiperClient.h"
#include "speech/PiperServerManager.h"
#include "speech/PlaybackController.h"

namespace {
// Rough word count -- good enough for progress estimates, not precise.
int countWords(const QString &text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    return text.split(whitespace, Qt::SkipEmptyParts).size();
}

// QSlider's default groove click does a page-step on many styles (especially
// under Linux), which feels broken for a media seek bar. Handle press/move/
// release ourselves so a click jumps absolute and a drag keeps seeking.
class ClickJumpSlider : public QSlider {
public:
    explicit ClickJumpSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            setSliderDown(true); // emits sliderPressed
            setValue(valueFromPos(event->pos()));
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (isSliderDown()) {
            setValue(valueFromPos(event->pos()));
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && isSliderDown()) {
            setValue(valueFromPos(event->pos()));
            setSliderDown(false); // emits sliderReleased
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    int valueFromPos(const QPoint &pos) const
    {
        if (maximum() <= minimum()) {
            return minimum();
        }
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderHandle, this);

        if (orientation() == Qt::Horizontal) {
            const int sliderMin = groove.x();
            const int sliderMax = groove.right() - handle.width() + 1;
            const int span = qMax(1, sliderMax - sliderMin);
            const int x = pos.x() - handle.width() / 2;
            return QStyle::sliderValueFromPosition(minimum(), maximum(), x - sliderMin, span,
                                                   opt.upsideDown);
        }
        const int sliderMin = groove.y();
        const int sliderMax = groove.bottom() - handle.height() + 1;
        const int span = qMax(1, sliderMax - sliderMin);
        const int y = pos.y() - handle.height() / 2;
        return QStyle::sliderValueFromPosition(minimum(), maximum(), y - sliderMin, span,
                                               opt.upsideDown);
    }
};

// Typical spoken-word rate at 1.0x speed -- Piper's actual rate varies by voice.
constexpr double kWordsPerMinuteBase = 150.0;

// Format seconds as m:ss or h:mm:ss (no leading zero hours).
QString formatDuration(int totalSeconds)
{
    totalSeconds = qMax(0, totalSeconds);
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

int wordsToSeconds(double words, double speed)
{
    const double wpm = kWordsPerMinuteBase * qMax(0.1, speed);
    return static_cast<int>((words / wpm) * 60.0 + 0.5);
}

constexpr int kMaxRecentFiles = 12;

QString canonicalizePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return info.absoluteFilePath();
}
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
    m_documentView->setExtraMarginSize(m_options.extraMarginSize);
    // Margins stay off until the user toggles the toolbar button (session).

    m_piperClient = new PiperClient(this);
    m_playback = new PlaybackController(m_piperClient, this);

    setUpUi();
    setUpShortcuts();

    connect(m_piperClient, &PiperClient::ready, this, &MainWindow::onServerReady);
    connect(m_piperClient, &PiperClient::ready, this, [this](const PiperServerInfo &info) {
        m_connectAttemptsLeft = 0; // successfully connected; stop treating further errors as retryable
        if (info.realAudio) {
            setActivity(tr("Ready"), ActivityKind::Ready);
        }
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
        setActivity(tr("Piper connection failed"), ActivityKind::Error);
        setPlaybackEnabled(false);
        setAudioWarning(tr("Cannot connect to piper-server: %1").arg(msg));
        statusBar()->showMessage(tr("Piper connection error: %1").arg(msg), 8000);
    });
    connect(m_piperClient, &PiperClient::disconnected, this, [this] {
        if (m_connectAttemptsLeft > 0) {
            return; // still in the startup retry window
        }
        setPlaybackEnabled(false);
        setActivity(tr("Disconnected"), ActivityKind::Error);
        setAudioWarning(tr("Lost connection to piper-server."));
    });
    connect(m_playback, &PlaybackController::sentenceStarted, this, &MainWindow::onSentenceStarted);
    connect(m_playback, &PlaybackController::preparingAudio, this, &MainWindow::onPreparingAudio);
    connect(m_playback, &PlaybackController::errorOccurred, this, &MainWindow::onPlaybackError);
    connect(m_playback, &PlaybackController::audioPositionChanged, this, &MainWindow::onAudioPositionChanged);
    connect(m_playback, &PlaybackController::playbackFinished, this, [this] {
        setPlayingUi(false);
        m_documentView->setHighlightState(DocumentView::HighlightState::Stopped);
        m_intraSentenceFraction = 0.0;
        setActivity(tr("Finished"), ActivityKind::Ready);
        statusBar()->showMessage(tr("Finished"), 3000);
        refreshProgress();
    });
    connect(m_documentView, &DocumentView::positionClicked, this, &MainWindow::onDocumentClicked);

    QString socketPath;
    if (!m_options.piperSocket.isEmpty()) {
        // User asked to attach to a server they're managing themselves --
        // we must never spawn or kill a process in this case.
        socketPath = m_options.piperSocket;
        setActivity(tr("Connecting to piper-server…"), ActivityKind::Idle);
        qInfo().noquote() << "Using externally-managed piper-server at" << socketPath;
    } else {
        setActivity(tr("Starting piper-server…"), ActivityKind::Idle);
        m_piperServerManager = new PiperServerManager(this);
        connect(m_piperServerManager, &PiperServerManager::serverOutput, this, [](const QString &text) {
            qDebug().noquote() << "[piper-server]" << text.trimmed();
        });
        connect(m_piperServerManager, &PiperServerManager::failedToStart, this, [this](const QString &reason) {
            qWarning().noquote() << reason;
            setActivity(tr("Piper server unavailable"), ActivityKind::Error);
            setPlaybackEnabled(false);
            setAudioWarning(reason);
            statusBar()->showMessage(reason, 8000);
        });
        socketPath = m_piperServerManager->startWithUniqueSocket();
        if (!socketPath.isEmpty()) {
            qInfo().noquote() << "Spawned piper-server on" << socketPath;
        } else {
            // failedToStart already emitted; nothing further to connect to.
        }
    }

    if (!socketPath.isEmpty()) {
        connectToPiperServer(socketPath);
    } else if (!m_options.piperSocket.isEmpty()) {
        // External socket path was empty string somehow — treat as error.
        setPlaybackEnabled(false);
        setAudioWarning(tr("No piper-server socket path configured."));
    }

    refreshProgress(); // initializes to "No document loaded"

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
    saveCurrentPosition();
    m_playback->stop();
    // Drop the Unix socket before SIGTERM'ing the server. Otherwise the
    // server's asyncio shutdown waits for this still-open client connection
    // and closeEvent blocks for the full process-wait timeout.
    m_piperClient->disconnectFromServer();
    if (m_piperServerManager) {
        // Only torn down when we spawned it ourselves; an externally
        // provided --piper-socket server is left running for whoever
        // started it.
        m_piperServerManager->stop();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData() && event->mimeData()->hasUrls()) {
        openDroppedUrls(event->mimeData()->urls());
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void MainWindow::setUpUi()
{
    setAcceptDrops(true);

    // Central column: optional audio warning, document, media-player bar.
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    m_warningBanner = new QLabel(central);
    m_warningBanner->setObjectName(QStringLiteral("audioWarningBanner"));
    m_warningBanner->setWordWrap(true);
    m_warningBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_warningBanner->setContentsMargins(12, 8, 12, 8);
    m_warningBanner->setVisible(false);
    // Inline style scoped to this widget only (avoids the dialog-wide
    // stylesheet palette pitfall noted in PreferencesDialog).
    m_warningBanner->setStyleSheet(
        QStringLiteral("QLabel#audioWarningBanner {"
                       "  background-color: #5c3b00;"
                       "  color: #fff3d0;"
                       "  border-bottom: 1px solid #8a5a00;"
                       "  padding: 6px 12px;"
                       "}"));
    centralLayout->addWidget(m_warningBanner);

    // Find bar (Ctrl+F) — hidden until requested.
    m_findBar = new QWidget(central);
    m_findBar->setObjectName(QStringLiteral("findBar"));
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(8, 4, 8, 4);
    findLayout->setSpacing(6);
    auto *findLabel = new QLabel(tr("Find:"), m_findBar);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setPlaceholderText(tr("Text in document"));
    m_findEdit->setClearButtonEnabled(true);
    auto *findPrevBtn = new QToolButton(m_findBar);
    findPrevBtn->setText(tr("Previous"));
    findPrevBtn->setToolTip(tr("Find previous (Shift+Enter)"));
    findPrevBtn->setAutoRaise(true);
    auto *findNextBtn = new QToolButton(m_findBar);
    findNextBtn->setText(tr("Next"));
    findNextBtn->setToolTip(tr("Find next (Enter)"));
    findNextBtn->setAutoRaise(true);
    m_findStatusLabel = new QLabel(m_findBar);
    m_findStatusLabel->setMinimumWidth(80);
    auto *findCloseBtn = new QToolButton(m_findBar);
    findCloseBtn->setText(tr("✕"));
    findCloseBtn->setToolTip(tr("Close find (Esc)"));
    findCloseBtn->setAutoRaise(true);
    findLayout->addWidget(findLabel);
    findLayout->addWidget(m_findEdit, /*stretch=*/1);
    findLayout->addWidget(findPrevBtn);
    findLayout->addWidget(findNextBtn);
    findLayout->addWidget(m_findStatusLabel);
    findLayout->addWidget(findCloseBtn);
    m_findBar->setVisible(false);
    centralLayout->addWidget(m_findBar);
    centralLayout->addWidget(m_documentView, /*stretch=*/1);

    connect(findNextBtn, &QToolButton::clicked, this, &MainWindow::findNext);
    connect(findPrevBtn, &QToolButton::clicked, this, &MainWindow::findPrevious);
    connect(findCloseBtn, &QToolButton::clicked, this, &MainWindow::hideFindBar);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] {
        // Shift+Enter is awkward on QLineEdit; use modifiers on the event
        // via a small event filter would be overkill — Enter = next always,
        // Previous button / F3-style handled by shortcuts.
        findNext();
    });
    connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        m_findStatusLabel->clear();
    });

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

    // Document: Open (with Recent submenu) + paste.
    m_openAction = new QAction(
        themeIcon(QStringLiteral("document-open"), style()->standardIcon(QStyle::SP_DialogOpenButton)),
        tr("Open"), this);
    m_openAction->setToolTip(tr("Open a text or HTML file (or drop one onto the window)"));
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openFile);
    addAction(m_openAction);

    m_recentMenu = new QMenu(tr("Recent"), this);
    rebuildRecentMenu();
    auto *openButton = new QToolButton(toolbar);
    openButton->setDefaultAction(m_openAction);
    openButton->setMenu(m_recentMenu);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    openButton->setIconSize(toolbar->iconSize());
    toolbar->addWidget(openButton);

    connect(m_documentView, &DocumentView::filesDropped, this, &MainWindow::openDroppedUrls);

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
    m_speedSpin->setToolTip(tr("Playback speed (also [ ] < > + − keys)"));
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

    m_marginsAction = toolbar->addAction(tr("Margins"));
    m_marginsAction->setCheckable(true);
    m_marginsAction->setChecked(false);
    m_marginsAction->setToolTip(
        tr("Extra padding around the text (size set in Preferences)"));
    connect(m_marginsAction, &QAction::toggled, m_documentView, &DocumentView::setExtraMarginsEnabled);

    auto *findToolbarAction = toolbar->addAction(
        themeIcon(QStringLiteral("edit-find"), style()->standardIcon(QStyle::SP_FileDialogContentsView)),
        tr("Find"));
    findToolbarAction->setToolTip(tr("Find text in the document (Ctrl+F)"));
    connect(findToolbarAction, &QAction::triggered, this, &MainWindow::showFindBar);

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

    // --- Bottom player bar -------------------------------------------------
    // Layout matches the reference chrome:
    //   [elapsed] ———— progress slider ———— [−remaining] [N / M]
    //              Previous   Play   Stop   Next   (icons + labels)
    // Status bar (below): ● Ready
    auto *playerBar = new QWidget(central);
    playerBar->setObjectName(QStringLiteral("playerBar"));
    auto *playerColumn = new QVBoxLayout(playerBar);
    playerColumn->setContentsMargins(12, 8, 12, 4);
    playerColumn->setSpacing(4);

    // Progress row: times + seek track + sentence counter on one line.
    auto *progressRow = new QWidget(playerBar);
    auto *progressLayout = new QHBoxLayout(progressRow);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(10);

    m_elapsedLabel = new QLabel(QStringLiteral("0:00"), progressRow);
    m_elapsedLabel->setMinimumWidth(40);
    m_elapsedLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_elapsedLabel->setToolTip(tr("Estimated time listened (at current speed)"));

    m_progressSlider = new ClickJumpSlider(Qt::Horizontal, progressRow);
    m_progressSlider->setRange(0, 0);
    m_progressSlider->setValue(0);
    m_progressSlider->setEnabled(false);
    m_progressSlider->setFocusPolicy(Qt::NoFocus);
    m_progressSlider->setTracking(true);
    m_progressSlider->setPageStep(1); // page keys still move a little if focused
    m_progressSlider->setToolTip(tr("Jump to a position in the document"));
    m_progressSlider->setMinimumHeight(18);
    m_progressSlider->setMaximumHeight(22);
    connect(m_progressSlider, &QSlider::sliderPressed, this, [this] {
        m_progressSliderPressed = true;
        // Groove clicks jump via ClickJumpSlider before press; refresh labels
        // immediately so the thumb and times match the click.
        const int value = m_progressSlider->value();
        if (!m_sentences.isEmpty() && m_totalWords > 0) {
            const double speed = m_speedSpin->value();
            const double words = double(value) / double(kProgressUnitsPerWord);
            m_elapsedLabel->setText(formatDuration(wordsToSeconds(words, speed)));
            m_remainingLabel->setText(QStringLiteral("−%1").arg(
                formatDuration(wordsToSeconds(double(m_totalWords) - words, speed))));
            m_sentenceLabel->setText(
                tr("%1 / %2").arg(sentenceIndexForProgressUnit(value) + 1).arg(m_sentences.size()));
        }
    });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this] {
        m_progressSliderPressed = false;
        onProgressSliderCommitted(m_progressSlider->value());
    });
    connect(m_progressSlider, &QSlider::sliderMoved, this, [this](int value) {
        // Live-update the time labels while dragging without seeking audio
        // on every tick (that would thrash synthesis).
        if (!m_sentences.isEmpty() && m_totalWords > 0) {
            const double speed = m_speedSpin->value();
            const double words = double(value) / double(kProgressUnitsPerWord);
            m_elapsedLabel->setText(formatDuration(wordsToSeconds(words, speed)));
            m_remainingLabel->setText(QStringLiteral("−%1").arg(
                formatDuration(wordsToSeconds(double(m_totalWords) - words, speed))));
            const int idx = sentenceIndexForProgressUnit(value);
            m_sentenceLabel->setText(tr("%1 / %2").arg(idx + 1).arg(m_sentences.size()));
        }
    });
    // Keyboard / style-driven moves (not a drag) still seek on commit.
    connect(m_progressSlider, &QSlider::actionTriggered, this, [this](int /*action*/) {
        if (!m_progressSliderPressed) {
            QTimer::singleShot(0, this, [this] {
                if (!m_progressSliderPressed) {
                    onProgressSliderCommitted(m_progressSlider->value());
                }
            });
        }
    });

    m_remainingLabel = new QLabel(QStringLiteral("−0:00"), progressRow);
    m_remainingLabel->setMinimumWidth(48);
    m_remainingLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_remainingLabel->setToolTip(tr("Estimated time remaining (at current speed)"));

    m_sentenceLabel = new QLabel(progressRow);
    m_sentenceLabel->setMinimumWidth(56);
    m_sentenceLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_sentenceLabel->setToolTip(tr("Current sentence / total"));

    QFont metaFont = m_elapsedLabel->font();
    metaFont.setPointSizeF(metaFont.pointSizeF() * 0.95);
    m_elapsedLabel->setFont(metaFont);
    m_remainingLabel->setFont(metaFont);
    m_sentenceLabel->setFont(metaFont);

    progressLayout->addWidget(m_elapsedLabel, 0, Qt::AlignVCenter);
    progressLayout->addWidget(m_progressSlider, /*stretch=*/1);
    progressLayout->addWidget(m_remainingLabel, 0, Qt::AlignVCenter);
    progressLayout->addWidget(m_sentenceLabel, 0, Qt::AlignVCenter);
    playerColumn->addWidget(progressRow);

    // Transport row: icon-only Previous / Play / Stop / Next, truly centered.
    auto *transportRow = new QWidget(playerBar);
    auto *transportOuter = new QHBoxLayout(transportRow);
    transportOuter->setContentsMargins(0, 4, 0, 2);
    transportOuter->setSpacing(0);

    auto makeTransportButton = [this, transportRow](QStyle::StandardPixmap sp, const QString &tip,
                                                    const QSize &iconSize) {
        auto *btn = new QToolButton(transportRow);
        btn->setIcon(style()->standardIcon(sp));
        btn->setIconSize(iconSize);
        btn->setToolTip(tip);
        btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
        btn->setAutoRaise(true);
        btn->setFocusPolicy(Qt::NoFocus);
        return btn;
    };

    auto *prevBtn = makeTransportButton(QStyle::SP_MediaSkipBackward,
                                        tr("Previous sentence (Left)"), QSize(28, 28));
    connect(prevBtn, &QToolButton::clicked, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex - 1);
    });

    m_playPauseAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    m_playPauseAction->setToolTip(tr("Play (Space)"));
    connect(m_playPauseAction, &QAction::triggered, this, &MainWindow::togglePlayPause);
    auto *playPauseBtn = new QToolButton(transportRow);
    playPauseBtn->setDefaultAction(m_playPauseAction);
    playPauseBtn->setIconSize(QSize(36, 36));
    playPauseBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    playPauseBtn->setAutoRaise(true);
    playPauseBtn->setFocusPolicy(Qt::NoFocus);

    auto *stopBtn = makeTransportButton(QStyle::SP_MediaStop, tr("Stop"), QSize(28, 28));
    connect(stopBtn, &QToolButton::clicked, this, [this] {
        m_playback->stop();
        // Keep the current sentence visible in the muted Stopped color
        // (same as pause / finished) rather than clearing the highlight.
        m_documentView->setHighlightState(DocumentView::HighlightState::Stopped);
        setPlayingUi(false);
        m_intraSentenceFraction = 0.0; // stop() restarts the sentence from 0 next play
        setActivity(tr("Stopped"), ActivityKind::Idle);
        refreshProgress();
    });

    auto *nextBtn = makeTransportButton(QStyle::SP_MediaSkipForward,
                                        tr("Next sentence (Right)"), QSize(28, 28));
    connect(nextBtn, &QToolButton::clicked, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex + 1);
    });

    // Volume cluster stays available but sits off to the right so the
    // transport block remains visually centered like the reference UI.
    m_muteButton = new QToolButton(transportRow);
    m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_muteButton->setIconSize(QSize(20, 20));
    m_muteButton->setAutoRaise(true);
    m_muteButton->setFocusPolicy(Qt::NoFocus);
    m_muteButton->setCheckable(true);
    m_muteButton->setToolTip(tr("Mute"));
    connect(m_muteButton, &QToolButton::toggled, this, [this](bool muted) {
        m_playback->setMuted(muted);
        if (muted) {
            m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
            m_muteButton->setToolTip(tr("Unmute"));
        } else {
            m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
            m_muteButton->setToolTip(tr("Mute"));
        }
    });

    m_volumeSlider = new QSlider(Qt::Horizontal, transportRow);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setMaximumWidth(100);
    m_volumeSlider->setMinimumWidth(64);
    m_volumeSlider->setFixedHeight(20);
    m_volumeSlider->setFocusPolicy(Qt::NoFocus);
    m_volumeSlider->setToolTip(tr("Volume: 100%"));
    m_volumeSlider->setAccessibleName(tr("Volume"));
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        m_playback->setVolume(value / 100.0f);
        m_volumeSlider->setToolTip(tr("Volume: %1%").arg(value));
        if (value == 0) {
            if (m_muteButton && !m_muteButton->isChecked()) {
                m_muteButton->blockSignals(true);
                m_muteButton->setChecked(true);
                m_muteButton->blockSignals(false);
                m_playback->setMuted(true);
                m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolumeMuted));
                m_muteButton->setToolTip(tr("Unmute"));
            }
        } else if (m_muteButton && m_muteButton->isChecked()) {
            m_muteButton->blockSignals(true);
            m_muteButton->setChecked(false);
            m_muteButton->blockSignals(false);
            m_playback->setMuted(false);
            m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
            m_muteButton->setToolTip(tr("Mute"));
        }
    });
    m_playback->setVolume(1.0f);

    auto *transport = new QWidget(transportRow);
    auto *transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(0, 0, 0, 0);
    transportLayout->setSpacing(12);
    transportLayout->addWidget(prevBtn);
    transportLayout->addWidget(playPauseBtn);
    transportLayout->addWidget(stopBtn);
    transportLayout->addWidget(nextBtn);

    // Symmetric side panels: empty left vs volume on the right. Equal stretch
    // factors keep the transport group on the true horizontal center.
    auto *leftSide = new QWidget(transportRow);
    leftSide->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *rightSide = new QWidget(transportRow);
    rightSide->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *rightLayout = new QHBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);
    rightLayout->addStretch(1);
    rightLayout->addWidget(m_muteButton, 0, Qt::AlignVCenter);
    rightLayout->addWidget(m_volumeSlider, 0, Qt::AlignVCenter);

    transportOuter->addWidget(leftSide, /*stretch=*/1);
    transportOuter->addWidget(transport, /*stretch=*/0, Qt::AlignCenter);
    transportOuter->addWidget(rightSide, /*stretch=*/1);

    playerColumn->addWidget(transportRow);
    centralLayout->addWidget(playerBar);
    setCentralWidget(central);

    // Status bar: green (or state-colored) dot + activity text, matching the
    // reference "● Ready" presentation. Transient statusBar()->showMessage()
    // popups still temporarily cover this normal area.
    auto *statusWidget = new QWidget(this);
    auto *statusLayout = new QHBoxLayout(statusWidget);
    statusLayout->setContentsMargins(4, 0, 0, 0);
    statusLayout->setSpacing(8);
    m_statusDot = new QLabel(statusWidget);
    m_statusDot->setFixedSize(10, 10);
    m_statusDot->setAlignment(Qt::AlignCenter);
    m_activityLabel = new QLabel(statusWidget);
    statusLayout->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    statusLayout->addWidget(m_activityLabel, 0, Qt::AlignVCenter);
    statusLayout->addStretch(1);
    statusBar()->addWidget(statusWidget, /*stretch=*/1);
    setActivity(tr("Starting…"), ActivityKind::Idle);

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

    // Speed: + / ] / > faster; − / [ / < slower (mpv bracket + YouTube angle keys).
    auto *speedUp = new QAction(this);
    speedUp->setShortcuts({Qt::Key_Plus, Qt::Key_BracketRight, Qt::Key_Greater});
    connect(speedUp, &QAction::triggered, this, [this] { m_speedSpin->stepUp(); });
    addAction(speedUp);

    auto *speedDown = new QAction(this);
    speedDown->setShortcuts({Qt::Key_Minus, Qt::Key_BracketLeft, Qt::Key_Less});
    connect(speedDown, &QAction::triggered, this, [this] { m_speedSpin->stepDown(); });
    addAction(speedDown);

    auto *increaseTextSize = new QAction(this);
    // ZoomIn is typically Ctrl++; also bind Ctrl+= so layouts where + is
    // Shift+= don't require the extra modifier.
    increaseTextSize->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    connect(increaseTextSize, &QAction::triggered, m_documentView, &DocumentView::increaseTextSize);
    addAction(increaseTextSize);

    auto *decreaseTextSize = new QAction(this);
    decreaseTextSize->setShortcut(QKeySequence::ZoomOut);
    connect(decreaseTextSize, &QAction::triggered, m_documentView, &DocumentView::decreaseTextSize);
    addAction(decreaseTextSize);

    // Platform standard: Ctrl+Q (Linux/Windows), Cmd+Q (macOS). Goes through
    // closeEvent() so piper-server shutdown and socket cleanup still run.
    auto *quitAction = new QAction(this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    addAction(quitAction);

    auto *findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find); // Ctrl+F / Cmd+F
    connect(findAction, &QAction::triggered, this, &MainWindow::showFindBar);
    addAction(findAction);

    auto *findNextAction = new QAction(this);
    findNextAction->setShortcut(QKeySequence::FindNext); // often F3 / Ctrl+G
    connect(findNextAction, &QAction::triggered, this, &MainWindow::findNext);
    addAction(findNextAction);

    auto *findPrevAction = new QAction(this);
    findPrevAction->setShortcut(QKeySequence::FindPrevious);
    connect(findPrevAction, &QAction::triggered, this, &MainWindow::findPrevious);
    addAction(findPrevAction);

    // Esc closes the find bar when focus is inside it.
    if (m_findBar) {
        auto *escapeFind = new QAction(m_findBar);
        escapeFind->setShortcut(Qt::Key_Escape);
        escapeFind->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(escapeFind, &QAction::triggered, this, &MainWindow::hideFindBar);
        m_findBar->addAction(escapeFind);
    }
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
    QString resolvedPath;

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
        QFileInfo info(path);
        if (!info.exists() || !info.isFile()) {
            QMessageBox::warning(this, tr("Open failed"),
                                 tr("File not found:\n%1").arg(path));
            return;
        }
        resolvedPath = canonicalizePath(path);
        QFile file(resolvedPath.isEmpty() ? path : resolvedPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, tr("Open failed"), file.errorString());
            return;
        }
        QTextStream stream(&file);
        stream.setEncoding(QStringConverter::Utf8);
        contents = stream.readAll();
        isHtml = info.suffix().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0
            || info.suffix().compare(QLatin1String("htm"), Qt::CaseInsensitive) == 0;
    }

    // Persist the previous file's place before switching documents.
    saveCurrentPosition();
    setCurrentDocumentPath(resolvedPath);
    loadDocumentText(contents, isHtml);

    if (!resolvedPath.isEmpty()) {
        addToRecentFiles(resolvedPath);
        restoreSavedPosition();
    }
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
        saveCurrentPosition();
        setCurrentDocumentPath(QString());
        loadDocumentText(mime->html(), true);
    } else if (mime->hasText()) {
        saveCurrentPosition();
        setCurrentDocumentPath(QString());
        loadDocumentText(mime->text(), false);
    } else {
        statusBar()->showMessage(tr("Nothing to paste from that source"), 3000);
    }
}

void MainWindow::showWelcomeMessage()
{
    // Same pipeline as Open/Paste: HTML for readable layout, then sentence
    // split so Play / click-to-jump work on the welcome copy itself.
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
             tr("<b>Open</b> a text or HTML file (or drag one onto the window), "
                "pick a <b>Recent</b> file from the Open menu, or <b>Paste</b> "
                "from the clipboard (Ctrl+V). On Linux you can also paste the "
                "primary selection (middle-click or the Paste menu). Reading "
                "position is remembered per file."),
             tr("Press <b>Play</b> (Space) to hear the document. The current sentence "
                "is highlighted as it is spoken."),
             tr("Click any sentence, or use the <b>Left</b>/<b>Right</b> arrow keys, "
                "to jump. Previous / Next on the transport bar do the same. "
                "<b>Find</b> (Ctrl+F) searches within the document."),
             tr("Choose <b>Voice</b> and <b>Speed</b> on the toolbar for this session. "
                "<b>Preferences</b> sets defaults for the next launch (and can Apply "
                "them now)."),
             tr("Playback controls sit at the bottom: previous, play/pause, stop, next."),
             tr("Speech stays on your machine via Piper — nothing is sent to the cloud."));

    saveCurrentPosition();
    setCurrentDocumentPath(QString());
    loadDocumentText(html, /*isHtml=*/true);
    m_showingWelcome = true;
    refreshProgress();
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
        // Reader-mode-ish subset: headings, paragraphs, lists, emphasis,
        // code, links — no colours, font sizes, families, or images.
        m_documentView->loadHtml(HtmlSanitizer::toSemanticHtml(content));
    } else {
        m_documentView->loadPlainText(content);
    }
    reindexSentences();

    if (!m_currentDocumentPath.isEmpty()) {
        setWindowTitle(tr("%1 — text2sprech").arg(QFileInfo(m_currentDocumentPath).fileName()));
    } else {
        setWindowTitle(tr("text2sprech"));
    }

    if (m_options.autoplay && !m_sentences.isEmpty() && m_realAudioAvailable
        && m_piperClient->isConnected()) {
        m_playback->play();
        setPlayingUi(true);
    } else {
        setPlayingUi(false);
        if (m_options.autoplay && !m_realAudioAvailable) {
            statusBar()->showMessage(
                tr("Loaded %1 sentences — autoplay skipped (no TTS audio)").arg(m_sentences.size()),
                5000);
            return;
        }
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
    m_intraSentenceFraction = 0.0;
    refreshProgress();
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
        setActivity(tr("Paused"), ActivityKind::Idle);
        return;
    }
    if (!m_realAudioAvailable) {
        statusBar()->showMessage(
            tr("Cannot play: Piper TTS audio is not available. See the warning above."), 6000);
        return;
    }
    if (!m_piperClient->isConnected()) {
        statusBar()->showMessage(tr("Cannot play: not connected to piper-server."), 6000);
        return;
    }
    m_playback->play();
    setPlayingUi(true);
    // play() will emit preparingAudio / sentenceStarted, which recolor
    // the highlight; no need to force a state here.
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
    // Keep m_intraSentenceFraction: mid-sentence seeks set it (and the player
    // position) before sentenceStarted; a normal advance reports 0 via
    // audioPositionChanged almost immediately.
    setActivity(tr("Playing"), ActivityKind::Active);
    refreshProgress();
    saveCurrentPosition();
}

void MainWindow::onAudioPositionChanged(qint64 positionMs, qint64 durationMs)
{
    if (durationMs > 0) {
        m_intraSentenceFraction = qBound(0.0, double(positionMs) / double(durationMs), 1.0);
    } else {
        m_intraSentenceFraction = 0.0;
    }
    if (!m_progressSliderPressed) {
        refreshProgress();
    }
}

void MainWindow::onPreparingAudio(int sentenceId)
{
    if (sentenceId >= 0 && sentenceId < m_sentences.size()) {
        const Sentence &s = m_sentences[sentenceId];
        m_documentView->highlightRange(s.start, s.end, DocumentView::HighlightState::Preparing);
        m_currentSentenceIndex = sentenceId;
        refreshProgress();
    } else {
        m_documentView->setHighlightState(DocumentView::HighlightState::Preparing);
    }
    setActivity(tr("Preparing audio…"), ActivityKind::Active);
}

void MainWindow::onDocumentClicked(int plainTextOffset)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    jumpToSentence(sentenceIndexForOffset(plainTextOffset));
}

void MainWindow::jumpToSentence(int index, double fractionWithin)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(m_sentences.size()) - 1);
    fractionWithin = qBound(0.0, fractionWithin, 1.0);
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
    m_intraSentenceFraction = fractionWithin;
    refreshProgress();
    m_playback->seekToSentence(index, fractionWithin);
    if (!m_restoringPosition) {
        saveCurrentPosition();
    }
}

void MainWindow::onServerReady(const PiperServerInfo &info)
{
    m_knownVoices = info.voices;
    m_realAudioAvailable = info.realAudio;

    QString selected = info.currentVoice;
    if (!m_options.voice.isEmpty()) {
        if (info.voices.contains(m_options.voice)) {
            selected = m_options.voice;
        } else {
            qWarning().noquote() << "Requested --voice" << m_options.voice
                                  << "not available on server; using default" << info.currentVoice;
            statusBar()->showMessage(
                tr("Voice “%1” not found; using “%2”").arg(m_options.voice, info.currentVoice), 5000);
        }
    }

    m_voiceCombo->blockSignals(true);
    m_voiceCombo->clear();
    m_voiceCombo->addItems(info.voices);
    int idx = info.voices.indexOf(selected);
    if (idx >= 0) {
        m_voiceCombo->setCurrentIndex(idx);
    }
    m_voiceCombo->blockSignals(false);

    if (!selected.isEmpty() && selected != info.currentVoice) {
        m_playback->setVoice(selected);
    }
    // Speed was set on the spin box during setUpUi() before we were
    // connected, so the actual synth-side speed was never sent -- send it
    // now that the connection is live.
    m_playback->setSpeed(m_options.speed);

    if (!info.realAudio) {
        const QString warning = !info.warning.isEmpty()
            ? info.warning
            : tr("No real TTS audio available (backend: %1).").arg(
                  info.backend.isEmpty() ? tr("unknown") : info.backend);
        setAudioWarning(warning);
        setPlaybackEnabled(false);
        setActivity(tr("No audio"), ActivityKind::Warning);
        qWarning().noquote() << warning;
        statusBar()->showMessage(warning, 10000);
        return;
    }

    setAudioWarning(QString());
    setPlaybackEnabled(true);
    const QString backendLabel = info.backend.isEmpty() ? tr("piper") : info.backend;
    statusBar()->showMessage(
        tr("Connected (%1, %2 voice(s))").arg(backendLabel).arg(info.voices.size()), 4000);
}

void MainWindow::onSpeedChanged(double speed)
{
    m_playback->setSpeed(speed);
    refreshProgress(); // estimated minutes scales with speed
}

void MainWindow::onPlaybackError(const QString &message)
{
    qWarning().noquote() << message;
    setActivity(tr("Synthesis error"), ActivityKind::Error);
    statusBar()->showMessage(message, 8000);
}

void MainWindow::setAudioWarning(const QString &message)
{
    if (!m_warningBanner) {
        return;
    }
    if (message.isEmpty()) {
        m_warningBanner->clear();
        m_warningBanner->setVisible(false);
        return;
    }
    m_warningBanner->setText(message);
    m_warningBanner->setVisible(true);
}

void MainWindow::showFindBar()
{
    if (!m_findBar || !m_findEdit) {
        return;
    }
    m_findBar->setVisible(true);
    m_findEdit->setFocus(Qt::ShortcutFocusReason);
    m_findEdit->selectAll();
}

void MainWindow::hideFindBar()
{
    if (!m_findBar) {
        return;
    }
    m_findBar->setVisible(false);
    if (m_findStatusLabel) {
        m_findStatusLabel->clear();
    }
    // Clear the transient find selection so it doesn't linger as a
    // native highlight on top of sentence ExtraSelections.
    QTextCursor cursor = m_documentView->textCursor();
    cursor.clearSelection();
    m_documentView->setTextCursor(cursor);
    m_documentView->setFocus(Qt::OtherFocusReason);
}

void MainWindow::findNext()
{
    if (!m_findBar || !m_findBar->isVisible()) {
        showFindBar();
        return;
    }
    findInDocument(/*forward=*/true);
}

void MainWindow::findPrevious()
{
    if (!m_findBar || !m_findBar->isVisible()) {
        showFindBar();
        return;
    }
    findInDocument(/*forward=*/false);
}

bool MainWindow::findInDocument(bool forward)
{
    if (!m_findEdit) {
        return false;
    }
    const QString needle = m_findEdit->text();
    if (needle.isEmpty()) {
        m_findStatusLabel->setText(tr("Enter text"));
        return false;
    }

    const QTextDocument::FindFlags flags =
        forward ? QTextDocument::FindFlags() : QTextDocument::FindBackward;

    // Case-insensitive by default — better for a reader than strict match.
    auto tryFind = [&](bool fromStart) -> bool {
        if (fromStart) {
            QTextCursor cursor = m_documentView->textCursor();
            if (forward) {
                cursor.movePosition(QTextCursor::Start);
            } else {
                cursor.movePosition(QTextCursor::End);
            }
            m_documentView->setTextCursor(cursor);
        }
        return m_documentView->find(needle, flags);
    };

    bool found = tryFind(/*fromStart=*/false);
    if (!found) {
        // Wrap once.
        found = tryFind(/*fromStart=*/true);
        if (found) {
            m_findStatusLabel->setText(forward ? tr("Wrapped to top") : tr("Wrapped to bottom"));
        }
    } else {
        m_findStatusLabel->clear();
    }

    if (!found) {
        m_findStatusLabel->setText(tr("Not found"));
        statusBar()->showMessage(tr("“%1” not found").arg(needle), 3000);
        return false;
    }

    m_documentView->ensureCursorVisible();
    // Align progress / sentence highlight with the match when possible.
    const int pos = m_documentView->textCursor().selectionStart();
    if (!m_sentences.isEmpty() && pos >= 0) {
        const int idx = sentenceIndexForOffset(pos);
        if (idx >= 0 && idx < m_sentences.size()) {
            // Visual only — don't seek playback (user may be reading, not listening).
            const Sentence &s = m_sentences[idx];
            m_documentView->highlightRange(s.start, s.end, DocumentView::HighlightState::Stopped);
            m_currentSentenceIndex = idx;
            refreshProgress();
        }
    }
    return true;
}

void MainWindow::setPlaybackEnabled(bool enabled)
{
    if (m_playPauseAction) {
        m_playPauseAction->setEnabled(enabled);
    }
    if (m_progressSlider) {
        // Keep the slider usable for navigation even without audio.
        m_progressSlider->setEnabled(m_progressSlider->maximum() > 0);
    }
    if (m_voiceCombo) {
        m_voiceCombo->setEnabled(enabled && m_voiceCombo->count() > 0);
    }
    if (!enabled && m_playback && m_playback->isPlaying()) {
        m_playback->stop();
        setPlayingUi(false);
    }
}

void MainWindow::openPreferences()
{
    // Seed from saved startup defaults (config file), not the live session.
    QSettings settings;
    settings.beginGroup(QStringLiteral("startup"));

    auto readColor = [&settings](const QString &key) {
        const QString s = settings.value(key).toString();
        if (s.isEmpty()) {
            return QColor();
        }
        QColor c(s);
        return c.isValid() ? c : QColor();
    };

    const QString defaultVoice = settings.value(QStringLiteral("voice")).toString();
    bool speedOk = false;
    const double s = settings.value(QStringLiteral("speed"), 1.0).toDouble(&speedOk);
    const double defaultSpeed = (speedOk && s >= 0.5 && s <= 3.0) ? s : 1.0;
    const QString defaultFamily = settings.value(QStringLiteral("fontFamily")).toString();
    bool sizeOk = false;
    const int ps = settings.value(QStringLiteral("fontPointSize"), 12).toInt(&sizeOk);
    const int defaultPointSize = (sizeOk && ps >= 6 && ps <= 72) ? ps : 12;

    DocumentView::Colors defaultColors;
    defaultColors.text = readColor(QStringLiteral("colorText"));
    defaultColors.background = readColor(QStringLiteral("colorBackground"));
    defaultColors.playing = readColor(QStringLiteral("colorPlaying"));
    defaultColors.preparing = readColor(QStringLiteral("colorPreparing"));
    defaultColors.stopped = readColor(QStringLiteral("colorStopped"));
    defaultColors.hover = readColor(QStringLiteral("colorHover"));

    bool marginOk = false;
    const int marginVal = settings.value(QStringLiteral("extraMarginSize"),
                                         DocumentView::kDefaultExtraMarginSize)
                              .toInt(&marginOk);
    const int defaultExtraMargin =
        (marginOk && marginVal >= DocumentView::kMinExtraMarginSize
         && marginVal <= DocumentView::kMaxExtraMarginSize)
            ? marginVal
            : DocumentView::kDefaultExtraMarginSize;
    settings.endGroup();

    settings.beginGroup(QStringLiteral("voices"));
    const QStringList extraVoiceDirs = settings.value(QStringLiteral("extraDirs")).toStringList();
    settings.endGroup();

    QFont defaultFont;
    if (!defaultFamily.isEmpty()) {
        defaultFont.setFamily(defaultFamily);
    }
    defaultFont.setPointSize(defaultPointSize);

    PreferencesDialog dialog(m_knownVoices, defaultVoice, defaultSpeed, defaultFont, defaultColors,
                             extraVoiceDirs, defaultExtraMargin, this);

    auto applyToSession = [this, &dialog] {
        if (!dialog.voice().isEmpty()) {
            m_voiceCombo->setCurrentText(dialog.voice());
        }
        m_speedSpin->setValue(dialog.speed());
        m_documentView->setBaseFont(dialog.font());
        m_documentView->setColors(dialog.colors());
        m_documentView->setExtraMarginSize(dialog.extraMarginSize());
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
    settings.setValue(QStringLiteral("extraMarginSize"), dialog.extraMarginSize());
    storeColor(QStringLiteral("colorText"), chosenColors.text);
    storeColor(QStringLiteral("colorBackground"), chosenColors.background);
    storeColor(QStringLiteral("colorPlaying"), chosenColors.playing);
    storeColor(QStringLiteral("colorPreparing"), chosenColors.preparing);
    storeColor(QStringLiteral("colorStopped"), chosenColors.stopped);
    storeColor(QStringLiteral("colorHover"), chosenColors.hover);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("voices"));
    settings.setValue(QStringLiteral("extraDirs"), dialog.extraVoiceDirs());
    settings.endGroup();
    settings.sync();

    statusBar()->showMessage(
        tr("Preferences saved (voice directories apply on next start)"), 4000);
}

void MainWindow::setActivity(const QString &text, ActivityKind kind)
{
    if (m_activityLabel) {
        m_activityLabel->setText(text);
    }
    if (!m_statusDot) {
        return;
    }
    // Solid circle via a small styled label — matches the reference "● Ready".
    QString color;
    switch (kind) {
    case ActivityKind::Ready:
        color = QStringLiteral("#2e9b45"); // green
        break;
    case ActivityKind::Active:
        color = QStringLiteral("#2e7d32"); // deeper green while playing/working
        break;
    case ActivityKind::Warning:
        color = QStringLiteral("#c48a00"); // amber
        break;
    case ActivityKind::Error:
        color = QStringLiteral("#c62828"); // red
        break;
    case ActivityKind::Idle:
    default:
        color = QStringLiteral("#9aa0a6"); // muted gray
        break;
    }
    m_statusDot->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; border-radius: 5px; }").arg(color));
    m_statusDot->setToolTip(text);
}

void MainWindow::refreshProgress()
{
    if (m_sentences.isEmpty()) {
        m_progressSlider->blockSignals(true);
        m_progressSlider->setRange(0, 0);
        m_progressSlider->setValue(0);
        m_progressSlider->setEnabled(false);
        m_progressSlider->blockSignals(false);
        m_elapsedLabel->setText(QStringLiteral("0:00"));
        m_remainingLabel->setText(QStringLiteral("−0:00"));
        m_sentenceLabel->setText(m_showingWelcome ? tr("Welcome") : tr("—"));
        m_progressSlider->setToolTip(tr("No document loaded"));
        return;
    }

    const double speed = m_speedSpin->value();
    const int totalSec = wordsToSeconds(double(m_totalWords), speed);
    const int maxUnits = qMax(1, m_totalWords * kProgressUnitsPerWord);

    double wordsSoFar = 0.0;
    int displayIndex = 0; // 1-based when positioned; 0 means "not started"
    if (m_currentSentenceIndex >= 0
        && m_currentSentenceIndex < m_cumulativeWordsBeforeSentence.size()) {
        wordsSoFar = double(m_cumulativeWordsBeforeSentence[m_currentSentenceIndex]);
        const int wordsInSentence = countWords(m_sentences[m_currentSentenceIndex].text);
        wordsSoFar += m_intraSentenceFraction * double(qMax(1, wordsInSentence));
        displayIndex = m_currentSentenceIndex + 1;
    }

    const int elapsedSec = wordsToSeconds(wordsSoFar, speed);
    const int remainingSec = wordsToSeconds(double(m_totalWords) - wordsSoFar, speed);
    const int unitValue = qBound(
        0, static_cast<int>(wordsSoFar * double(kProgressUnitsPerWord) + 0.5), maxUnits);

    // Don't fight the user while they're dragging the thumb.
    if (!m_progressSliderPressed) {
        m_progressSlider->blockSignals(true);
        m_progressSlider->setEnabled(true);
        m_progressSlider->setRange(0, maxUnits);
        m_progressSlider->setValue(unitValue);
        m_progressSlider->blockSignals(false);
    }

    m_elapsedLabel->setText(formatDuration(elapsedSec));
    m_remainingLabel->setText(QStringLiteral("−%1").arg(formatDuration(remainingSec)));
    if (displayIndex > 0) {
        m_sentenceLabel->setText(tr("%1 / %2").arg(displayIndex).arg(m_sentences.size()));
    } else {
        m_sentenceLabel->setText(tr("— / %1").arg(m_sentences.size()));
    }

    const int percent = m_totalWords > 0
        ? static_cast<int>(100.0 * wordsSoFar / double(m_totalWords) + 0.5)
        : 0;
    m_progressSlider->setToolTip(
        tr("Position: %1% · ~%2 total at current speed · click or drag to jump")
            .arg(percent)
            .arg(formatDuration(totalSec)));
}

int MainWindow::sentenceIndexForProgressUnit(int unit) const
{
    if (m_sentences.isEmpty()) {
        return 0;
    }
    const double wordOffset = double(unit) / double(kProgressUnitsPerWord);
    // Last sentence whose start (words before it) is <= wordOffset.
    auto it = std::upper_bound(
        m_cumulativeWordsBeforeSentence.begin(), m_cumulativeWordsBeforeSentence.end(), wordOffset,
        [](double off, int before) { return off < double(before); });
    int index = static_cast<int>(it - m_cumulativeWordsBeforeSentence.begin()) - 1;
    return qBound(0, index, static_cast<int>(m_sentences.size()) - 1);
}

void MainWindow::onProgressSliderCommitted(int progressUnit)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    const int index = sentenceIndexForProgressUnit(progressUnit);
    const double wordPos = double(progressUnit) / double(kProgressUnitsPerWord);
    const int before = m_cumulativeWordsBeforeSentence.value(index, 0);
    const int wordsIn = qMax(1, countWords(m_sentences[index].text));
    const double fraction = qBound(0.0, (wordPos - double(before)) / double(wordsIn), 1.0);
    jumpToSentence(index, fraction);
}

void MainWindow::setCurrentDocumentPath(const QString &path)
{
    m_currentDocumentPath = path;
}

void MainWindow::saveCurrentPosition()
{
    if (m_currentDocumentPath.isEmpty() || m_showingWelcome) {
        return;
    }
    if (m_currentSentenceIndex < 0) {
        return;
    }
    storeSavedSentenceIndex(m_currentDocumentPath, m_currentSentenceIndex);
}

void MainWindow::restoreSavedPosition()
{
    if (m_currentDocumentPath.isEmpty() || m_sentences.isEmpty()) {
        return;
    }
    const int saved = loadSavedSentenceIndex(m_currentDocumentPath);
    if (saved < 0) {
        return;
    }
    m_restoringPosition = true;
    jumpToSentence(saved);
    m_restoringPosition = false;
    statusBar()->showMessage(
        tr("Resumed at sentence %1 of %2").arg(m_currentSentenceIndex + 1).arg(m_sentences.size()),
        3000);
}

void MainWindow::addToRecentFiles(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    QStringList recent = loadRecentFiles();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentFiles) {
        recent.removeLast();
    }
    storeRecentFiles(recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu) {
        return;
    }
    m_recentMenu->clear();

    const QStringList recent = loadRecentFiles();
    if (recent.isEmpty()) {
        auto *empty = m_recentMenu->addAction(tr("No recent files"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &path : recent) {
        const QString label = QFileInfo(path).fileName();
        auto *action = m_recentMenu->addAction(label);
        action->setToolTip(path);
        // Capture path by value for the lambda.
        connect(action, &QAction::triggered, this, [this, path] {
            openFilePath(path);
        });
    }
    m_recentMenu->addSeparator();
    auto *clearAction = m_recentMenu->addAction(tr("Clear recent files"));
    connect(clearAction, &QAction::triggered, this, [this] {
        storeRecentFiles({});
        rebuildRecentMenu();
    });
}

QStringList MainWindow::loadRecentFiles()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("recent"));
    QStringList files = settings.value(QStringLiteral("files")).toStringList();
    settings.endGroup();
    // Drop entries that no longer exist on disk.
    files.erase(std::remove_if(files.begin(), files.end(),
                                [](const QString &p) { return !QFileInfo::exists(p); }),
                files.end());
    return files;
}

void MainWindow::storeRecentFiles(const QStringList &paths)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("recent"));
    settings.setValue(QStringLiteral("files"), paths);
    settings.endGroup();
}

QString MainWindow::positionSettingsKey(const QString &path)
{
    // Paths contain '/' which QSettings treats as group separators -- encode.
    return QString::fromLatin1(
        path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

int MainWindow::loadSavedSentenceIndex(const QString &path)
{
    if (path.isEmpty()) {
        return -1;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("positions"));
    const QVariant v = settings.value(positionSettingsKey(path));
    settings.endGroup();
    if (!v.isValid()) {
        return -1;
    }
    bool ok = false;
    const int index = v.toInt(&ok);
    return ok && index >= 0 ? index : -1;
}

void MainWindow::storeSavedSentenceIndex(const QString &path, int sentenceIndex)
{
    if (path.isEmpty() || sentenceIndex < 0) {
        return;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("positions"));
    settings.setValue(positionSettingsKey(path), sentenceIndex);
    settings.endGroup();
}

void MainWindow::openDroppedUrls(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.isFile()) {
            continue;
        }
        openFilePath(path);
        return; // one document at a time
    }
    statusBar()->showMessage(tr("Drop a text or HTML file to open it"), 3000);
}
