// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PreferencesDialog.h"

#include "speech/PlaybackController.h"

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
#include <QTabWidget>
#include <QScrollArea>
#include <QFrame>
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
                                      int defaultExtraMarginPercent, double defaultLineSpacing,
                                      PresentationDefaults presentation, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_voiceCombo->setMinimumWidth(220);
    m_voiceCombo->addItems(availableVoices);
    if (!defaultVoice.isEmpty() && !availableVoices.contains(defaultVoice)) {
        m_voiceCombo->addItem(defaultVoice);
    }
    int voiceIndex = m_voiceCombo->findText(defaultVoice);
    if (voiceIndex >= 0) {
        m_voiceCombo->setCurrentIndex(voiceIndex);
    }

    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_speedSpin->setMinimumWidth(120);
    m_speedSpin->setRange(PlaybackController::kMinSpeed, PlaybackController::kMaxSpeed);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->setSuffix(tr("x"));
    m_speedSpin->setValue(defaultSpeed);

    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_fontCombo->setMinimumWidth(200);
    m_fontCombo->setCurrentFont(defaultFont);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_fontSizeSpin->setMinimumWidth(90);
    m_fontSizeSpin->setRange(DocumentView::kMinFontPointSize, DocumentView::kMaxFontPointSize);
    m_fontSizeSpin->setSuffix(tr(" pt"));
    m_fontSizeSpin->setValue(defaultFont.pointSize() > 0 ? defaultFont.pointSize() : 12);

    m_extraMarginSpin = new QSpinBox(this);
    m_extraMarginSpin->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_extraMarginSpin->setMinimumWidth(120);
    m_extraMarginSpin->setRange(DocumentView::kMinExtraMarginPercent,
                                DocumentView::kMaxExtraMarginPercent);
    m_extraMarginSpin->setSingleStep(1);
    m_extraMarginSpin->setSuffix(tr(" %"));
    m_extraMarginSpin->setToolTip(
        tr("Horizontal padding on each side as a percent of the document "
           "width when the Formatting → Margins button is on. Scales with "
           "window size. Does not change the default tight layout."));
    m_extraMarginSpin->setValue(
        qBound(DocumentView::kMinExtraMarginPercent, defaultExtraMarginPercent,
              DocumentView::kMaxExtraMarginPercent));

    m_lineSpacingSpin = new QDoubleSpinBox(this);
    m_lineSpacingSpin->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_lineSpacingSpin->setMinimumWidth(120);
    m_lineSpacingSpin->setRange(DocumentView::kMinLineSpacing, DocumentView::kMaxLineSpacing);
    m_lineSpacingSpin->setSingleStep(0.1);
    m_lineSpacingSpin->setDecimals(2);
    m_lineSpacingSpin->setSuffix(tr("×"));
    m_lineSpacingSpin->setToolTip(
        tr("Line height relative to single spacing (1.0 = single, 1.5 = one-and-a-half)."));
    m_lineSpacingSpin->setValue(
        qBound(DocumentView::kMinLineSpacing, defaultLineSpacing, DocumentView::kMaxLineSpacing));

    auto *fontRow = new QWidget(this);
    auto *fontRowLayout = new QHBoxLayout(fontRow);
    fontRowLayout->setContentsMargins(0, 0, 0, 0);
    fontRowLayout->addWidget(m_fontCombo, /*stretch=*/3);
    fontRowLayout->addWidget(m_fontSizeSpin, /*stretch=*/1);

    const QString voicesDir = defaultUserVoicesDirPath();
    auto *voicesPath = new QLabel(voicesDir, this);
    voicesPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    voicesPath->setWordWrap(true);
    voicesPath->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    voicesPath->setToolTip(
        tr("Drop Piper .onnx and matching .onnx.json files here (any language). "
           "The directory is created automatically if it is missing."));
    QFont pathFont = voicesPath->font();
    pathFont.setFamily(QStringLiteral("monospace"));
    pathFont.setStyleHint(QFont::Monospace);
    voicesPath->setFont(pathFont);

    m_extraVoiceDirsEdit = new QPlainTextEdit(this);
    m_extraVoiceDirsEdit->setPlaceholderText(
        tr("Optional: one extra directory per line\n/path/to/more/voices"));
    m_extraVoiceDirsEdit->setPlainText(defaultExtraVoiceDirs.join(QLatin1Char('\n')));
    m_extraVoiceDirsEdit->setFixedHeight(56);
    m_extraVoiceDirsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_extraVoiceDirsEdit->setToolTip(
        tr("Additional directories scanned for Piper voices on next start "
           "(the user voices folder above is always included)."));

    auto makeForm = [](QWidget *parent) {
        auto *form = new QFormLayout(parent);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(10);
        form->setContentsMargins(12, 10, 12, 12);
        return form;
    };

    // --- Voice (startup defaults + model paths) -----------------------------
    auto *voiceGroup = new QGroupBox(tr("Voice"), this);
    voiceGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto *voiceForm = makeForm(voiceGroup);
    voiceForm->addRow(tr("Default voice:"), m_voiceCombo);
    voiceForm->addRow(tr("Default speed:"), m_speedSpin);
    voiceForm->addRow(tr("User voices folder:"), voicesPath);
    voiceForm->addRow(tr("Extra voice dirs:"), m_extraVoiceDirsEdit);

    // --- Reading / Presentation tabs (font, spacing, colors) ----------------
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    const DocumentView::Colors themeDefaults = DocumentView::builtinDefaults(dark);
    const DocumentView::Colors presentationBuiltins = DocumentView::presentationDefaults();

    auto *readingInner = new QWidget(this);
    auto *readingLayout = new QVBoxLayout(readingInner);
    readingLayout->setContentsMargins(8, 10, 8, 10);
    readingLayout->setSpacing(14);

    auto *formatGroup = new QGroupBox(tr("Formatting"), readingInner);
    formatGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *formatForm = makeForm(formatGroup);
    formatForm->addRow(tr("Font:"), fontRow);
    formatForm->addRow(tr("Extra text margin (each side):"), m_extraMarginSpin);
    formatForm->addRow(tr("Line spacing:"), m_lineSpacingSpin);

    auto *colorsGroup = new QGroupBox(tr("Colors"), readingInner);
    colorsGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
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

    readingLayout->addWidget(formatGroup);
    readingLayout->addWidget(colorsGroup);
    readingLayout->addStretch(1);

    auto *readingPage = new QScrollArea(this);
    readingPage->setWidget(readingInner);
    readingPage->setWidgetResizable(true);
    readingPage->setFrameShape(QFrame::NoFrame);
    readingPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    readingPage->setMinimumHeight(320);

    m_presentationFontCombo = new QFontComboBox(this);
    m_presentationFontCombo->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_presentationFontCombo->setMinimumWidth(200);
    {
        QFont seed = defaultFont;
        if (!presentation.fontFamily.isEmpty()) {
            seed.setFamily(presentation.fontFamily);
        }
        m_presentationFontCombo->setCurrentFont(seed);
    }

    m_presentationFontSizeSpin = new QSpinBox(this);
    m_presentationFontSizeSpin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_presentationFontSizeSpin->setMinimumWidth(90);
    m_presentationFontSizeSpin->setRange(DocumentView::kMinFontPointSize,
                                         DocumentView::kMaxFontPointSize);
    m_presentationFontSizeSpin->setSuffix(tr(" pt"));
    m_presentationFontSizeSpin->setToolTip(
        tr("Font used while Presentation Mode is active."));
    m_presentationFontSizeSpin->setValue(
        qBound(DocumentView::kMinFontPointSize,
              presentation.fontPointSize > 0 ? presentation.fontPointSize
                                             : DocumentView::kPresentationFontPointSize,
              DocumentView::kMaxFontPointSize));

    auto *presentationFontRow = new QWidget(this);
    auto *presentationFontRowLayout = new QHBoxLayout(presentationFontRow);
    presentationFontRowLayout->setContentsMargins(0, 0, 0, 0);
    presentationFontRowLayout->addWidget(m_presentationFontCombo, /*stretch=*/3);
    presentationFontRowLayout->addWidget(m_presentationFontSizeSpin, /*stretch=*/1);

    m_presentationMarginSpin = new QSpinBox(this);
    m_presentationMarginSpin->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_presentationMarginSpin->setMinimumWidth(120);
    m_presentationMarginSpin->setRange(DocumentView::kMinExtraMarginPercent,
                                       DocumentView::kMaxExtraMarginPercent);
    m_presentationMarginSpin->setSingleStep(1);
    m_presentationMarginSpin->setSuffix(tr(" %"));
    m_presentationMarginSpin->setToolTip(
        tr("Horizontal padding on each side as a percent of the document "
           "width while Presentation Mode is active."));
    m_presentationMarginSpin->setValue(
        qBound(DocumentView::kMinExtraMarginPercent, presentation.extraMarginPercent,
              DocumentView::kMaxExtraMarginPercent));

    m_presentationLineSpacingSpin = new QDoubleSpinBox(this);
    m_presentationLineSpacingSpin->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_presentationLineSpacingSpin->setMinimumWidth(120);
    m_presentationLineSpacingSpin->setRange(DocumentView::kMinLineSpacing,
                                            DocumentView::kMaxLineSpacing);
    m_presentationLineSpacingSpin->setSingleStep(0.1);
    m_presentationLineSpacingSpin->setDecimals(2);
    m_presentationLineSpacingSpin->setSuffix(tr("×"));
    m_presentationLineSpacingSpin->setToolTip(
        tr("Line height in Presentation Mode relative to single spacing."));
    m_presentationLineSpacingSpin->setValue(
        qBound(DocumentView::kMinLineSpacing, presentation.lineSpacing,
              DocumentView::kMaxLineSpacing));

    auto *presentationInner = new QWidget(this);
    auto *presentationLayout = new QVBoxLayout(presentationInner);
    presentationLayout->setContentsMargins(8, 10, 8, 10);
    presentationLayout->setSpacing(14);

    auto *presentationFormatGroup = new QGroupBox(tr("Formatting"), presentationInner);
    presentationFormatGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *presentationFormatForm = makeForm(presentationFormatGroup);
    presentationFormatForm->addRow(tr("Font:"), presentationFontRow);
    presentationFormatForm->addRow(tr("Extra text margin (each side):"), m_presentationMarginSpin);
    presentationFormatForm->addRow(tr("Line spacing:"), m_presentationLineSpacingSpin);

    auto *presentationColorsGroup = new QGroupBox(tr("Colors"), presentationInner);
    presentationColorsGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *presentationColorsForm = makeForm(presentationColorsGroup);
    m_presentationTextColor =
        addColorRow(presentationColorsForm, tr("Text:"), presentation.colors.text, presentationBuiltins.text);
    m_presentationBackgroundColor =
        addColorRow(presentationColorsForm, tr("Background:"), presentation.colors.background,
                    presentationBuiltins.background);
    m_presentationPlayingColor =
        addColorRow(presentationColorsForm, tr("Playing highlight:"), presentation.colors.playing,
                    presentationBuiltins.playing);
    m_presentationPreparingColor =
        addColorRow(presentationColorsForm, tr("Preparing highlight:"), presentation.colors.preparing,
                    presentationBuiltins.preparing);
    m_presentationStoppedColor =
        addColorRow(presentationColorsForm, tr("Stopped highlight:"), presentation.colors.stopped,
                    presentationBuiltins.stopped);
    m_presentationHoverColor =
        addColorRow(presentationColorsForm, tr("Hover highlight:"), presentation.colors.hover,
                    presentationBuiltins.hover);

    presentationLayout->addWidget(presentationFormatGroup);
    presentationLayout->addWidget(presentationColorsGroup);
    presentationLayout->addStretch(1);

    auto *presentationPage = new QScrollArea(this);
    presentationPage->setWidget(presentationInner);
    presentationPage->setWidgetResizable(true);
    presentationPage->setFrameShape(QFrame::NoFrame);
    presentationPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    presentationPage->setMinimumHeight(320);

    auto *tabs = new QTabWidget(this);
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    tabs->setDocumentMode(false);
    tabs->setUsesScrollButtons(false);
    tabs->addTab(readingPage, tr("Reading"));
    tabs->addTab(presentationPage, tr("Presentation"));

    auto *buttons = new QDialogButtonBox(this);
    auto *saveBtn = buttons->addButton(QDialogButtonBox::Save);
    auto *closeBtn = buttons->addButton(QDialogButtonBox::Close);
    saveBtn->setDefault(true);
    // Save writes startup defaults (see saveRequested); Close only dismisses.
    // Reject role makes Esc/Close button call reject() and leave the config alone.
    connect(saveBtn, &QPushButton::clicked, this, &PreferencesDialog::saveRequested);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    wireLiveApply();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    // Voice is compact; tabs (with scrollable pages) take the remaining space
    // so formatting/color rows are never crushed by the voice help text.
    voiceGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    tabs->setMinimumHeight(360);
    layout->addWidget(voiceGroup, /*stretch=*/0);
    layout->addWidget(tabs, /*stretch=*/1);
    layout->addWidget(buttons, /*stretch=*/0);

    setMinimumSize(660, 640);
    resize(720, 760);
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
    field->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    auto *rowLayout = new QHBoxLayout(field);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    row->swatch = new QToolButton(field);
    row->swatch->setAutoRaise(false);
    row->swatch->setToolButtonStyle(Qt::ToolButtonIconOnly);
    row->swatch->setFixedSize(72, 28);
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
    emit applyRequested();
}

void PreferencesDialog::resetColor(ColorRow *row)
{
    row->value = QColor(); // invalid => built-in theme default
    updateSwatch(row);
    emit applyRequested();
}


void PreferencesDialog::wireLiveApply()
{
    // Session preview on every change. Voice-dir text is next-start only and
    // is not connected here.
    const auto apply = [this] { emit applyRequested(); };

    connect(m_voiceCombo, &QComboBox::currentTextChanged, this, [apply](const QString &) { apply(); });
    connect(m_speedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [apply](double) { apply(); });
    connect(m_fontCombo, &QFontComboBox::currentFontChanged, this, [apply](const QFont &) { apply(); });
    connect(m_fontSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [apply](int) { apply(); });
    connect(m_extraMarginSpin, qOverload<int>(&QSpinBox::valueChanged), this, [apply](int) { apply(); });
    connect(m_lineSpacingSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [apply](double) { apply(); });

    connect(m_presentationFontCombo, &QFontComboBox::currentFontChanged, this,
            [apply](const QFont &) { apply(); });
    connect(m_presentationFontSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [apply](int) { apply(); });
    connect(m_presentationMarginSpin, qOverload<int>(&QSpinBox::valueChanged), this,
            [apply](int) { apply(); });
    connect(m_presentationLineSpacingSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [apply](double) { apply(); });
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

int PreferencesDialog::extraMarginPercent() const
{
    return m_extraMarginSpin ? m_extraMarginSpin->value()
                             : DocumentView::kDefaultExtraMarginPercent;
}

double PreferencesDialog::lineSpacing() const
{
    return m_lineSpacingSpin ? m_lineSpacingSpin->value()
                             : DocumentView::kDefaultLineSpacing;
}

QFont PreferencesDialog::presentationFont() const
{
    QFont f = m_presentationFontCombo ? m_presentationFontCombo->currentFont() : QFont();
    f.setPointSize(presentationFontPointSize());
    return f;
}

int PreferencesDialog::presentationFontPointSize() const
{
    return m_presentationFontSizeSpin ? m_presentationFontSizeSpin->value()
                                      : DocumentView::kPresentationFontPointSize;
}

int PreferencesDialog::presentationExtraMarginPercent() const
{
    return m_presentationMarginSpin ? m_presentationMarginSpin->value()
                                    : DocumentView::kDefaultExtraMarginPercent;
}

double PreferencesDialog::presentationLineSpacing() const
{
    return m_presentationLineSpacingSpin ? m_presentationLineSpacingSpin->value()
                                         : DocumentView::kDefaultPresentationLineSpacing;
}

DocumentView::Colors PreferencesDialog::presentationColors() const
{
    DocumentView::Colors c;
    c.text = m_presentationTextColor ? m_presentationTextColor->value : QColor();
    c.background =
        m_presentationBackgroundColor ? m_presentationBackgroundColor->value : QColor();
    c.playing = m_presentationPlayingColor ? m_presentationPlayingColor->value : QColor();
    c.preparing =
        m_presentationPreparingColor ? m_presentationPreparingColor->value : QColor();
    c.stopped = m_presentationStoppedColor ? m_presentationStoppedColor->value : QColor();
    c.hover = m_presentationHoverColor ? m_presentationHoverColor->value : QColor();
    return c;
}
