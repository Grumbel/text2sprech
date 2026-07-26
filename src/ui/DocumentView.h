#pragma once

#include <QClipboard>
#include <QFont>
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

    // The font family/size used for plain text and any HTML content
    // without its own explicit styling. A real font+point-size, not an
    // abstract "zoom level" -- see setBaseFont().
    QFont baseFont() const { return m_baseFont; }

public slots:
    // Sets the base font (family + point size) and applies it immediately.
    // Unlike the old zoom-step approach, this doesn't need to be reapplied
    // on every load: it's set on the QTextDocument itself
    // (setDefaultFont()), which persists across loadPlainText()/loadHtml()
    // calls since they reuse the same QTextDocument rather than replacing
    // it. Plain, unstyled text picks this up dynamically; HTML content with
    // its own explicit sizing (e.g. headings) won't -- see the HTML-cleanup
    // TODO in MainWindow/README for narrowing what HTML styling gets kept.
    void setBaseFont(const QFont &font);

    // Convenience +/- 1pt adjustments on the current base font, e.g. for
    // quick toolbar buttons -- operate on a real point size now, not a
    // relative zoom unit.
    void increaseTextSize();
    void decreaseTextSize();

signals:
    // Emitted when the user clicks inside the text; caller maps this back
    // to a sentence index (e.g. via binary search over sentence offsets).
    void positionClicked(int plainTextOffset);
    // Left/Right arrow keys, repurposed for sentence navigation (see
    // keyPressEvent()) rather than their default fine-grained character
    // cursor movement -- this is a read-only reader, not an editor, so
    // sentence-at-a-time navigation is more useful here.
    void previousSentenceRequested();
    void nextSentenceRequested();
    // Ctrl+V (clipboard) or middle-click (primary selection, X11/Wayland
    // convention) -- this view is read-only and shows one whole document
    // at a time, so "paste" here means "load this as the new document"
    // rather than inserting text at a cursor position.
    void pasteRequested(QClipboard::Mode mode);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void refreshExtraSelections();
    void centerCursor(const QTextCursor &cursor);

    QList<QTextEdit::ExtraSelection> m_tintSelections;
    QTextEdit::ExtraSelection m_currentHighlight;
    bool m_hasCurrentHighlight = false;
    QFont m_baseFont;
};
