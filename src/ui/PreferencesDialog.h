#pragma once

#include <QDialog>
#include <QFont>
#include <QString>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QSpinBox;

// Lets the user pick a default voice, speed, font, and font size to
// persist to the config file (~/.config/text2sprech/text2sprech.ini). This
// is the *only* place in the app that writes to that file -- CLI flags and
// the regular toolbar controls (voice combo, speed spinbox, A-/A+) only
// ever affect the current session, by design (see
// MainWindow::openPreferences).
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    // availableVoices/currentVoice/currentSpeed/currentFont seed the dialog
    // with the live session's current values, so opening Preferences with
    // no changes and hitting Save just persists whatever is already in
    // effect.
    PreferencesDialog(const QStringList &availableVoices, const QString &currentVoice, double currentSpeed,
                       const QFont &currentFont, QWidget *parent = nullptr);

    QString voice() const;
    double speed() const;
    // Family + point size combined, since that's how DocumentView::
    // setBaseFont() and the config file both take it.
    QFont font() const;

private:
    QComboBox *m_voiceCombo;
    QDoubleSpinBox *m_speedSpin;
    QFontComboBox *m_fontCombo;
    QSpinBox *m_fontSizeSpin;
};
