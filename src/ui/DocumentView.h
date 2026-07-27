// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QList>
#include <QTextEdit>
#include <QUrl>
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
// Two optional ExtraSelections are combined (see refreshExtraSelections()):
//  1. The single current-sentence highlight, colored by HighlightState
//     (playing / preparing / stopped-or-selected).
//  2. A lighter hover highlight on the sentence under the mouse cursor,
//     drawn underneath the current highlight when both overlap so the
//     active sentence always wins visually.
// Sentence bodies themselves stay un-tinted (plain document background).
class DocumentView : public QTextEdit {
    Q_OBJECT
public:
    // Visual mode for the current-sentence highlight. Callers set this via
    // highlightRange() or setHighlightState() as playback progresses.
    enum class HighlightState {
        Stopped,    // selected / paused / idle -- muted
        Preparing,  // audio is being synthesized for this sentence
        Playing,    // actively speaking this sentence
    };

    // User-overridable view colors. An invalid QColor means "use the
    // built-in theme default for the current light/dark palette".
    struct Colors {
        QColor text;         // document text (palette WindowText / Text)
        QColor background;   // document background (palette Base)
        QColor playing;      // current-sentence highlight while speaking
        QColor preparing;    // current-sentence highlight while synthesizing
        QColor stopped;      // current-sentence highlight when idle/paused
        QColor hover;        // sentence under the mouse cursor

        bool operator==(const Colors &o) const
        {
            return text == o.text && background == o.background && playing == o.playing
                && preparing == o.preparing && stopped == o.stopped && hover == o.hover;
        }
    };

    // Built-in defaults for the given theme (invalid overrides in Colors
    // resolve to these).
    static Colors builtinDefaults(bool darkTheme);

    explicit DocumentView(QWidget *parent = nullptr);

    void loadPlainText(const QString &text);
    void loadHtml(const QString &html);

    // Stores sentence ranges for hover hit-testing and clears any previous
    // hover highlight. Call after (re)loading a document and re-splitting
    // it into sentences. Pass an empty vector to clear boundaries (e.g.
    // before loading new content). Does not paint static per-sentence
    // background tints -- text stays plain until hovered or selected.
    void setSentenceBoundaries(const QVector<Sentence> &sentences);

    // start/end are plain-text offsets (end exclusive), matching Sentence.
    // state selects the highlight color (playing / preparing / stopped).
    void highlightRange(int start, int end, HighlightState state = HighlightState::Stopped);
    // Recolor the existing current highlight without moving it (e.g. pause
    // -> Stopped, or preparing -> Playing once audio arrives). No-op if
    // there is no current highlight.
    void setHighlightState(HighlightState state);
    void clearHighlight();

    HighlightState highlightState() const { return m_highlightState; }

    // Applies user color overrides (invalid entries keep built-in defaults).
    void setColors(const Colors &colors);
    Colors colors() const { return m_colors; }

    // The font family/size used for plain text and any HTML content
    // without its own explicit styling. A real font+point-size, not an
    // abstract "zoom level" -- see setBaseFont().
    QFont baseFont() const { return m_baseFont; }

    // Extra document margins (QTextDocument::documentMargin) for a
    // narrower reading column. Size is the pixel value used when enabled;
    // when disabled the Qt default (4) is restored. Size may be changed
    // while disabled and takes effect the next time margins are turned on.
    bool extraMarginsEnabled() const { return m_extraMarginsEnabled; }
    int extraMarginSize() const { return m_extraMarginSize; }
    void setExtraMarginsEnabled(bool enabled);
    void setExtraMarginSize(int pixels);

    // Built-in defaults for the margin preference spin box / config.
    static constexpr int kDefaultExtraMarginSize = 64;
    static constexpr int kMinExtraMarginSize = 8;
    static constexpr int kMaxExtraMarginSize = 240;
    static constexpr int kBuiltinDocumentMargin = 4; // Qt default

public slots:
    // Sets the base font (family + point size) and applies it immediately.
    // Set on the QTextDocument itself (setDefaultFont()), so it persists
    // across loadPlainText()/loadHtml() (they reuse the same document).
    // Plain/unstyled text picks this up; HtmlSanitizer strips author font
    // sizing so pasted HTML does not fight the user's choice.
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
    // Local file(s) dropped onto the view -- MainWindow opens the first
    // usable text/HTML path.
    void filesDropped(const QList<QUrl> &urls);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void refreshExtraSelections();
    void centerCursor(const QTextCursor &cursor);
    // Maps a plain-text offset to the sentence that contains it, or -1.
    int sentenceIndexForOffset(int offset) const;
    void setHoverSentence(int index);
    // Applies background/foreground colors for m_highlightState onto
    // m_currentHighlight.format (does not touch the cursor range).
    void applyHighlightColors();
    void applyDocumentPalette();
    bool isDarkTheme() const;
    // Near-black or near-white text depending on background luminance.
    static QColor contrastingText(const QColor &background);

    // Sentence ranges for hover hit-testing (not painted as static tints).
    QVector<Sentence> m_sentences;

    QTextEdit::ExtraSelection m_currentHighlight;
    bool m_hasCurrentHighlight = false;
    HighlightState m_highlightState = HighlightState::Stopped;
    int m_highlightStart = -1;
    int m_highlightEnd = -1;

    QTextEdit::ExtraSelection m_hoverHighlight;
    bool m_hasHoverHighlight = false;
    int m_hoverSentenceIndex = -1;

    Colors m_colors; // invalid entries => theme builtins
    QFont m_baseFont;

    void applyDocumentMargins();

    bool m_extraMarginsEnabled = false;
    int m_extraMarginSize = kDefaultExtraMarginSize;
};
