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
    reapplyZoom();
}

void DocumentView::loadHtml(const QString &html)
{
    // Qt's HTML subset (documented under QTextDocument::setHtml) covers
    // headings, paragraphs, emphasis, and line breaks as required, but it
    // is not a full browser engine -- CSS layout features like flexbox/grid
    // are not supported. Fine for the "headings/paragraphs/emphasis/line
    // breaks" scope in the proposal; flag it if richer HTML shows up.
    setHtml(html);
    reapplyZoom();
}

void DocumentView::reapplyZoom()
{
    // zoomIn/zoomOut scale the font sizes of text already in the document
    // at the time they're called -- they don't affect content inserted
    // afterwards. So switching documents resets to the un-zoomed size
    // unless we explicitly reapply the accumulated zoom here.
    if (m_zoomSteps != 0) {
        zoomIn(m_zoomSteps);
    }
}

void DocumentView::increaseTextSize()
{
    zoomIn(1);
    ++m_zoomSteps;
}

void DocumentView::decreaseTextSize()
{
    zoomIn(-1);
    --m_zoomSteps;
}

void DocumentView::resetTextSize()
{
    if (m_zoomSteps != 0) {
        zoomIn(-m_zoomSteps);
        m_zoomSteps = 0;
    }
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
