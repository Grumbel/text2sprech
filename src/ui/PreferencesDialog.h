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
class QToolButton;

// Edits startup defaults (config file) and optionally applies them to the
// live session. Seeded from the saved defaults, not the current toolbar
// values.
//
//  - Save: write defaults to the config file and close (does not change
//    the live session by itself).
//  - Apply: push the dialog values into the current session (voice/speed/
//    font/colors) without writing the config file or closing.
//  - Cancel: close without saving or applying.
//
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
                       int defaultExtraMarginSize = DocumentView::kDefaultExtraMarginSize,
                       QWidget *parent = nullptr);

    QString voice() const;
    double speed() const;
    QFont font() const;
    DocumentView::Colors colors() const;
    // Extra directories to scan for Piper .onnx voices (does not include the
    // built-in user drop-in folder). Applied on next app start.
    QStringList extraVoiceDirs() const;
    // Pixel size for DocumentView extra margins when the toolbar toggle is on.
    int extraMarginSize() const;

signals:
    // Emitted when the user clicks Apply. Caller should push getters into
    // the live session. The dialog stays open.
    void applyRequested();

private:
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
    QPlainTextEdit *m_extraVoiceDirsEdit = nullptr;

    ColorRow *m_textColor = nullptr;
    ColorRow *m_backgroundColor = nullptr;
    ColorRow *m_playingColor = nullptr;
    ColorRow *m_preparingColor = nullptr;
    ColorRow *m_stoppedColor = nullptr;
    ColorRow *m_hoverColor = nullptr;
};
