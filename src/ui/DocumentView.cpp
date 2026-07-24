#include "DocumentView.h"

#include <QMouseEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>

DocumentView::DocumentView(QWidget *parent) : QTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QTextEdit::WidgetWidth);
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
    setHtml(html);
}

void DocumentView::clearHighlight()
{
    setExtraSelections({});
}

void DocumentView::applyHighlight(int start, int end)
{
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(palette().highlight());
    selection.format.setForeground(palette().highlightedText());
    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    selection.cursor = cursor;
    setExtraSelections({selection});

    // Auto-scroll so the highlighted sentence stays in view.
    setTextCursor(cursor);
    ensureCursorVisible();
}

void DocumentView::highlightRange(int start, int end)
{
    if (start < 0 || end > document()->characterCount() || start >= end) {
        return;
    }
    applyHighlight(start, end);
}

void DocumentView::mousePressEvent(QMouseEvent *event)
{
    QTextEdit::mousePressEvent(event);
    QTextCursor cursor = cursorForPosition(event->pos());
    emit positionClicked(cursor.position());
}
