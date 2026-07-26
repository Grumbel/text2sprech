#include "PreferencesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

PreferencesDialog::PreferencesDialog(const QStringList &availableVoices, const QString &currentVoice,
                                      double currentSpeed, const QFont &currentFont, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_voiceCombo->addItems(availableVoices);
    if (!currentVoice.isEmpty() && !availableVoices.contains(currentVoice)) {
        // Not in the list yet (e.g. server hasn't reported voices this
        // session) -- add it anyway rather than silently dropping it.
        m_voiceCombo->addItem(currentVoice);
    }
    int voiceIndex = m_voiceCombo->findText(currentVoice);
    if (voiceIndex >= 0) {
        m_voiceCombo->setCurrentIndex(voiceIndex);
    }

    m_speedSpin = new QDoubleSpinBox(this);
    m_speedSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_speedSpin->setMinimumWidth(90);
    m_speedSpin->setRange(0.5, 3.0);
    m_speedSpin->setSingleStep(0.25);
    m_speedSpin->setSuffix(tr("x"));
    m_speedSpin->setValue(currentSpeed);

    // Real font family + point size, not an abstract "zoom" level -- a
    // QFontComboBox previews each entry in its own typeface, which reads
    // more clearly than a plain text list of family names would.
    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fontCombo->setCurrentFont(currentFont);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setMinimumWidth(80);
    m_fontSizeSpin->setRange(6, 72);
    m_fontSizeSpin->setSuffix(tr(" pt"));
    m_fontSizeSpin->setValue(currentFont.pointSize() > 0 ? currentFont.pointSize() : 12);

    // Family and size read as one concept ("Font: Lessac, 12pt"), so they
    // share a row -- a QWidget wrapper (not a bare QHBoxLayout added
    // directly as the field) so QFormLayout's field-growth policy applies
    // to it the same predictable way it does to a plain widget.
    auto *fontRow = new QWidget(this);
    auto *fontRowLayout = new QHBoxLayout(fontRow);
    fontRowLayout->setContentsMargins(0, 0, 0, 0);
    fontRowLayout->addWidget(m_fontCombo, /*stretch=*/3);
    fontRowLayout->addWidget(m_fontSizeSpin, /*stretch=*/1);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    form->addRow(tr("Voice:"), m_voiceCombo);
    form->addRow(tr("Speed:"), m_speedSpin);
    form->addRow(tr("Font:"), fontRow);

    // Plain QFont-based styling only -- setStyleSheet() on even one widget
    // in a dialog can push Qt onto its CSS-based style engine for the
    // whole subtree instead of the native platform style, which previously
    // caused text to render in a mismatched, barely-readable color
    // combination here.
    auto *note = new QLabel(
        tr("These become the defaults used next time %1 starts. Changes made here also "
           "apply immediately; changes made via the toolbar or command-line options "
           "are session-only and aren't saved.")
            .arg(QStringLiteral("text2sprech")),
        this);
    note->setWordWrap(true);
    QFont noteFont = note->font();
    noteFont.setPointSizeF(noteFont.pointSizeF() * 0.9);
    note->setFont(noteFont);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(14);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    setMinimumWidth(460);
    adjustSize();
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
