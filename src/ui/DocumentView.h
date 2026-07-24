#pragma once

#include <QTextEdit>

// Read-only document display with sentence highlighting and click-to-jump.
//
// Highlighting uses an ExtraSelection rather than mutating the document's
// character formats directly -- that keeps the highlight cheap to move
// (no undo-stack entries, no re-layout of surrounding text) and trivial to
// clear. Offsets passed to highlightRange() must be positions in
// toPlainText() of *this* document (i.e. computed after setHtml/setPlainText
// has already run), matching what SentenceSplitter produces when run on
// document()->toPlainText().
class DocumentView : public QTextEdit {
    Q_OBJECT
public:
    explicit DocumentView(QWidget *parent = nullptr);

    void loadPlainText(const QString &text);
    void loadHtml(const QString &html);

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
    void applyHighlight(int start, int end);
};
