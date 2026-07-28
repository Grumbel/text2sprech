// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QFont>
#include <QString>
#include <QStringList>

#include "DocumentView.h"

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QToolButton;

// Edits appearance and voice defaults. Seeded from the saved config, not
// the live toolbar values.
//
//  - Controls auto-apply to the current session as they change.
//  - Save: write the dialog values as startup defaults (dialog stays open).
//  - Close: dismiss without writing the config (session keeps the preview).
//
// Presentation-mode startup defaults (invalid colors → built-in
// DocumentView::presentationDefaults() when applied). Defined outside
// PreferencesDialog so it is a complete type usable as a constructor
// default argument (nested default-member-initializers cannot be used
// that way before the enclosing class is complete).
struct PresentationDefaults {
    QString fontFamily; // empty → seed from reading/default font in the dialog
    int fontPointSize = DocumentView::kPresentationFontPointSize;
    int extraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    double lineSpacing = DocumentView::kDefaultPresentationLineSpacing;
    DocumentView::Colors colors; // invalid channel → presentationDefaults()
};

// This is the *only* place in the app that writes to the config file.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // availableVoices seeds the voice dropdown. default* values are the
    // startup defaults from the config file (not the live session).
    // defaultColors: invalid QColor entries mean "built-in theme default".
    // defaultExtraVoiceDirs: optional extra model directories (one path per
    // entry); the primary ~/.local/share/text2sprech/voices dir is always used.
    PreferencesDialog(const QStringList &availableVoices, const QString &defaultVoice, double defaultSpeed,
                       const QFont &defaultFont, const DocumentView::Colors &defaultColors,
                       const QStringList &defaultExtraVoiceDirs = {},
                       int defaultExtraMarginPercent = DocumentView::kDefaultExtraMarginPercent,
                       double defaultLineSpacing = DocumentView::kDefaultLineSpacing,
                       PresentationDefaults presentation = {},
                       QWidget *parent = nullptr);

    QString voice() const;
    double speed() const;
    QFont font() const;
    DocumentView::Colors colors() const;
    // Extra directories to scan for Piper .onnx voices (does not include the
    // built-in user drop-in folder). Applied on next app start.
    QStringList extraVoiceDirs() const;
    // Percent of DocumentView width for extra left/right margins when the
    // toolbar toggle is on.
    int extraMarginPercent() const;
    double lineSpacing() const;

    // Presentation / teleprompter defaults (font, margin %, line spacing, colors).
    QFont presentationFont() const;
    int presentationFontPointSize() const;
    int presentationExtraMarginPercent() const;
    double presentationLineSpacing() const;
    DocumentView::Colors presentationColors() const;

signals:
    // Live preview: any control change. Caller pushes getters into the session.
    void applyRequested();
    // Explicit Save: caller writes getters to the config file. Dialog stays open.
    void saveRequested();

private:
    void wireLiveApply();
    // One color row: swatch button + reset-to-default tool button.
    // Stored color may be invalid (= use built-in default); the swatch then
    // shows the theme default for preview.
    struct ColorRow {
        QToolButton *swatch = nullptr;
        QToolButton *reset = nullptr;
        QColor value;          // may be invalid
        QColor themeDefault;   // preview when value is invalid
        QString label;
    };

    ColorRow *addColorRow(class QFormLayout *form, const QString &label, const QColor &value,
                          const QColor &themeDefault);
    void updateSwatch(ColorRow *row);
    void pickColor(ColorRow *row);
    void resetColor(ColorRow *row);

    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QFontComboBox *m_fontCombo;
    QSpinBox *m_fontSizeSpin;
    QSpinBox *m_extraMarginSpin = nullptr;
    QDoubleSpinBox *m_lineSpacingSpin = nullptr;
    QPlainTextEdit *m_extraVoiceDirsEdit = nullptr;

    ColorRow *m_textColor = nullptr;
    ColorRow *m_backgroundColor = nullptr;
    ColorRow *m_playingColor = nullptr;
    ColorRow *m_preparingColor = nullptr;
    ColorRow *m_stoppedColor = nullptr;
    ColorRow *m_hoverColor = nullptr;

    QFontComboBox *m_presentationFontCombo = nullptr;
    QSpinBox *m_presentationFontSizeSpin = nullptr;
    QSpinBox *m_presentationMarginSpin = nullptr;
    QDoubleSpinBox *m_presentationLineSpacingSpin = nullptr;
    ColorRow *m_presentationTextColor = nullptr;
    ColorRow *m_presentationBackgroundColor = nullptr;
    ColorRow *m_presentationPlayingColor = nullptr;
    ColorRow *m_presentationPreparingColor = nullptr;
    ColorRow *m_presentationStoppedColor = nullptr;
    ColorRow *m_presentationHoverColor = nullptr;
};
