#include "PreferencesDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

PreferencesDialog::PreferencesDialog(const QStringList &availableVoices, const QString &defaultVoice,
                                      double defaultSpeed, const QFont &defaultFont, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    m_voiceCombo = new QComboBox(this);
    m_voiceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_voiceCombo->addItems(availableVoices);
    if (!defaultVoice.isEmpty() && !availableVoices.contains(defaultVoice)) {
        // Saved default isn't in the list yet (e.g. server hasn't reported
        // voices this session, or the voice was removed) -- keep it visible
        // rather than silently dropping the stored preference.
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

    // Real font family + point size, not an abstract "zoom" level -- a
    // QFontComboBox previews each entry in its own typeface, which reads
    // more clearly than a plain text list of family names would.
    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fontCombo->setCurrentFont(defaultFont);

    m_fontSizeSpin = new QSpinBox(this);
    m_fontSizeSpin->setMinimumWidth(80);
    m_fontSizeSpin->setRange(6, 72);
    m_fontSizeSpin->setSuffix(tr(" pt"));
    m_fontSizeSpin->setValue(defaultFont.pointSize() > 0 ? defaultFont.pointSize() : 12);

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
        tr("Save writes these as the defaults for the next time %1 starts "
           "(current session unchanged). Apply pushes them into the current "
           "session only and does not update the saved defaults.")
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
    // Make Save the default action (Enter), not Apply.
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
