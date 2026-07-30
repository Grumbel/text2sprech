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
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QIODevice>
#include <QStringConverter>
#include <QPixmap>
#include <QPainter>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QContextMenuEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QSizePolicy>
#include <QSize>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
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
#include "ExportAudioDialog.h"
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
    m_documentView->setExtraMarginPercent(m_options.extraMarginPercent);
    m_documentView->setExtraMarginsEnabled(true);
    m_documentView->setLineSpacing(m_options.lineSpacing);
    m_presentationFontFamily = m_options.presentationFontFamily;
    m_presentationFontPointSize = m_options.presentationFontPointSize;
    m_presentationExtraMarginPercent = m_options.presentationExtraMarginPercent;
    m_presentationLineSpacing = m_options.presentationLineSpacing;
    m_presentationColors = m_options.presentationColors;

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
        // Pin UI at the end of the document (last sentence, fraction 1.0).
        // Resetting fraction to 0 made the seek bar jump back to the start of
        // the last segment when playback completed.
        if (!m_sentences.isEmpty()) {
            m_currentSentenceIndex = m_sentences.size() - 1;
            m_intraSentenceFraction = 1.0;
            applyPositionHighlight(DocumentView::HighlightState::Stopped);
        }
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


void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange) {
        return;
    }
    // User left fullscreen via window manager (not our Exit action).
    if (!isFullScreen() && m_presentationMode && !m_leavingPresentation) {
        if (m_presentationAction) {
            QSignalBlocker blocker(m_presentationAction);
            m_presentationAction->setChecked(false);
        }
        setPresentationMode(false);
    }
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

    // --- Menu bar + toolbar (plain and conventional: File / Edit / View /
    // Settings menus, with a single classic toolbar underneath mirroring
    // the most-used actions). No custom widgets, no special layout tricks.
    auto themeIcon = [](const QString &name, const QIcon &fallback) {
        const QIcon icon = QIcon::fromTheme(name);
        return icon.isNull() ? fallback : icon;
    };
    // Prefer a theme glyph, then a list of alternates, then a painted fallback.
    auto themedOrPainted = [&themeIcon](const QStringList &names, const QIcon &painted) {
        for (const QString &name : names) {
            const QIcon icon = QIcon::fromTheme(name);
            if (!icon.isNull()) {
                return icon;
            }
        }
        return painted;
    };
    auto paintTextSizeIcon = [](bool larger) {
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(40, 40, 40), 3));
        QFont font(QStringLiteral("Sans Serif"));
        font.setBold(true);
        font.setPixelSize(larger ? 40 : 28);
        p.setFont(font);
        p.drawText(QRect(0, larger ? 4 : 12, 48, 48), Qt::AlignCenter, QStringLiteral("A"));
        p.setPen(QPen(QColor(40, 40, 40), 4, Qt::SolidLine, Qt::RoundCap));
        if (larger) {
            p.drawLine(46, 32, 58, 32);
            p.drawLine(52, 26, 52, 38);
        } else {
            p.drawLine(46, 32, 58, 32);
        }
        p.end();
        return QIcon(pm);
    };
    auto paintCopyIcon = []() {
        // Two overlapping sheets -- the standard "copy" metaphor, used as
        // a fallback when the icon theme doesn't ship edit-copy.
        QPixmap pm(64, 64);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(40, 40, 40), 3));
        p.setBrush(QColor(225, 225, 225));
        p.drawRoundedRect(10, 14, 34, 42, 3, 3);
        p.setBrush(QColor(250, 250, 250));
        p.drawRoundedRect(20, 6, 34, 42, 3, 3);
        p.end();
        return QIcon(pm);
    };

    // --- Open (+ Recent), Copy, and Paste -----------------------------------
    m_openAction = new QAction(
        themeIcon(QStringLiteral("document-open"), style()->standardIcon(QStyle::SP_DialogOpenButton)),
        tr("&Open…"), this);
    m_openAction->setToolTip(tr("Open a text or HTML file (or drop one onto the window)"));
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openFile);
    addAction(m_openAction);

    m_recentMenu = new QMenu(tr("Open &Recent"), this);
    rebuildRecentMenu();

    connect(m_documentView, &DocumentView::filesDropped, this, &MainWindow::openDroppedUrls);

    auto *copyAction = new QAction(
        themedOrPainted({QStringLiteral("edit-copy")}, paintCopyIcon()),
        tr("&Copy"), this);
    copyAction->setToolTip(tr("Copy the selected text (Ctrl+C)"));
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(false); // like any editor: nothing selected yet
    connect(copyAction, &QAction::triggered, m_documentView, &DocumentView::copy);
    connect(m_documentView, &DocumentView::copyAvailable, copyAction, &QAction::setEnabled);

    // Plain paste from the clipboard only -- the primary-selection variant
    // was dropped from the UI. Middle-click still pastes the primary
    // selection directly in the document view (a separate, mouse-driven
    // X11/Wayland convention, not exposed as a menu/toolbar action).
    auto *pasteClipboardAction = new QAction(
        themeIcon(QStringLiteral("edit-paste"), style()->standardIcon(QStyle::SP_DialogResetButton)),
        tr("&Paste"), this);
    pasteClipboardAction->setToolTip(tr("Paste from the clipboard (Ctrl+V)"));
    // No QAction shortcut here: Ctrl+V is intercepted directly in
    // DocumentView::keyPressEvent() (see pasteRequested below), since a
    // window-level shortcut would race/duplicate that handling.
    connect(pasteClipboardAction, &QAction::triggered, this, [this] {
        pasteFromSource(QClipboard::Clipboard);
    });

    auto *saveAsAction = new QAction(
        themeIcon(QStringLiteral("document-save-as"), style()->standardIcon(QStyle::SP_DialogSaveButton)),
        tr("Save &As…"), this);
    saveAsAction->setToolTip(tr("Save the current document text to a file"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveDocumentAs);
    addAction(saveAsAction);

    auto *exportAudioAction = new QAction(
        themeIcon(QStringLiteral("audio-x-generic"), style()->standardIcon(QStyle::SP_MediaVolume)),
        tr("Export &Audio…"), this);
    exportAudioAction->setToolTip(tr("Synthesize the document to a WAV file"));
    connect(exportAudioAction, &QAction::triggered, this, &MainWindow::exportAudio);
    addAction(exportAudioAction);

    auto *quitAction = new QAction(
        themeIcon(QStringLiteral("application-exit"), style()->standardIcon(QStyle::SP_DialogCloseButton)),
        tr("&Quit"), this);
    quitAction->setToolTip(tr("Quit text2sprech"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // --- Text size + margins ------------------------------------------------
    // "A−" / "A+" are recognizable from any text editor; zoom-out/zoom-in
    // is the standard freedesktop icon pair most themes ship for this.
    auto *decreaseTextSizeAction = new QAction(
        themedOrPainted(
            {QStringLiteral("zoom-out"), QStringLiteral("format-text-smaller"),
             QStringLiteral("list-remove")},
            paintTextSizeIcon(false)),
        tr("&Decrease Text Size"), this);
    decreaseTextSizeAction->setToolTip(tr("Decrease text size (Ctrl+−)"));
    decreaseTextSizeAction->setShortcut(QKeySequence::ZoomOut);
    connect(decreaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::decreaseTextSize);

    auto *increaseTextSizeAction = new QAction(
        themedOrPainted(
            {QStringLiteral("zoom-in"), QStringLiteral("format-text-larger"),
             QStringLiteral("list-add")},
            paintTextSizeIcon(true)),
        tr("&Increase Text Size"), this);
    increaseTextSizeAction->setToolTip(tr("Increase text size (Ctrl++)"));
    increaseTextSizeAction->setShortcuts(
        {QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    connect(increaseTextSizeAction, &QAction::triggered, m_documentView, &DocumentView::increaseTextSize);

    // Margin widen/narrow (±5% of view width each side). Session only —
    // startup default remains Preferences / --margin / config.
    auto adjustSessionMargin = [this](int delta) {
        if (m_presentationMode) {
            m_presentationExtraMarginPercent = qBound(
                DocumentView::kMinExtraMarginPercent,
                m_presentationExtraMarginPercent + delta,
                DocumentView::kMaxExtraMarginPercent);
            applyLivePresentationAppearance();
            statusBar()->showMessage(
                tr("Presentation margins: %1% each side")
                    .arg(m_presentationExtraMarginPercent),
                2500);
            return;
        }
        const int next = qBound(DocumentView::kMinExtraMarginPercent,
                               m_documentView->extraMarginPercent() + delta,
                               DocumentView::kMaxExtraMarginPercent);
        m_documentView->setExtraMarginsEnabled(true);
        m_documentView->setExtraMarginPercent(next);
        statusBar()->showMessage(tr("Margins: %1% each side").arg(next), 2500);
    };

    auto *decreaseMarginAction = new QAction(
        themedOrPainted(
            {QStringLiteral("format-indent-less"), QStringLiteral("go-previous"),
             QStringLiteral("object-flip-horizontal")},
            style()->standardIcon(QStyle::SP_ArrowLeft)),
        tr("Decrease &Margins"), this);
    decreaseMarginAction->setToolTip(
        tr("Narrow side margins by %1% (Ctrl+[)").arg(DocumentView::kExtraMarginStepPercent));
    decreaseMarginAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    connect(decreaseMarginAction, &QAction::triggered, this, [adjustSessionMargin] {
        adjustSessionMargin(-DocumentView::kExtraMarginStepPercent);
    });
    addAction(decreaseMarginAction);

    auto *increaseMarginAction = new QAction(
        themedOrPainted(
            {QStringLiteral("format-indent-more"), QStringLiteral("go-next"),
             QStringLiteral("object-flip-horizontal")},
            style()->standardIcon(QStyle::SP_ArrowRight)),
        tr("Increase &Margins"), this);
    increaseMarginAction->setToolTip(
        tr("Widen side margins by %1% (Ctrl+])").arg(DocumentView::kExtraMarginStepPercent));
    increaseMarginAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    connect(increaseMarginAction, &QAction::triggered, this, [adjustSessionMargin] {
        adjustSessionMargin(DocumentView::kExtraMarginStepPercent);
    });
    addAction(increaseMarginAction);

    // Follow the speaking sentence in the viewport (default on). Uncheck to
    // scroll freely while audio continues — highlight still updates in place.
    auto *autoScrollAction = new QAction(
        themedOrPainted(
            {QStringLiteral("go-bottom"), QStringLiteral("view-scroll-bottom"),
             QStringLiteral("transform-move")},
            style()->standardIcon(QStyle::SP_ArrowDown)),
        tr("Auto-&Scroll"), this);
    autoScrollAction->setCheckable(true);
    autoScrollAction->setChecked(true);
    autoScrollAction->setToolTip(
        tr("Keep the current sentence centered while playing. Turn off to "
           "browse the document without the view jumping."));
    connect(autoScrollAction, &QAction::toggled, this, [this](bool on) {
        m_documentView->setAutoScrollEnabled(on);
        statusBar()->showMessage(
            on ? tr("Auto-scroll on — following the current sentence")
               : tr("Auto-scroll off — scroll freely while listening"),
            3000);
    });
    addAction(autoScrollAction);

    m_presentationAction = new QAction(
        themedOrPainted(
            {QStringLiteral("view-fullscreen"), QStringLiteral("view-fullscreen-symbolic")},
            style()->standardIcon(QStyle::SP_TitleBarMaxButton)),
        tr("&Presentation Mode"), this);
    m_presentationAction->setCheckable(true);
    m_presentationAction->setChecked(false);
    m_presentationAction->setToolTip(
        tr("Fullscreen teleprompter view (F11). Tab shows or hides controls; "
           "Esc exits."));
    m_presentationAction->setShortcut(Qt::Key_F11);
    connect(m_presentationAction, &QAction::toggled, this, &MainWindow::setPresentationMode);
    addAction(m_presentationAction);

    // Application-wide so Tab still works when focus is on the toolbar /
    // player after chrome is revealed (DocumentView keyPressEvent only
    // runs when the document has focus).
    m_presentationTabShortcut = new QShortcut(Qt::Key_Tab, this);
    m_presentationTabShortcut->setContext(Qt::ApplicationShortcut);
    m_presentationTabShortcut->setEnabled(false);
    connect(m_presentationTabShortcut, &QShortcut::activated, this,
            &MainWindow::togglePresentationChrome);
    m_presentationEscShortcut = new QShortcut(Qt::Key_Escape, this);
    m_presentationEscShortcut->setContext(Qt::ApplicationShortcut);
    m_presentationEscShortcut->setEnabled(false);
    connect(m_presentationEscShortcut, &QShortcut::activated, this, [this] {
        if (m_presentationMode && m_presentationAction) {
            m_presentationAction->setChecked(false);
        }
    });

    // --- Find toggle ----------------------------------------------------
    m_findAction = new QAction(
        themeIcon(QStringLiteral("edit-find"), style()->standardIcon(QStyle::SP_FileDialogContentsView)),
        tr("&Find…"), this);
    m_findAction->setToolTip(tr("Show or hide the find bar (Ctrl+F)"));
    m_findAction->setCheckable(true);
    m_findAction->setChecked(false);
    m_findAction->setShortcut(QKeySequence::Find);
    // Use toggled so the button acts as a show/hide switch.
    connect(m_findAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            showFindBar();
        } else {
            hideFindBar();
        }
    });
    addAction(m_findAction);

    // --- Preferences ------------------------------------------------------
    auto *preferencesAction = new QAction(
        themeIcon(QStringLiteral("preferences-system"), style()->standardIcon(QStyle::SP_FileDialogDetailedView)),
        tr("&Preferences…"), this);
    preferencesAction->setToolTip(tr("Startup defaults (voice, speed, font, colors)"));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::openPreferences);

    // --- About --------------------------------------------------------------
    auto *aboutAction = new QAction(tr("&About text2sprech…"), this);
    connect(aboutAction, &QAction::triggered, this, &MainWindow::openAbout);

    auto *aboutQtAction = new QAction(tr("About &Qt…"), this);
    connect(aboutQtAction, &QAction::triggered, this, [this] { QMessageBox::aboutQt(this); });

    // --- Menu bar: File / Edit / View / Settings / Help ---------------------
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_openAction);
    fileMenu->addMenu(m_recentMenu);
    fileMenu->addSeparator();
    fileMenu->addAction(saveAsAction);
    fileMenu->addAction(exportAudioAction);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(copyAction);
    editMenu->addAction(pasteClipboardAction);
    editMenu->addSeparator();
    editMenu->addAction(m_findAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(increaseTextSizeAction);
    viewMenu->addAction(decreaseTextSizeAction);
    viewMenu->addSeparator();
    viewMenu->addAction(increaseMarginAction);
    viewMenu->addAction(decreaseMarginAction);
    viewMenu->addSeparator();
    viewMenu->addAction(autoScrollAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_presentationAction);

    QMenu *settingsMenu = menuBar()->addMenu(tr("&Settings"));
    settingsMenu->addAction(preferencesAction);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(aboutAction);
    helpMenu->addAction(aboutQtAction);

    // --- Toolbar: one plain row mirroring the most-used actions -----------
    m_toolBar = addToolBar(tr("Main Toolbar"));
    m_toolBar->setObjectName(QStringLiteral("mainToolBar"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_toolBar->setIconSize(QSize(20, 20));

    m_toolBar->addAction(m_openAction);
    if (auto *openButton = qobject_cast<QToolButton *>(m_toolBar->widgetForAction(m_openAction))) {
        openButton->setMenu(m_recentMenu);
        openButton->setPopupMode(QToolButton::MenuButtonPopup);
    }
    m_toolBar->addAction(saveAsAction);
    m_toolBar->addAction(exportAudioAction);
    m_toolBar->addAction(copyAction);
    m_toolBar->addAction(pasteClipboardAction);
    m_toolBar->addSeparator();

    m_toolBar->addWidget(new QLabel(tr("Voice: "), m_toolBar));
    m_voiceCombo = new QComboBox(m_toolBar);
    m_voiceCombo->setMinimumWidth(150);
    m_voiceCombo->setMaximumWidth(260);
    m_voiceCombo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_voiceCombo->setToolTip(tr("TTS voice for this session"));
    connect(m_voiceCombo, &QComboBox::currentTextChanged, this, [this](const QString &voice) {
        m_playback->setVoice(voice);
    });
    m_toolBar->addWidget(m_voiceCombo);

    m_toolBar->addWidget(new QLabel(tr("  Speed: "), m_toolBar));
    m_speedSpin = new QDoubleSpinBox(m_toolBar);
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
    m_toolBar->addWidget(m_speedSpin);

    m_toolBar->addSeparator();
    m_toolBar->addAction(decreaseTextSizeAction);
    m_toolBar->addAction(increaseTextSizeAction);
    m_toolBar->addAction(decreaseMarginAction);
    m_toolBar->addAction(increaseMarginAction);
    m_toolBar->addAction(autoScrollAction);

    m_toolBar->addSeparator();
    m_toolBar->addAction(m_findAction);

    m_toolBar->addSeparator();
    m_toolBar->addAction(preferencesAction);

    // Push presentation/fullscreen to the far right of the toolbar.
    auto *toolBarSpacer = new QWidget(m_toolBar);
    toolBarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(toolBarSpacer);
    m_toolBar->addAction(m_presentationAction);

    connect(m_documentView, &DocumentView::previousSentenceRequested, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex - 1);
    });
    connect(m_documentView, &DocumentView::nextSentenceRequested, this, [this] {
        jumpToSentence(m_currentSentenceIndex < 0 ? 0 : m_currentSentenceIndex + 1);
    });
    connect(m_documentView, &DocumentView::pasteRequested, this, [this](QClipboard::Mode mode) {
        pasteFromSource(mode);
    });
    connect(m_documentView, &DocumentView::presentationChromeToggleRequested, this,
            &MainWindow::togglePresentationChrome);
    connect(m_documentView, &DocumentView::presentationExitRequested, this, [this] {
        if (m_presentationMode) {
            m_presentationAction->setChecked(false); // routes through setPresentationMode(false)
        }
    });
    m_documentView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_documentView, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        showDocumentContextMenu(m_documentView->mapToGlobal(pos));
    });

    // --- Bottom player bar -------------------------------------------------
    // Layout matches the reference chrome:
    //   [elapsed] ———— progress slider ———— [−remaining] [N / M]
    //              Previous   Play   Stop   Next   (icons + labels)
    // Status bar (below): ● Ready
    m_playerBar = new QWidget(central);
    m_playerBar->setObjectName(QStringLiteral("playerBar"));
    auto *playerColumn = new QVBoxLayout(m_playerBar);
    playerColumn->setContentsMargins(12, 8, 12, 4);
    playerColumn->setSpacing(4);

    // Progress row: times + seek track + sentence counter on one line.
    auto *progressRow = new QWidget(m_playerBar);
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
        // Groove clicks jump via ClickJumpSlider before press; mirror that
        // position in the time labels and fine-grained text highlight.
        previewProgressUnit(m_progressSlider->value());
    });
    connect(m_progressSlider, &QSlider::sliderReleased, this, [this] {
        m_progressSliderPressed = false;
        onProgressSliderCommitted(m_progressSlider->value());
    });
    connect(m_progressSlider, &QSlider::sliderMoved, this, [this](int value) {
        // Live preview while dragging: time labels + in-sentence highlight.
        // Audio seeks only on release so we do not thrash synthesis.
        previewProgressUnit(value);
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


    QFont metaFont = m_elapsedLabel->font();
    metaFont.setPointSizeF(metaFont.pointSizeF() * 0.95);
    m_elapsedLabel->setFont(metaFont);
    m_remainingLabel->setFont(metaFont);

    progressLayout->addWidget(m_elapsedLabel, 0, Qt::AlignVCenter);
    progressLayout->addWidget(m_progressSlider, /*stretch=*/1);
    progressLayout->addWidget(m_remainingLabel, 0, Qt::AlignVCenter);
    playerColumn->addWidget(progressRow);

    // Transport row: icon-only Previous / Play / Stop / Next, truly centered.
    auto *transportRow = new QWidget(m_playerBar);
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
        setPlayingUi(false);
        m_intraSentenceFraction = 0.0; // stop() restarts the sentence from 0 next play
        applyPositionHighlight(DocumentView::HighlightState::Stopped);
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
    centralLayout->addWidget(m_playerBar);
    setCentralWidget(central);

    // Status bar: green (or state-colored) dot + activity text on the left
    // (reference "● Ready"), word/byte counts permanently on the right.
    // Transient statusBar()->showMessage() popups still temporarily cover
    // the left normal area; permanent widgets on the right stay visible.
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

    m_documentStatsLabel = new QLabel(this);
    m_documentStatsLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_documentStatsLabel->setContentsMargins(8, 0, 8, 0);
    m_documentStatsLabel->setToolTip(tr("Word count and UTF-8 byte size of the document"));
    statusBar()->addPermanentWidget(m_documentStatsLabel);
    setActivity(tr("Starting…"), ActivityKind::Idle);

    // Visible immediately while the piper-server connects; replaced if
    // startup paste/file loads, otherwise stays until the user opens/pastes.
    showWelcomeMessage();
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

void MainWindow::setUpShortcuts()
{
    auto *playPause = new QAction(this);
    playPause->setShortcut(Qt::Key_Space);
    connect(playPause, &QAction::triggered, this, &MainWindow::togglePlayPause);
    addAction(playPause);

    // Speed: ] / > faster; [ / < slower (mpv brackets + YouTube angle keys).
    // Bare + / − are not bound here — they collide with Ctrl++ / Ctrl+− zoom
    // (ZoomIn/ZoomOut) as ambiguous overloads on some layouts.
    auto *speedUp = new QAction(this);
    speedUp->setShortcuts({Qt::Key_BracketRight, Qt::Key_Greater});
    connect(speedUp, &QAction::triggered, this, [this] { m_speedSpin->stepUp(); });
    addAction(speedUp);

    auto *speedDown = new QAction(this);
    speedDown->setShortcuts({Qt::Key_BracketLeft, Qt::Key_Less});
    connect(speedDown, &QAction::triggered, this, [this] { m_speedSpin->stepDown(); });
    addAction(speedDown);

    // Text zoom shortcuts live only on the View menu / toolbar actions
    // (ZoomIn / ZoomOut / Ctrl+=). A second pair here caused
    // "Ambiguous shortcut overload: Ctrl++ / Ctrl+-".

    // Quit (Ctrl+Q / Cmd+Q) now lives on the File menu's Quit action, set up
    // in setUpUi() — same shortcut, goes through closeEvent() either way so
    // piper-server shutdown and socket cleanup still run.

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

void MainWindow::saveDocumentAs()
{
    // Prefer the form the document was loaded as. HTML is the sanitized
    // semantic markup we stored on load (not Qt's toHtml() dump). Choosing
    // a .txt path still allows a plain-text export when the user wants it.
    const QString baseName = m_currentDocumentPath.isEmpty()
        ? QStringLiteral("document")
        : QFileInfo(m_currentDocumentPath).completeBaseName();
    const QString suggested = m_documentIsHtml
        ? (baseName + QStringLiteral(".html"))
        : (baseName + QStringLiteral(".txt"));
    const QString filter = m_documentIsHtml
        ? tr("HTML files (*.html *.htm);;Text files (*.txt);;All files (*)")
        : tr("Text files (*.txt);;HTML files (*.html *.htm);;All files (*)");
    const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggested, filter);
    if (path.isEmpty()) {
        return;
    }

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool saveAsHtml = (suffix == QLatin1String("html") || suffix == QLatin1String("htm"))
        || (suffix.isEmpty() && m_documentIsHtml);

    QString payload;
    if (saveAsHtml) {
        if (m_documentIsHtml && !m_documentSource.isEmpty()) {
            payload = m_documentSource;
        } else {
            // Plain-text document saved with an .html name: wrap minimally.
            const QString plain = m_documentView->document()->toPlainText();
            payload = QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head>"
                                     "<body><pre>%1</pre></body></html>\n")
                          .arg(plain.toHtmlEscaped());
        }
    } else {
        // Explicit .txt (or other): plain text of the rendered document.
        payload = m_documentView->document()->toPlainText();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Save failed"), file.errorString());
        return;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << payload;
    if (stream.status() != QTextStream::Ok) {
        QMessageBox::warning(this, tr("Save failed"), tr("Could not write the full document."));
        return;
    }
    const QString resolved = canonicalizePath(path);
    setCurrentDocumentPath(resolved.isEmpty() ? path : resolved);
    // Track format so the next Save As defaults match what we just wrote.
    m_documentIsHtml = saveAsHtml;
    m_documentSource = payload;
    if (!m_currentDocumentPath.isEmpty()) {
        addToRecentFiles(m_currentDocumentPath);
        setWindowTitle(tr("%1 — text2sprech").arg(QFileInfo(m_currentDocumentPath).fileName()));
        saveCurrentPosition();
    }
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::exportAudio()
{
    if (!m_realAudioAvailable) {
        QMessageBox::warning(
            this, tr("Export Audio"),
            tr("Piper TTS audio is not available. Install Piper and voice models first."));
        return;
    }
    if (!m_piperClient || !m_piperClient->isConnected()) {
        QMessageBox::warning(this, tr("Export Audio"),
                             tr("Not connected to the Piper server."));
        return;
    }
    if (m_sentences.isEmpty()) {
        QMessageBox::information(this, tr("Export Audio"),
                                 tr("Open or paste a document with speakable text first."));
        return;
    }

    // Suspend playback so export owns PiperClient responses exclusively.
    const QString voice = m_voiceCombo ? m_voiceCombo->currentText() : QString();
    const double speed = m_speedSpin ? m_speedSpin->value() : 1.0;
    const int startIndex = m_currentSentenceIndex >= 0 ? m_currentSentenceIndex : 0;

    setPlayingUi(false);
    m_playback->setSuspended(true);

    ExportAudioDialog dialog(m_piperClient, m_sentences, startIndex, voice, speed, m_knownVoices,
                             this);
    dialog.exec();

    // Restore session voice/speed on the server (export may have changed them)
    // and allow playback to request synthesis again.
    if (!voice.isEmpty()) {
        m_playback->setVoice(voice);
    }
    m_playback->setSpeed(speed);
    m_playback->setSuspended(false);
    statusBar()->showMessage(tr("Export dialog closed"), 2500);
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
             tr("Choose <b>Voice</b> and <b>Speed</b> in the Voice group for this session. "
                "<b>Preferences</b> previews changes live; Save writes them as "
                "startup defaults."),
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

    m_documentIsHtml = isHtml;
    if (isHtml) {
        // Reader-mode-ish subset: headings, paragraphs, lists, emphasis,
        // code, links — no colours, font sizes, families, or images.
        // Keep the sanitized HTML for Save As so markup is not stripped.
        m_documentSource = HtmlSanitizer::toSemanticHtml(content);
        m_documentView->loadHtml(m_documentSource);
    } else {
        m_documentSource = content;
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
    refreshDocumentStats();
}

void MainWindow::refreshDocumentStats()
{
    if (!m_documentStatsLabel) {
        return;
    }
    const QString plain = m_documentView ? m_documentView->document()->toPlainText() : QString();
    const int words = countWords(plain);
    const qsizetype bytes = plain.toUtf8().size();
    // Compact right-side permanent status: "1,234 words · 5,678 bytes"
    m_documentStatsLabel->setText(
        tr("%1 words · %2 bytes")
            .arg(words)
            .arg(bytes));
    m_documentStatsLabel->setToolTip(
        tr("%1 words, %2 UTF-8 bytes in the current document")
            .arg(words)
            .arg(bytes));
}

QSettings MainWindow::makeSettings() const
{
    if (!m_options.configPath.isEmpty()) {
        return QSettings(m_options.configPath, QSettings::IniFormat);
    }
    return QSettings();
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
        // Keep progress highlight, muted stopped color (not actively speaking).
        applyPositionHighlight(DocumentView::HighlightState::Stopped);
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

DocumentView::Colors MainWindow::resolvedPresentationColors(const DocumentView::Colors &overrides)
{
    const DocumentView::Colors builtins = DocumentView::presentationDefaults();
    DocumentView::Colors c;
    c.text = overrides.text.isValid() ? overrides.text : builtins.text;
    c.background = overrides.background.isValid() ? overrides.background : builtins.background;
    c.playing = overrides.playing.isValid() ? overrides.playing : builtins.playing;
    c.preparing = overrides.preparing.isValid() ? overrides.preparing : builtins.preparing;
    c.stopped = overrides.stopped.isValid() ? overrides.stopped : builtins.stopped;
    c.hover = overrides.hover.isValid() ? overrides.hover : builtins.hover;
    return c;
}

void MainWindow::applyLivePresentationAppearance()
{
    if (!m_documentView) {
        return;
    }
    m_documentView->setColors(resolvedPresentationColors(m_presentationColors));
    // Family: dedicated presentation family if set, otherwise the reading
    // font that was active when presentation mode was entered (or current
    // base font when applying from Preferences while already in mode).
    QFont presentationFont = m_presentationMode ? m_savedFont : m_documentView->baseFont();
    if (!m_presentationFontFamily.isEmpty()) {
        presentationFont.setFamily(m_presentationFontFamily);
    }
    const int pt = m_presentationFontPointSize > 0 ? m_presentationFontPointSize
                                                   : DocumentView::kPresentationFontPointSize;
    presentationFont.setPointSize(pt);
    m_documentView->setBaseFont(presentationFont);
    m_documentView->setExtraMarginPercent(m_presentationExtraMarginPercent);
    m_documentView->setExtraMarginsEnabled(true);
    m_documentView->setLineSpacing(m_presentationLineSpacing);
}

void MainWindow::setPresentationMode(bool on)
{
    if (on == m_presentationMode) {
        // Still sync chrome if re-entering with the same flag (e.g. action
        // re-checked after an external fullscreen exit already cleaned up).
        if (on) {
            applyPresentationChromeVisibility();
        }
        return;
    }

    if (on) {
        m_savedColors = m_documentView->colors();
        m_savedFont = m_documentView->baseFont();
        m_savedMarginPercent = m_documentView->extraMarginPercent();
        m_savedLineSpacing = m_documentView->lineSpacing();

        m_presentationMode = true;
        m_presentationChromeVisible = false;
        m_documentView->setPresentationKeysEnabled(true);
        if (m_presentationTabShortcut) {
            m_presentationTabShortcut->setEnabled(true);
        }
        if (m_presentationEscShortcut) {
            m_presentationEscShortcut->setEnabled(true);
        }
        applyLivePresentationAppearance();

        showFullScreen();
        applyPresentationChromeVisibility();
        updatePresentationActionIcon();
        m_documentView->setFocus(Qt::OtherFocusReason);
        statusBar()->showMessage(
            tr("Presentation mode — Tab: controls · Esc: exit · F11: toggle"), 5000);
    } else {
        m_leavingPresentation = true;
        m_presentationMode = false;
        m_presentationChromeVisible = false;
        m_documentView->setPresentationKeysEnabled(false);
        if (m_presentationTabShortcut) {
            m_presentationTabShortcut->setEnabled(false);
        }
        if (m_presentationEscShortcut) {
            m_presentationEscShortcut->setEnabled(false);
        }

        m_documentView->setColors(m_savedColors);
        m_documentView->setBaseFont(m_savedFont);
        m_documentView->setExtraMarginPercent(m_savedMarginPercent);
        m_documentView->setExtraMarginsEnabled(true);
        m_documentView->setLineSpacing(m_savedLineSpacing);

        if (isFullScreen()) {
            showNormal();
        }
        applyPresentationChromeVisibility();
        updatePresentationActionIcon();
        m_leavingPresentation = false;
        statusBar()->showMessage(tr("Left presentation mode"), 2500);
    }
}

void MainWindow::applyPresentationChromeVisibility()
{
    const bool showChrome = !m_presentationMode || m_presentationChromeVisible;

    if (menuBar()) {
        menuBar()->setVisible(showChrome);
    }
    if (m_toolBar) {
        m_toolBar->setVisible(showChrome);
    }
    if (m_playerBar) {
        m_playerBar->setVisible(showChrome);
    }
    if (statusBar()) {
        statusBar()->setVisible(showChrome);
    }
    // Warning / find stay hidden in presentation unless chrome is up, and
    // find stays hidden unless the user opened it.
    if (m_warningBanner) {
        const bool hasWarning = !m_warningBanner->text().isEmpty();
        m_warningBanner->setVisible(showChrome && hasWarning);
    }
    if (m_findBar && !showChrome) {
        m_findBar->setVisible(false);
        if (m_findAction && m_findAction->isChecked()) {
            QSignalBlocker blocker(m_findAction);
            m_findAction->setChecked(false);
        }
    }
}

void MainWindow::togglePresentationChrome()
{
    if (!m_presentationMode) {
        return;
    }
    m_presentationChromeVisible = !m_presentationChromeVisible;
    applyPresentationChromeVisibility();
    // Always return focus to the document so further Tab presses are not
    // swallowed by toolbar/player widgets (and so typing still targets the
    // reader). ApplicationShortcut also covers Tab, but focus matters for
    // arrows / Space.
    m_documentView->setFocus(Qt::OtherFocusReason);
    if (m_presentationChromeVisible) {
        statusBar()->showMessage(tr("Controls visible — Tab to hide again"), 3000);
    }
}

void MainWindow::updatePresentationActionIcon()
{
    if (!m_presentationAction) {
        return;
    }
    const QIcon fullscreenIcon = QIcon::fromTheme(QStringLiteral("view-fullscreen"));
    const QIcon restoreIcon = QIcon::fromTheme(QStringLiteral("view-restore"));
    if (m_presentationMode) {
        m_presentationAction->setIcon(
            restoreIcon.isNull()
                ? (fullscreenIcon.isNull() ? style()->standardIcon(QStyle::SP_TitleBarNormalButton)
                                           : fullscreenIcon)
                : restoreIcon);
        m_presentationAction->setText(tr("Exit &Presentation Mode"));
        m_presentationAction->setToolTip(
            tr("Leave presentation mode (F11 or Esc)"));
    } else {
        m_presentationAction->setIcon(
            fullscreenIcon.isNull() ? style()->standardIcon(QStyle::SP_TitleBarMaxButton)
                                     : fullscreenIcon);
        m_presentationAction->setText(tr("&Presentation Mode"));
        m_presentationAction->setToolTip(
            tr("Fullscreen teleprompter view (F11). Tab shows or hides controls; "
               "Esc exits."));
    }
}

void MainWindow::showDocumentContextMenu(const QPoint &globalPos)
{
    QMenu menu(this);

    auto *copyAct = menu.addAction(tr("&Copy"));
    copyAct->setEnabled(m_documentView->textCursor().hasSelection());
    connect(copyAct, &QAction::triggered, m_documentView, &DocumentView::copy);

    menu.addSeparator();

    if (m_playPauseAction) {
        menu.addAction(m_playPauseAction);
    }

    menu.addSeparator();

    if (m_presentationMode) {
        auto *chromeAct = menu.addAction(m_presentationChromeVisible
                                             ? tr("Hide &Controls")
                                             : tr("Show &Controls"));
        connect(chromeAct, &QAction::triggered, this, &MainWindow::togglePresentationChrome);

        auto *exitAct = menu.addAction(tr("Exit &Presentation Mode"));
        connect(exitAct, &QAction::triggered, this, [this] {
            if (m_presentationAction) {
                m_presentationAction->setChecked(false);
            }
        });
    } else {
        if (m_presentationAction) {
            menu.addAction(m_presentationAction);
        }
    }

    menu.exec(globalPos);
}

void MainWindow::onSentenceStarted(int sentenceId, int start, int end)
{
    Q_UNUSED(start);
    Q_UNUSED(end);
    m_currentSentenceIndex = sentenceId; // ids are assigned sequentially == index into m_sentences
    m_intraSentenceFraction = m_playback->currentFraction();
    applyPositionHighlight(DocumentView::HighlightState::Playing);
    setActivity(tr("Playing"), ActivityKind::Active);
    refreshProgress();
    saveCurrentPosition();
}

void MainWindow::onAudioPositionChanged(qint64 positionMs, qint64 durationMs)
{
    // While the user is dragging the seek bar, previewProgressUnit owns the
    // highlight and fraction; ignore the player clock.
    if (m_progressSliderPressed) {
        return;
    }

    if (durationMs > 0) {
        const double fraction = qBound(0.0, double(positionMs) / double(durationMs), 1.0);
        // After stop/finish the player often reports position 0; do not let
        // that pull a completed or mid-segment parked position back to start.
        if (!m_playback->isPlaying() && positionMs == 0 && m_intraSentenceFraction > 0.01) {
            return;
        }
        m_intraSentenceFraction = fraction;
    }

    if (m_currentSentenceIndex >= 0) {
        const auto state = m_playback->isPlaying()
            ? DocumentView::HighlightState::Playing
            : DocumentView::HighlightState::Stopped;
        applyPositionHighlight(state);
    }
    refreshProgress();
}

void MainWindow::onPreparingAudio(int sentenceId)
{
    if (sentenceId >= 0 && sentenceId < m_sentences.size()) {
        m_currentSentenceIndex = sentenceId;
        applyPositionHighlight(DocumentView::HighlightState::Preparing);
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
    const int index = sentenceIndexForOffset(plainTextOffset);
    const Sentence &s = m_sentences[index];
    double fraction = 0.0;
    if (s.end > s.start) {
        fraction = qBound(0.0, double(plainTextOffset - s.start) / double(s.end - s.start), 1.0);
    }
    jumpToSentence(index, fraction);
}

void MainWindow::applyPositionHighlight(DocumentView::HighlightState state)
{
    if (m_currentSentenceIndex < 0 || m_currentSentenceIndex >= m_sentences.size()) {
        return;
    }
    const Sentence &s = m_sentences[m_currentSentenceIndex];
    m_documentView->highlightSentenceProgress(s.start, s.end, m_intraSentenceFraction, state);
}

void MainWindow::jumpToSentence(int index, double fractionWithin)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    index = std::clamp(index, 0, static_cast<int>(m_sentences.size()) - 1);
    fractionWithin = qBound(0.0, fractionWithin, 1.0);
    // Immediate visual feedback regardless of playback state -- if we're
    // paused/stopped, seekToSentence() below won't emit sentenceStarted
    // (that only fires once audio actually starts), so without this a
    // click or arrow-key press would appear to do nothing until Play.
    // Color: Preparing if playback is already running (synth will follow),
    // otherwise the muted Stopped/selected state.
    const auto state = m_playback->isPlaying()
        ? DocumentView::HighlightState::Preparing
        : DocumentView::HighlightState::Stopped;
    m_currentSentenceIndex = index;
    m_intraSentenceFraction = fractionWithin;
    applyPositionHighlight(state);
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
    if (m_findAction && !m_findAction->isChecked()) {
        QSignalBlocker blocker(m_findAction);
        m_findAction->setChecked(true);
    }
    m_findEdit->setFocus(Qt::ShortcutFocusReason);
    m_findEdit->selectAll();
}

void MainWindow::hideFindBar()
{
    if (!m_findBar) {
        return;
    }
    m_findBar->setVisible(false);
    if (m_findAction && m_findAction->isChecked()) {
        QSignalBlocker blocker(m_findAction);
        m_findAction->setChecked(false);
    }
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
    QSettings settings = makeSettings();
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
    const int marginVal = settings.value(QStringLiteral("extraMarginPercent"),
                                         DocumentView::kDefaultExtraMarginPercent)
                              .toInt(&marginOk);
    const int defaultExtraMarginPercent =
        (marginOk && marginVal >= DocumentView::kMinExtraMarginPercent
         && marginVal <= DocumentView::kMaxExtraMarginPercent)
            ? marginVal
            : DocumentView::kDefaultExtraMarginPercent;
    bool lineOk = false;
    const double lineVal = settings.value(QStringLiteral("lineSpacing"),
                                          DocumentView::kDefaultLineSpacing)
                               .toDouble(&lineOk);
    const double defaultLineSpacing =
        (lineOk && lineVal >= DocumentView::kMinLineSpacing
         && lineVal <= DocumentView::kMaxLineSpacing)
            ? lineVal
            : DocumentView::kDefaultLineSpacing;
    settings.endGroup();

    settings.beginGroup(QStringLiteral("voices"));
    const QStringList extraVoiceDirs = settings.value(QStringLiteral("extraDirs")).toStringList();
    settings.endGroup();

    PresentationDefaults presentationDefaults;
    settings.beginGroup(QStringLiteral("presentation"));
    bool presSizeOk = false;
    const int presPs = settings.value(QStringLiteral("fontPointSize"),
                                      DocumentView::kPresentationFontPointSize)
                           .toInt(&presSizeOk);
    presentationDefaults.fontFamily =
        settings.value(QStringLiteral("fontFamily")).toString().trimmed();
    presentationDefaults.fontPointSize =
        (presSizeOk && presPs >= 6 && presPs <= 72) ? presPs
                                                    : DocumentView::kPresentationFontPointSize;
    bool presMarginOk = false;
    const int presMargin = settings.value(QStringLiteral("extraMarginPercent"),
                                          DocumentView::kDefaultExtraMarginPercent)
                               .toInt(&presMarginOk);
    presentationDefaults.extraMarginPercent =
        (presMarginOk && presMargin >= DocumentView::kMinExtraMarginPercent
         && presMargin <= DocumentView::kMaxExtraMarginPercent)
            ? presMargin
            : DocumentView::kDefaultExtraMarginPercent;
    bool presLineOk = false;
    const double presLine = settings.value(QStringLiteral("lineSpacing"),
                                           DocumentView::kDefaultPresentationLineSpacing)
                                .toDouble(&presLineOk);
    presentationDefaults.lineSpacing =
        (presLineOk && presLine >= DocumentView::kMinLineSpacing
         && presLine <= DocumentView::kMaxLineSpacing)
            ? presLine
            : DocumentView::kDefaultPresentationLineSpacing;
    presentationDefaults.colors.text = readColor(QStringLiteral("colorText"));
    presentationDefaults.colors.background = readColor(QStringLiteral("colorBackground"));
    presentationDefaults.colors.playing = readColor(QStringLiteral("colorPlaying"));
    presentationDefaults.colors.preparing = readColor(QStringLiteral("colorPreparing"));
    presentationDefaults.colors.stopped = readColor(QStringLiteral("colorStopped"));
    presentationDefaults.colors.hover = readColor(QStringLiteral("colorHover"));
    settings.endGroup();

    QFont defaultFont;
    if (!defaultFamily.isEmpty()) {
        defaultFont.setFamily(defaultFamily);
    }
    defaultFont.setPointSize(defaultPointSize);

    PreferencesDialog dialog(m_knownVoices, defaultVoice, defaultSpeed, defaultFont, defaultColors,
                             extraVoiceDirs, defaultExtraMarginPercent, defaultLineSpacing,
                             presentationDefaults, this);

    auto applyToSession = [this, &dialog] {
        if (!dialog.voice().isEmpty()) {
            m_voiceCombo->setCurrentText(dialog.voice());
        }
        m_speedSpin->setValue(dialog.speed());
        m_presentationFontFamily = dialog.presentationFont().family();
        m_presentationFontPointSize = dialog.presentationFontPointSize();
        m_presentationExtraMarginPercent = dialog.presentationExtraMarginPercent();
        m_presentationLineSpacing = dialog.presentationLineSpacing();
        m_presentationColors = dialog.presentationColors();
        if (m_presentationMode) {
            // Keep presentation look in sync; don't clobber with reading colors.
            applyLivePresentationAppearance();
        } else {
            m_documentView->setBaseFont(dialog.font());
            m_documentView->setColors(dialog.colors());
            m_documentView->setExtraMarginPercent(dialog.extraMarginPercent());
            m_documentView->setLineSpacing(dialog.lineSpacing());
        }
    };
    connect(&dialog, &PreferencesDialog::applyRequested, this, applyToSession);

    auto saveToConfig = [this, &dialog, &settings] {
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
        settings.setValue(QStringLiteral("extraMarginPercent"), dialog.extraMarginPercent());
        settings.setValue(QStringLiteral("lineSpacing"), dialog.lineSpacing());
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

        const DocumentView::Colors chosenPresentationColors = dialog.presentationColors();
        settings.beginGroup(QStringLiteral("presentation"));
        settings.setValue(QStringLiteral("fontFamily"), dialog.presentationFont().family());
        settings.setValue(QStringLiteral("fontPointSize"), dialog.presentationFontPointSize());
        settings.setValue(QStringLiteral("extraMarginPercent"), dialog.presentationExtraMarginPercent());
        settings.setValue(QStringLiteral("lineSpacing"), dialog.presentationLineSpacing());
        storeColor(QStringLiteral("colorText"), chosenPresentationColors.text);
        storeColor(QStringLiteral("colorBackground"), chosenPresentationColors.background);
        storeColor(QStringLiteral("colorPlaying"), chosenPresentationColors.playing);
        storeColor(QStringLiteral("colorPreparing"), chosenPresentationColors.preparing);
        storeColor(QStringLiteral("colorStopped"), chosenPresentationColors.stopped);
        storeColor(QStringLiteral("colorHover"), chosenPresentationColors.hover);
        settings.endGroup();
        settings.sync();

        statusBar()->showMessage(
            tr("Preferences saved (voice directories apply on next start)"), 4000);
    };
    connect(&dialog, &PreferencesDialog::saveRequested, this, saveToConfig);

    // Close only dismisses; session already reflects live preview. Config is
    // written only when the user clicks Save.
    dialog.exec();
}

void MainWindow::openAbout()
{
    // QMessageBox::about() already uses the application/window icon and
    // gives a plain, native-looking dialog with a Close button -- no need
    // to build a custom one.
    QMessageBox::about(
        this, tr("About text2sprech"),
        tr("<h3>text2sprech %1</h3>"
           "<p>A local text-to-speech reader with live sentence "
           "highlighting, powered by a Piper TTS backend running entirely "
           "on your machine — nothing is sent to the cloud.</p>"
           "<p>Project page: "
           "<a href=\"https://github.com/Grumbel/text2sprech\">"
           "github.com/Grumbel/text2sprech</a></p>"
           "<p>Copyright © 2026 Ingo Ruhnke &lt;grumbel@gmail.com&gt;<br>"
           "Licensed under the GNU General Public License v3.0 or later.</p>")
            .arg(QApplication::applicationVersion()));
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
        m_progressSlider->setToolTip(tr("No document loaded"));
        return;
    }

    const double speed = m_speedSpin->value();
    // Slider stays word-based (uniform axis for seeking). Elapsed/remaining
    // prefer real WAV durations when PlaybackController has them (lookahead
    // or already-spoken sentences); otherwise ~150 wpm at current speed.
    auto sentenceDurationMs = [this, speed](int index) -> qint64 {
        if (index < 0 || index >= m_sentences.size()) {
            return 0;
        }
        if (m_playback) {
            const qint64 known = m_playback->cachedDurationMs(m_sentences[index].id);
            if (known > 0) {
                return known; // already synthesized at session speed
            }
        }
        return qint64(wordsToSeconds(double(qMax(1, countWords(m_sentences[index].text))), speed))
            * 1000;
    };

    qint64 totalMs = 0;
    for (int i = 0; i < m_sentences.size(); ++i) {
        totalMs += sentenceDurationMs(i);
    }

    qint64 elapsedMs = 0;
    double wordsSoFar = 0.0;
    if (m_currentSentenceIndex >= 0
        && m_currentSentenceIndex < m_sentences.size()) {
        for (int i = 0; i < m_currentSentenceIndex; ++i) {
            elapsedMs += sentenceDurationMs(i);
        }
        elapsedMs += qint64(m_intraSentenceFraction * double(sentenceDurationMs(m_currentSentenceIndex))
                            + 0.5);
        if (m_currentSentenceIndex < m_cumulativeWordsBeforeSentence.size()) {
            wordsSoFar = double(m_cumulativeWordsBeforeSentence[m_currentSentenceIndex]);
            const int wordsInSentence = countWords(m_sentences[m_currentSentenceIndex].text);
            wordsSoFar += m_intraSentenceFraction * double(qMax(1, wordsInSentence));
        }
    }

    const int totalSec = int((totalMs + 500) / 1000);
    const int elapsedSec = int((elapsedMs + 500) / 1000);
    const int remainingSec = qMax(0, int((totalMs - elapsedMs + 500) / 1000));
    const int maxUnits = qMax(1, m_totalWords * kProgressUnitsPerWord);
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

    const int percent = m_totalWords > 0
        ? static_cast<int>(100.0 * wordsSoFar / double(m_totalWords) + 0.5)
        : 0;
    m_progressSlider->setToolTip(
        tr("Position: %1% · ~%2 total · click or drag to jump")
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

void MainWindow::progressUnitToPosition(int unit, int *sentenceIndex, double *fraction) const
{
    if (!sentenceIndex || !fraction) {
        return;
    }
    if (m_sentences.isEmpty()) {
        *sentenceIndex = 0;
        *fraction = 0.0;
        return;
    }
    const int index = sentenceIndexForProgressUnit(unit);
    const double wordPos = double(unit) / double(kProgressUnitsPerWord);
    const int before = m_cumulativeWordsBeforeSentence.value(index, 0);
    const int wordsIn = qMax(1, countWords(m_sentences[index].text));
    *sentenceIndex = index;
    *fraction = qBound(0.0, (wordPos - double(before)) / double(wordsIn), 1.0);
}

void MainWindow::previewProgressUnit(int unit)
{
    if (m_sentences.isEmpty() || m_totalWords <= 0) {
        return;
    }
    int index = 0;
    double fraction = 0.0;
    progressUnitToPosition(unit, &index, &fraction);

    m_currentSentenceIndex = index;
    m_intraSentenceFraction = fraction;

    // Same hybrid elapsed/remaining as refreshProgress (labels only).
    refreshProgress();
    // refreshProgress may overwrite the slider while pressed; keep the drag value.
    if (m_progressSliderPressed) {
        m_progressSlider->blockSignals(true);
        m_progressSlider->setValue(unit);
        m_progressSlider->blockSignals(false);
    }

    // Fine highlight follows the thumb even before audio seeks on release.
    const auto state = m_playback->isPlaying()
        ? DocumentView::HighlightState::Preparing
        : DocumentView::HighlightState::Stopped;
    applyPositionHighlight(state);
}

void MainWindow::onProgressSliderCommitted(int progressUnit)
{
    if (m_sentences.isEmpty()) {
        return;
    }
    int index = 0;
    double fraction = 0.0;
    progressUnitToPosition(progressUnit, &index, &fraction);
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
    // Recent files always use the default org config so a one-off --config
    // profile does not hide the user's normal recent list.
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
    // Reading positions stay on the default org config (same as recent files).
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
