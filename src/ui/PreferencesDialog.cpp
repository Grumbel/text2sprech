// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PreferencesDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString defaultUserVoicesDirPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("text2sprech/voices"));
}

} // namespace

PreferencesDialog::PreferencesDialog(const QStringList &availableVoices, const QString &defaultVoice,
                                      double defaultSpeed, const QFont &defaultFont,
                                      const DocumentView::Colors &defaultColors,
                                      const QStringList &defaultExtraVoiceDirs,
                                      int defaultExtraMarginSize, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_voiceCombo->addItems(availableVoices);
    if (!defaultVoice.isEmpty() && !availableVoices.contains(defaultVoice)) {
        m_voiceCombo->addItem(defaultVoice);
    }
    int voiceIndex = m_voiceCombo->findText(defaultVoice);
    if (voiceIndex >= 0) {
        m_voiceCombo->setCurrentIndex(voiceIndex);
    }

    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_speedSpin->setMinimumWidth(90);
    m_speedSpin->setRange(0.5, 3.0);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->setSuffix(tr("x"));
    m_speedSpin->setValue(defaultSpeed);

    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fontCombo->setCurrentFont(defaultFont);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setMinimumWidth(80);
    m_fontSizeSpin->setRange(6, 72);
    m_fontSizeSpin->setSuffix(tr(" pt"));
    m_fontSizeSpin->setValue(defaultFont.pointSize() > 0 ? defaultFont.pointSize() : 12);

    m_extraMarginSpin = new QSpinBox(this);
    m_extraMarginSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_extraMarginSpin->setMinimumWidth(90);
    m_extraMarginSpin->setRange(DocumentView::kMinExtraMarginSize, DocumentView::kMaxExtraMarginSize);
    m_extraMarginSpin->setSingleStep(8);
    m_extraMarginSpin->setSuffix(tr(" px"));
    m_extraMarginSpin->setToolTip(
        tr("Padding around the text when the Formatting → Margins button is on. "
           "Does not change the default tight layout."));
    m_extraMarginSpin->setValue(
        qBound(DocumentView::kMinExtraMarginSize, defaultExtraMarginSize,
              DocumentView::kMaxExtraMarginSize));

    auto *fontRow = new QWidget(this);
    auto *fontRowLayout = new QHBoxLayout(fontRow);
    fontRowLayout->setContentsMargins(0, 0, 0, 0);
    fontRowLayout->addWidget(m_fontCombo, /*stretch=*/3);
    fontRowLayout->addWidget(m_fontSizeSpin, /*stretch=*/1);

    const QString voicesDir = defaultUserVoicesDirPath();
    auto *voicesHelp = new QLabel(
        tr("Drop Piper <code>.onnx</code> and matching <code>.onnx.json</code> files here "
           "(any language). Created automatically if missing:"),
        this);
    voicesHelp->setWordWrap(true);
    voicesHelp->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *voicesPath = new QLabel(voicesDir, this);
    voicesPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    voicesPath->setWordWrap(true);
    voicesPath->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QFont pathFont = voicesPath->font();
    pathFont.setFamily(QStringLiteral("monospace"));
    pathFont.setStyleHint(QFont::Monospace);
    voicesPath->setFont(pathFont);

    m_extraVoiceDirsEdit = new QPlainTextEdit(this);
    m_extraVoiceDirsEdit->setPlaceholderText(
        tr("Optional: one extra directory per line\n/path/to/more/voices"));
    m_extraVoiceDirsEdit->setPlainText(defaultExtraVoiceDirs.join(QLatin1Char('\n')));
    m_extraVoiceDirsEdit->setFixedHeight(72);
    m_extraVoiceDirsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto makeForm = [](QWidget *parent) {
        auto *form = new QFormLayout(parent);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(8);
        form->setContentsMargins(10, 8, 10, 10);
        return form;
    };

    // --- Voice (startup defaults + model paths) -----------------------------
    auto *voiceGroup = new QGroupBox(tr("Voice"), this);
    auto *voiceForm = makeForm(voiceGroup);
    voiceForm->addRow(tr("Default voice:"), m_voiceCombo);
    voiceForm->addRow(tr("Default speed:"), m_speedSpin);
    voiceForm->addRow(voicesHelp);
    voiceForm->addRow(voicesPath);
    voiceForm->addRow(tr("Extra voice dirs:"), m_extraVoiceDirsEdit);

    // --- Formatting (font + reading column) --------------------------------
    auto *formatGroup = new QGroupBox(tr("Formatting"), this);
    auto *formatForm = makeForm(formatGroup);
    formatForm->addRow(tr("Font:"), fontRow);
    formatForm->addRow(tr("Extra text margin:"), m_extraMarginSpin);

    // --- Colors ------------------------------------------------------------
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    const DocumentView::Colors themeDefaults = DocumentView::builtinDefaults(dark);

    auto *colorsGroup = new QGroupBox(tr("Colors"), this);
    auto *colorsForm = makeForm(colorsGroup);
    m_textColor = addColorRow(colorsForm, tr("Text:"), defaultColors.text, themeDefaults.text);
    m_backgroundColor =
        addColorRow(colorsForm, tr("Background:"), defaultColors.background, themeDefaults.background);
    m_playingColor =
        addColorRow(colorsForm, tr("Playing highlight:"), defaultColors.playing, themeDefaults.playing);
    m_preparingColor =
        addColorRow(colorsForm, tr("Preparing highlight:"), defaultColors.preparing, themeDefaults.preparing);
    m_stoppedColor =
        addColorRow(colorsForm, tr("Stopped highlight:"), defaultColors.stopped, themeDefaults.stopped);
    m_hoverColor = addColorRow(colorsForm, tr("Hover highlight:"), defaultColors.hover, themeDefaults.hover);

    auto *note = new QLabel(
        tr("Save writes these as the defaults for the next time %1 starts "
           "(current session unchanged). Apply pushes voice/speed/font/colors/"
           "margin size into the current session only — extra voice directories "
           "always need a restart. The Margins button in the Formatting group "
           "turns the extra text margin on or off for this session. The reset "
           "button on each color restores the built-in theme default.")
            .arg(QStringLiteral("text2sprech")),
        this);
    note->setWordWrap(true);
    QFont noteFont = note->font();
    noteFont.setPointSizeF(noteFont.pointSizeF() * 0.9);
    note->setFont(noteFont);

    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(QDialogButtonBox::Save);
    auto *applyBtn = buttons->addButton(QDialogButtonBox::Apply);
    buttons->addButton(QDialogButtonBox::Cancel);
    saveBtn->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(applyBtn, &QPushButton::clicked, this, &PreferencesDialog::applyRequested);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    layout->addWidget(voiceGroup);
    layout->addWidget(formatGroup);
    layout->addWidget(colorsGroup);
    layout->addWidget(note);
    layout->addWidget(buttons);

    setMinimumWidth(560);
    adjustSize();
}

PreferencesDialog::ColorRow *PreferencesDialog::addColorRow(QFormLayout *form, const QString &label,
                                                            const QColor &value,
                                                            const QColor &themeDefault)
{
    auto *row = new ColorRow;
    row->label = label;
    row->value = value;
    row->themeDefault = themeDefault;

    auto *field = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(field);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(6);

    row->swatch = new QToolButton(field);
    row->swatch->setAutoRaise(false);
    row->swatch->setToolButtonStyle(Qt::ToolButtonIconOnly);
    row->swatch->setFixedSize(56, 28);
    row->swatch->setToolTip(tr("Choose color"));
    row->swatch->setFocusPolicy(Qt::TabFocus);
    connect(row->swatch, &QToolButton::clicked, this, [this, row] { pickColor(row); });

    row->reset = new QToolButton(field);
    row->reset->setAutoRaise(true);
    row->reset->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    row->reset->setToolTip(tr("Reset to default"));
    row->reset->setFocusPolicy(Qt::TabFocus);
    connect(row->reset, &QToolButton::clicked, this, [this, row] { resetColor(row); });

    rowLayout->addWidget(row->swatch);
    rowLayout->addWidget(row->reset);
    rowLayout->addStretch(1);

    form->addRow(label, field);
    updateSwatch(row);
    return row;
}

void PreferencesDialog::updateSwatch(ColorRow *row)
{
    const QColor shown = row->value.isValid() ? row->value : row->themeDefault;
    // Solid fill via stylesheet on the tool button — avoid setStyleSheet on
    // the whole dialog (that can force the CSS style engine). Scoped to the
    // single swatch widget is fine and matches prior Preferences fixes.
    row->swatch->setStyleSheet(
        QStringLiteral("QToolButton { background-color: %1; border: 1px solid palette(mid); "
                       "border-radius: 3px; }")
            .arg(shown.name(QColor::HexArgb)));
    row->reset->setEnabled(row->value.isValid());
}

void PreferencesDialog::pickColor(ColorRow *row)
{
    const QColor initial = row->value.isValid() ? row->value : row->themeDefault;
    const QColor chosen = QColorDialog::getColor(initial, this, tr("Choose %1").arg(row->label),
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }
    row->value = chosen;
    updateSwatch(row);
}

void PreferencesDialog::resetColor(ColorRow *row)
{
    row->value = QColor(); // invalid => built-in theme default
    updateSwatch(row);
}

QString PreferencesDialog::voice() const
{
    return m_voiceCombo->currentText();
}

double PreferencesDialog::speed() const
{
    return m_speedSpin->value();
}

QFont PreferencesDialog::font() const
{
    QFont f = m_fontCombo->currentFont();
    f.setPointSize(m_fontSizeSpin->value());
    return f;
}

DocumentView::Colors PreferencesDialog::colors() const
{
    DocumentView::Colors c;
    c.text = m_textColor->value;
    c.background = m_backgroundColor->value;
    c.playing = m_playingColor->value;
    c.preparing = m_preparingColor->value;
    c.stopped = m_stoppedColor->value;
    c.hover = m_hoverColor->value;
    return c;
}

QStringList PreferencesDialog::extraVoiceDirs() const
{
    QStringList out;
    if (!m_extraVoiceDirsEdit) {
        return out;
    }
    const QStringList lines = m_extraVoiceDirsEdit->toPlainText().split(
        QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            out << trimmed;
        }
    }
    return out;
}

int PreferencesDialog::extraMarginSize() const
{
    return m_extraMarginSpin ? m_extraMarginSpin->value() : DocumentView::kDefaultExtraMarginSize;
}
