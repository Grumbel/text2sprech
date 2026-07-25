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
//  1. A static, alternating background tint per sentence (every other
//     sentence gets a subtle tint, the rest are left untinted) so segment
//     boundaries are visible at a glance, independent of playback.
//  2. The single "currently playing" highlight, drawn on top of the tints.
class DocumentView : public QTextEdit {
    Q_OBJECT
public:
    explicit DocumentView(QWidget *parent = nullptr);

    void loadPlainText(const QString &text);
    void loadHtml(const QString &html);

    // Renders alternating background tints for every sentence boundary.
    // Call after (re)loading a document and re-splitting it into sentences.
    // Pass an empty vector to clear the tinting (e.g. before loading new
    // content).
    void setSentenceBoundaries(const QVector<Sentence> &sentences);

    // start/end are plain-text offsets (end exclusive), matching Sentence.
    void highlightRange(int start, int end);
    void clearHighlight();

signals:
    // Emitted when the user clicks inside the text; caller maps this back
    // to a sentence index (e.g. via binary search over sentence offsets).
    void positionClicked(int plainTextOffset);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void refreshExtraSelections();

    QList<QTextEdit::ExtraSelection> m_tintSelections;
    QTextEdit::ExtraSelection m_currentHighlight;
    bool m_hasCurrentHighlight = false;
};
