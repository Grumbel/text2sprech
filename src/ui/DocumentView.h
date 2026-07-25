#pragma once

#include <QList>
#include <QTextEdit>
#include <QVector>

#include "document/Sentence.h"

// Read-only document display with sentence highlighting and click-to-jump.
//
// Highlighting uses ExtraSelections rather than mutating the document's
// character formats directly -- that keeps highlights cheap to move (no
// undo-stack entries, no re-layout of surrounding text) and trivial to
// clear/rebuild. Offsets passed to highlightRange()/setSentenceBoundaries()
// must be positions in toPlainText() of *this* document (i.e. computed
// after setHtml/setPlainText has already run), matching what
// SentenceSplitter produces when run on document()->toPlainText().
//
// Two layers of ExtraSelections are combined (see refreshExtraSelections()):
//  1. A static background tint on every sentence, alternating between two
//     yellow-family shades (pale butter yellow / warmer gold) so segment
//     boundaries are visible at a glance, independent of playback.
//  2. The single "currently playing" highlight, drawn on top of the tints
//     (also used for click-to-select, so clicking anywhere in a sentence
//     highlights that whole sentence even when nothing is playing).
class DocumentView : public QTextEdit {
    Q_OBJECT
public:
    explicit DocumentView(QWidget *parent = nullptr);

    void loadPlainText(const QString &text);
    void loadHtml(const QString &html);

    // Renders a background tint on every sentence, alternating between two
    // yellow-family shades. Call after (re)loading a document and
    // re-splitting it into sentences. Pass an empty vector to clear the
    // tinting (e.g. before loading new content).
    void setSentenceBoundaries(const QVector<Sentence> &sentences);

    // start/end are plain-text offsets (end exclusive), matching Sentence.
    void highlightRange(int start, int end);
    void clearHighlight();

public slots:
    // Text size, applied via QTextEdit's built-in zoomIn/zoomOut (scales
    // all font sizes proportionally, so HTML headings stay bigger than
    // body text rather than everything becoming a single flat size). The
    // cumulative zoom level is tracked and reapplied automatically whenever
    // new content is loaded, since zoomIn/zoomOut only affects text already
    // in the document at the time it's called.
    void increaseTextSize();
    void decreaseTextSize();
    void resetTextSize();

    // Sets the starting zoom level (e.g. from a config file default),
    // before any document has been loaded. Not a slot -- meant to be called
    // once, right after construction.
    void setInitialZoomSteps(int steps) { m_zoomSteps = steps; }

signals:
    // Emitted when the user clicks inside the text; caller maps this back
    // to a sentence index (e.g. via binary search over sentence offsets).
    void positionClicked(int plainTextOffset);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void refreshExtraSelections();
    void reapplyZoom();
    void centerCursor(const QTextCursor &cursor);

    QList<QTextEdit::ExtraSelection> m_tintSelections;
    QTextEdit::ExtraSelection m_currentHighlight;
    bool m_hasCurrentHighlight = false;
    int m_zoomSteps = 0;
};
