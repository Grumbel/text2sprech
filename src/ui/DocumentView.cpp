#include "DocumentView.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPalette>
#include <QRect>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

DocumentView::DocumentView(QWidget *parent) : QTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QTextEdit::WidgetWidth);
    m_baseFont = font(); // whatever the platform/style default is, until setBaseFont() is called
}

void DocumentView::loadPlainText(const QString &text)
{
    setPlainText(text);
}

void DocumentView::loadHtml(const QString &html)
{
    // Qt's HTML subset (documented under QTextDocument::setHtml) covers
    // headings, paragraphs, emphasis, and line breaks as required, but it
    // is not a full browser engine -- CSS layout features like flexbox/grid
    // are not supported. Fine for the "headings/paragraphs/emphasis/line
    // breaks" scope in the proposal; flag it if richer HTML shows up.
    //
    // TODO(HTML cleanup, deferred): pasted/opened HTML can carry font-size,
    // font-family, and other inline styling that makes no sense for a
    // text-to-speech reader (and fights with the user's own font choice
    // here). Worth narrowing to a genuinely minimal subset before this --
    // structural tags like headings are useful signal (e.g. a chapter-
    // change chime), but styling isn't. Not done yet; flagged for later.
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

void DocumentView::setSentenceBoundaries(const QVector<Sentence> &sentences)
{
    m_tintSelections.clear();

    // Every sentence gets tinted now (alternating between two shades)
    // rather than alternating tint/no-tint -- leaving every other sentence
    // at the plain background meant half of them still showed up white.
    // Both shades are yellow-family so they read as one cohesive highlight
    // scheme rather than two unrelated colors: a pale butter yellow and a
    // warmer gold/amber, tuned separately for light vs. dark themes so
    // neither washes out or overwhelms the base background.
    bool isDarkTheme = palette().color(QPalette::Base).lightness() < 128;
    QColor colorA = isDarkTheme ? QColor(133, 108, 8, 110) : QColor(255, 247, 191, 200);  // pale butter yellow
    QColor colorB = isDarkTheme ? QColor(168, 122, 10, 110) : QColor(255, 214, 92, 200);  // warmer gold/amber

    const int charCount = document()->characterCount();
    for (int i = 0; i < sentences.size(); ++i) {
        const Sentence &s = sentences[i];
        if (s.start < 0 || s.end > charCount || s.start >= s.end) {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(i % 2 == 0 ? colorA : colorB);
        QTextCursor cursor(document());
        cursor.setPosition(s.start);
        cursor.setPosition(s.end, QTextCursor::KeepAnchor);
        selection.cursor = cursor;
        m_tintSelections.append(selection);
    }

    refreshExtraSelections();
}

void DocumentView::clearHighlight()
{
    m_hasCurrentHighlight = false;
    refreshExtraSelections();
}

void DocumentView::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> combined = m_tintSelections;
    if (m_hasCurrentHighlight) {
        // Drawn last so the "currently playing"/"clicked" highlight sits
        // visibly on top of the alternating tint rather than blending
        // with it.
        combined.append(m_currentHighlight);
    }
    setExtraSelections(combined);
}

void DocumentView::highlightRange(int start, int end)
{
    if (start < 0 || end > document()->characterCount() || start >= end) {
        return;
    }

    QTextEdit::ExtraSelection selection;
    // Fixed, high-contrast colors rather than palette().highlight()/
    // highlightedText() -- the default blue-background/white-text
    // combination was hard to read (and, layered over the yellow tint
    // stripes, lower-contrast than it looks in isolation). A saturated
    // amber with near-black text stays clearly readable regardless of the
    // underlying tint or the app's light/dark theme, and reads as "this is
    // the active one" against the paler yellow tints.
    selection.format.setBackground(QColor(255, 152, 0));
    selection.format.setForeground(QColor(20, 20, 20));
    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    selection.cursor = cursor;

    m_currentHighlight = selection;
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

void DocumentView::centerCursor(const QTextCursor &cursor)
{
    const QRect cursorViewportRect = cursorRect(cursor);
    const int viewportMidY = viewport()->height() / 2;
    const int delta = cursorViewportRect.center().y() - viewportMidY;
    QScrollBar *vbar = verticalScrollBar();
    vbar->setValue(vbar->value() + delta);
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
