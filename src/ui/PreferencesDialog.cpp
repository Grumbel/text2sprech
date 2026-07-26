#include "PreferencesDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
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
                                      const QStringList &defaultExtraVoiceDirs, QWidget *parent)
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

    auto *fontRow = new QWidget(this);
    auto *fontRowLayout = new QHBoxLayout(fontRow);
    fontRowLayout->setContentsMargins(0, 0, 0, 0);
    fontRowLayout->addWidget(m_fontCombo, /*stretch=*/3);
    fontRowLayout->addWidget(m_fontSizeSpin, /*stretch=*/1);

    const QString voicesDir = defaultUserVoicesDirPath();
    auto *voicesDirLabel = new QLabel(
        tr("<b>Voice models folder</b><br/>"
           "Drop Piper <code>.onnx</code> and matching <code>.onnx.json</code> files here "
           "(any language). Created automatically if missing:<br/><code>%1</code>")
            .arg(voicesDir),
        this);
    voicesDirLabel->setWordWrap(true);
    voicesDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_extraVoiceDirsEdit = new QPlainTextEdit(this);
    m_extraVoiceDirsEdit->setPlaceholderText(
        tr("Optional: one extra directory per line\n/path/to/more/voices"));
    m_extraVoiceDirsEdit->setPlainText(defaultExtraVoiceDirs.join(QLatin1Char('\n')));
    m_extraVoiceDirsEdit->setFixedHeight(72);
    m_extraVoiceDirsEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->addRow(tr("Voice:"), m_voiceCombo);
    form->addRow(tr("Speed:"), m_speedSpin);
    form->addRow(tr("Font:"), fontRow);
    form->addRow(tr("Voices:"), voicesDirLabel);
    form->addRow(tr("Extra voice dirs:"), m_extraVoiceDirsEdit);

    // Theme defaults for reset + swatch preview when no override is stored.
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    const DocumentView::Colors themeDefaults = DocumentView::builtinDefaults(dark);

    form->addRow(new QLabel(tr("Colors"), this));
    m_textColor = addColorRow(form, tr("Text:"), defaultColors.text, themeDefaults.text);
    m_backgroundColor =
        addColorRow(form, tr("Background:"), defaultColors.background, themeDefaults.background);
    m_playingColor =
        addColorRow(form, tr("Playing highlight:"), defaultColors.playing, themeDefaults.playing);
    m_preparingColor =
        addColorRow(form, tr("Preparing highlight:"), defaultColors.preparing, themeDefaults.preparing);
    m_stoppedColor =
        addColorRow(form, tr("Stopped highlight:"), defaultColors.stopped, themeDefaults.stopped);
    m_hoverColor = addColorRow(form, tr("Hover highlight:"), defaultColors.hover, themeDefaults.hover);

    auto *note = new QLabel(
        tr("Save writes these as the defaults for the next time %1 starts "
           "(current session unchanged). Apply pushes voice/speed/font/colors "
           "into the current session only — extra voice directories always need "
           "a restart. The reset button on each color restores the built-in "
           "theme default.")
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
    layout->setSpacing(14);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    setMinimumWidth(500);
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
