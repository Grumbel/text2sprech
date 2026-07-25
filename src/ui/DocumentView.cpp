#include "DocumentView.h"

#include <QMouseEvent>
#include <QPalette>
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

void DocumentView::setSentenceBoundaries(const QVector<Sentence> &sentences)
{
    m_tintSelections.clear();

    // Tint every other sentence only -- leaving the rest at the plain
    // background creates a visible banding pattern without needing a
    // strong color. A low-alpha black/white overlay (rather than a fixed
    // color) adapts automatically to light and dark palettes/themes.
    bool isDarkTheme = palette().color(QPalette::Base).lightness() < 128;
    QColor tintColor = isDarkTheme ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 20);

    const int charCount = document()->characterCount();
    for (int i = 0; i < sentences.size(); ++i) {
        if (i % 2 == 0) {
            continue; // untinted -- alternates with the tinted ones below
        }
        const Sentence &s = sentences[i];
        if (s.start < 0 || s.end > charCount || s.start >= s.end) {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(tintColor);
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
        // Drawn last so the "currently playing" highlight sits visibly on
        // top of the alternating tint rather than blending with it.
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
    selection.format.setBackground(palette().highlight());
    selection.format.setForeground(palette().highlightedText());
    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    selection.cursor = cursor;

    m_currentHighlight = selection;
    m_hasCurrentHighlight = true;
    refreshExtraSelections();

    // Auto-scroll so the highlighted sentence stays in view.
    setTextCursor(cursor);
    ensureCursorVisible();
}

void DocumentView::mousePressEvent(QMouseEvent *event)
{
    QTextEdit::mousePressEvent(event);
    QTextCursor cursor = cursorForPosition(event->pos());
    emit positionClicked(cursor.position());
}
