// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DocumentView.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QRect>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QUrl>
#include <algorithm>

DocumentView::Colors DocumentView::builtinDefaults(bool darkTheme)
{
    Colors c;
    if (darkTheme) {
        c.text = QColor(232, 232, 236);
        c.background = QColor(30, 30, 34);
        c.playing = QColor(46, 110, 62);
        c.preparing = QColor(40, 78, 120);
        c.stopped = QColor(62, 66, 72);
        c.hover = QColor(120, 140, 170, 70);
    } else {
        c.text = QColor(28, 30, 34);
        c.background = QColor(255, 255, 255);
        c.playing = QColor(198, 228, 188);
        c.preparing = QColor(196, 220, 242);
        c.stopped = QColor(226, 228, 232);
        c.hover = QColor(160, 180, 210, 90);
    }
    return c;
}

DocumentView::DocumentView(QWidget *parent) : QTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QTextEdit::WidgetWidth);
    // Needed so mouseMoveEvent fires without a button held -- required for
    // per-sentence hover highlighting.
    setMouseTracking(true);
    // Accept file drops (forwarded to MainWindow); block QTextEdit's default
    // insert-on-drop which would fight read-only whole-document loading.
    setAcceptDrops(true);
    m_baseFont = font(); // whatever the platform/style default is, until setBaseFont() is called
}

void DocumentView::loadPlainText(const QString &text)
{
    setPlainText(text);
}

void DocumentView::loadHtml(const QString &html)
{
    // Expects already-sanitized HTML (see HtmlSanitizer::toSemanticHtml,
    // called from MainWindow before this). Qt's HTML subset covers the
    // structural tags we keep; it is not a full browser engine.
    setHtml(html);
}

void DocumentView::setBaseFont(const QFont &font)
{
    m_baseFont = font;
    // setDefaultFont() (not zoomIn/zoomOut) because it's a property of the
    // QTextDocument itself: it persists across loadPlainText()/loadHtml()
    // calls (they reuse this same QTextDocument, never replace it) and
    // applies live to any plain/unstyled text immediately, with no need to
    // reapply it after every load the way the old zoom-step approach did.
    document()->setDefaultFont(font);
    setFont(font);
}

void DocumentView::increaseTextSize()
{
    QFont f = m_baseFont;
    f.setPointSize(f.pointSize() + 1);
    setBaseFont(f);
}

void DocumentView::decreaseTextSize()
{
    QFont f = m_baseFont;
    f.setPointSize(qMax(1, f.pointSize() - 1));
    setBaseFont(f);
}

void DocumentView::setExtraMarginsEnabled(bool enabled)
{
    if (m_extraMarginsEnabled == enabled) {
        return;
    }
    m_extraMarginsEnabled = enabled;
    applyDocumentMargins();
}

void DocumentView::setExtraMarginSize(int pixels)
{
    const int clamped = qBound(kMinExtraMarginSize, pixels, kMaxExtraMarginSize);
    if (m_extraMarginSize == clamped) {
        // Still re-apply if currently enabled so a no-op clamp after a bad
        // config value doesn't leave the document on the old margin.
        if (m_extraMarginsEnabled) {
            applyDocumentMargins();
        }
        return;
    }
    m_extraMarginSize = clamped;
    if (m_extraMarginsEnabled) {
        applyDocumentMargins();
    }
}

void DocumentView::applyDocumentMargins()
{
    // documentMargin is a property of QTextDocument and survives
    // loadPlainText()/loadHtml() on the same document instance.
    document()->setDocumentMargin(
        m_extraMarginsEnabled ? m_extraMarginSize : kBuiltinDocumentMargin);
}

void DocumentView::setColors(const Colors &colors)
{
    m_colors = colors;
    applyDocumentPalette();
    if (m_hasCurrentHighlight) {
        applyHighlightColors();
    }
    // Rebuild hover with the new color if one is showing.
    if (m_hasHoverHighlight) {
        const int idx = m_hoverSentenceIndex;
        m_hoverSentenceIndex = -1; // force setHoverSentence to reapply
        setHoverSentence(idx);
    } else {
        refreshExtraSelections();
    }
}

void DocumentView::applyDocumentPalette()
{
    QPalette pal = palette();
    const Colors defaults = builtinDefaults(isDarkTheme());
    const QColor text = m_colors.text.isValid() ? m_colors.text : defaults.text;
    const QColor bg = m_colors.background.isValid() ? m_colors.background : defaults.background;
    // Only override when the user set a color; otherwise leave the platform
    // palette alone so system theme changes still flow through.
    if (m_colors.text.isValid()) {
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::WindowText, text);
    }
    if (m_colors.background.isValid()) {
        pal.setColor(QPalette::Base, bg);
        // Keep the viewport behind the text in sync when the document is short.
        pal.setColor(QPalette::Window, bg);
    }
    if (m_colors.text.isValid() || m_colors.background.isValid()) {
        setPalette(pal);
        viewport()->setPalette(pal);
    } else {
        // Clear custom palette so we track the app/style theme again.
        setPalette(QPalette());
        viewport()->setPalette(QPalette());
    }
}

bool DocumentView::isDarkTheme() const
{
    // Prefer the *effective* base color (may already include a user override).
    return palette().color(QPalette::Base).lightness() < 128;
}

QColor DocumentView::contrastingText(const QColor &background)
{
    // WCAG-ish relative luminance threshold: light text on dark, dark on light.
    const double r = background.redF();
    const double g = background.greenF();
    const double b = background.blueF();
    const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return luminance > 0.45 ? QColor(20, 24, 28) : QColor(240, 242, 245);
}

void DocumentView::setSentenceBoundaries(const QVector<Sentence> &sentences)
{
    m_sentences = sentences;
    // Drop any hover that referred to the previous document's ranges.
    m_hasHoverHighlight = false;
    m_hoverSentenceIndex = -1;
    refreshExtraSelections();
}

void DocumentView::clearHighlight()
{
    m_hasCurrentHighlight = false;
    m_highlightStart = -1;
    m_highlightEnd = -1;
    m_highlightState = HighlightState::Stopped;
    refreshExtraSelections();
}

void DocumentView::applyHighlightColors()
{
    const Colors defaults = builtinDefaults(isDarkTheme());
    QColor bg;
    switch (m_highlightState) {
    case HighlightState::Playing:
        bg = m_colors.playing.isValid() ? m_colors.playing : defaults.playing;
        break;
    case HighlightState::Preparing:
        bg = m_colors.preparing.isValid() ? m_colors.preparing : defaults.preparing;
        break;
    case HighlightState::Stopped:
    default:
        bg = m_colors.stopped.isValid() ? m_colors.stopped : defaults.stopped;
        break;
    }
    m_currentHighlight.format.setBackground(bg);
    m_currentHighlight.format.setForeground(contrastingText(bg));
}

void DocumentView::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> combined;
    // Hover first, current on top -- so the active/playing sentence always
    // wins when the mouse is over it.
    if (m_hasHoverHighlight) {
        combined.append(m_hoverHighlight);
    }
    if (m_hasCurrentHighlight) {
        combined.append(m_currentHighlight);
    }
    setExtraSelections(combined);
}

void DocumentView::highlightRange(int start, int end, HighlightState state)
{
    if (start < 0 || end > document()->characterCount() || start >= end) {
        return;
    }

    m_highlightState = state;
    m_highlightStart = start;
    m_highlightEnd = end;

    QTextEdit::ExtraSelection selection;
    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    selection.cursor = cursor;
    m_currentHighlight = selection;
    applyHighlightColors();
    m_hasCurrentHighlight = true;
    refreshExtraSelections();

    // Center the highlighted sentence in the viewport rather than just
    // nudging it into view at an edge (what ensureCursorVisible() alone
    // does) -- keeps the active sentence in a stable, comfortable reading
    // position as playback scrolls through a long document.
    //
    // Deliberately does NOT call setTextCursor(cursor) here: giving the
    // widget itself a real selection (anchor != position) makes Qt paint
    // its own native selection -- palette Highlight/HighlightedText, i.e.
    // exactly the blue-background/white-text this highlight scheme exists
    // to avoid -- on top of our ExtraSelection. cursorRect() works on any
    // cursor, not just the widget's actual one, so centering doesn't need
    // to touch real cursor/selection state at all.
    centerCursor(cursor);
}

void DocumentView::setHighlightState(HighlightState state)
{
    if (!m_hasCurrentHighlight) {
        return;
    }
    if (m_highlightState == state) {
        return;
    }
    m_highlightState = state;
    applyHighlightColors();
    refreshExtraSelections();
}

void DocumentView::centerCursor(const QTextCursor &cursor)
{
    const QRect cursorViewportRect = cursorRect(cursor);
    const int viewportMidY = viewport()->height() / 2;
    const int delta = cursorViewportRect.center().y() - viewportMidY;
    QScrollBar *vbar = verticalScrollBar();
    vbar->setValue(vbar->value() + delta);
}

int DocumentView::sentenceIndexForOffset(int offset) const
{
    if (m_sentences.isEmpty()) {
        return -1;
    }
    auto it = std::upper_bound(
        m_sentences.begin(), m_sentences.end(), offset,
        [](int off, const Sentence &s) { return off < s.end; });
    if (it == m_sentences.end()) {
        return -1;
    }
    const int index = static_cast<int>(it - m_sentences.begin());
    // upper_bound found the first sentence whose end is past offset; still
    // need the offset to actually fall inside [start, end).
    if (offset < m_sentences[index].start) {
        return -1;
    }
    return index;
}

void DocumentView::setHoverSentence(int index)
{
    if (index == m_hoverSentenceIndex) {
        return;
    }
    m_hoverSentenceIndex = index;

    if (index < 0 || index >= m_sentences.size()) {
        m_hasHoverHighlight = false;
        refreshExtraSelections();
        return;
    }

    const Sentence &s = m_sentences[index];
    const int charCount = document()->characterCount();
    if (s.start < 0 || s.end > charCount || s.start >= s.end) {
        m_hasHoverHighlight = false;
        refreshExtraSelections();
        return;
    }

    QTextEdit::ExtraSelection selection;
    const Colors defaults = builtinDefaults(isDarkTheme());
    const QColor hoverBg = m_colors.hover.isValid() ? m_colors.hover : defaults.hover;
    selection.format.setBackground(hoverBg);
    QTextCursor cursor(document());
    cursor.setPosition(s.start);
    cursor.setPosition(s.end, QTextCursor::KeepAnchor);
    selection.cursor = cursor;

    m_hoverHighlight = selection;
    m_hasHoverHighlight = true;
    refreshExtraSelections();
}

void DocumentView::changeEvent(QEvent *event)
{
    QTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        // Theme switch: re-resolve any colors still using builtins.
        applyDocumentPalette();
        if (m_hasCurrentHighlight) {
            applyHighlightColors();
            refreshExtraSelections();
        }
        if (m_hasHoverHighlight) {
            const int idx = m_hoverSentenceIndex;
            m_hoverSentenceIndex = -1;
            setHoverSentence(idx);
        }
    }
}

void DocumentView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        // X11/Wayland convention: middle-click pastes the primary
        // selection at the click point. This view only ever shows one
        // whole document, so "paste" means "load this as the new
        // document" -- not an insert -- and doesn't depend on where
        // exactly the click landed.
        emit pasteRequested(QClipboard::Selection);
        event->accept();
        return;
    }
    QTextEdit::mousePressEvent(event);
    QTextCursor cursor = cursorForPosition(event->pos());
    emit positionClicked(cursor.position());
}

void DocumentView::mouseMoveEvent(QMouseEvent *event)
{
    QTextEdit::mouseMoveEvent(event);
    const QTextCursor cursor = cursorForPosition(event->pos());
    setHoverSentence(sentenceIndexForOffset(cursor.position()));
}

void DocumentView::leaveEvent(QEvent *event)
{
    setHoverSentence(-1);
    QTextEdit::leaveEvent(event);
}

void DocumentView::keyPressEvent(QKeyEvent *event)
{
    // Must intercept here, before QTextEdit::keyPressEvent(): plain arrow
    // keys are handled directly by the focused widget rather than going
    // through the app-level QAction shortcut system, so a MainWindow-level
    // shortcut for Key_Left/Key_Right would silently lose to QTextEdit's
    // own character-cursor movement whenever this widget has focus (which
    // is most of the time, right after clicking in the document).
    if (event->modifiers() == Qt::NoModifier) {
        if (event->key() == Qt::Key_Left) {
            emit previousSentenceRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            emit nextSentenceRequested();
            event->accept();
            return;
        }
    }
    if (event->matches(QKeySequence::Paste)) {
        // QTextEdit would otherwise just silently do nothing here since
        // the widget is read-only -- intercept and treat it the same as
        // the toolbar's "Paste Clipboard" button.
        emit pasteRequested(QClipboard::Clipboard);
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

static bool mimeHasLocalFiles(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls()) {
        return false;
    }
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile()) {
            return true;
        }
    }
    return false;
}

void DocumentView::dragEnterEvent(QDragEnterEvent *event)
{
    if (mimeHasLocalFiles(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void DocumentView::dragMoveEvent(QDragMoveEvent *event)
{
    if (mimeHasLocalFiles(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void DocumentView::dropEvent(QDropEvent *event)
{
    if (!mimeHasLocalFiles(event->mimeData())) {
        event->ignore();
        return;
    }
    emit filesDropped(event->mimeData()->urls());
    event->acceptProposedAction();
}
