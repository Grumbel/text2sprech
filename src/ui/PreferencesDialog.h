#pragma once

#include <QDialog>
#include <QFont>
#include <QString>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QSpinBox;

// Edits startup defaults (config file) and optionally applies them to the
// live session. Seeded from the saved defaults, not the current toolbar
// values.
//
//  - Save: write defaults to the config file and close (does not change
//    the live session by itself).
//  - Apply: push the dialog values into the current session (voice/speed/
//    font) without writing the config file or closing.
//  - Cancel: close without saving or applying.
//
// This is the *only* place in the app that writes to the config file.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // availableVoices seeds the voice dropdown. defaultVoice/defaultSpeed/
    // defaultFont are the values currently stored as startup defaults
    // (from the config file), not the live session's toolbar settings.
    PreferencesDialog(const QStringList &availableVoices, const QString &defaultVoice, double defaultSpeed,
                       const QFont &defaultFont, QWidget *parent = nullptr);

    QString voice() const;
    double speed() const;
    // Family + point size combined, since that's how the config file
    // stores them (and DocumentView::setBaseFont() accepts).
    QFont font() const;

signals:
    // Emitted when the user clicks Apply. Caller should push voice()/speed()/
    // font() into the live session. The dialog stays open.
    void applyRequested();

private:
    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QFontComboBox *m_fontCombo;
    QSpinBox *m_fontSizeSpin;
};
