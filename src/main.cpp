// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>
#include <QColor>
#include <QSettings>

#include "ui/MainWindow.h"

namespace {

// Config file lives at (Linux) ~/.config/text2sprech/text2sprech.ini -- see
// README for the full key list. CLI flags always take precedence over these:
// a config value just changes the *default* used when a flag isn't given
// (implemented via QCommandLineOption's defaultValue, so parser.value()
// naturally returns the config value when the user didn't pass the flag).
// Keys live under the [startup] group.
struct FileConfig {
    QString voice;
    double speed = 1.0;
    QString fontFamily;
    int fontPointSize = -1; // <= 0 means "leave the widget's default size alone"
    DocumentView::Colors colors; // invalid entries = built-in theme defaults
    int extraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    int presentationFontPointSize = DocumentView::kPresentationFontPointSize;
    int presentationExtraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    DocumentView::Colors presentationColors; // invalid → presentationDefaults()
};

QColor readColorKey(QSettings &settings, const QString &key)
{
    const QString s = settings.value(key).toString();
    if (s.isEmpty()) {
        return QColor();
    }
    QColor c(s);
    return c.isValid() ? c : QColor();
}

FileConfig loadFileConfig()
{
    FileConfig config;
    QSettings settings;
    settings.beginGroup(QStringLiteral("startup"));

    config.voice = settings.value(QStringLiteral("voice")).toString();
    config.fontFamily = settings.value(QStringLiteral("fontFamily")).toString();

    bool speedOk = false;
    double speed = settings.value(QStringLiteral("speed"), 1.0).toDouble(&speedOk);
    if (speedOk && speed >= 0.5 && speed <= 3.0) {
        config.speed = speed;
    } else if (settings.contains(QStringLiteral("speed"))) {
        qWarning().noquote() << "Config: invalid speed value" << settings.value(QStringLiteral("speed"))
                              << "(expected 0.5-3.0); using 1.0";
    }

    bool sizeOk = false;
    int pointSize = settings.value(QStringLiteral("fontPointSize"), -1).toInt(&sizeOk);
    if (sizeOk && pointSize >= 6 && pointSize <= 72) {
        config.fontPointSize = pointSize;
    } else if (settings.contains(QStringLiteral("fontPointSize"))) {
        qWarning().noquote() << "Config: invalid fontPointSize value"
                              << settings.value(QStringLiteral("fontPointSize"))
                              << "(expected 6-72); using the widget default";
    }

    config.colors.text = readColorKey(settings, QStringLiteral("colorText"));
    config.colors.background = readColorKey(settings, QStringLiteral("colorBackground"));
    config.colors.playing = readColorKey(settings, QStringLiteral("colorPlaying"));
    config.colors.preparing = readColorKey(settings, QStringLiteral("colorPreparing"));
    config.colors.stopped = readColorKey(settings, QStringLiteral("colorStopped"));
    config.colors.hover = readColorKey(settings, QStringLiteral("colorHover"));

    bool marginOk = false;
    const int margin = settings.value(QStringLiteral("extraMarginPercent"),
                                      DocumentView::kDefaultExtraMarginPercent)
                           .toInt(&marginOk);
    if (marginOk && margin >= DocumentView::kMinExtraMarginPercent
        && margin <= DocumentView::kMaxExtraMarginPercent) {
        config.extraMarginPercent = margin;
    } else if (settings.contains(QStringLiteral("extraMarginPercent"))) {
        qWarning().noquote() << "Config: invalid extraMarginPercent value"
                              << settings.value(QStringLiteral("extraMarginPercent"))
                              << "(expected" << DocumentView::kMinExtraMarginPercent << "-"
                              << DocumentView::kMaxExtraMarginPercent << "); using"
                              << DocumentView::kDefaultExtraMarginPercent;
    }

    settings.endGroup();

    settings.beginGroup(QStringLiteral("presentation"));
    bool presSizeOk = false;
    const int presPs = settings.value(QStringLiteral("fontPointSize"),
                                      DocumentView::kPresentationFontPointSize)
                           .toInt(&presSizeOk);
    if (presSizeOk && presPs >= 6 && presPs <= 72) {
        config.presentationFontPointSize = presPs;
    } else if (settings.contains(QStringLiteral("fontPointSize"))) {
        qWarning().noquote() << "Config: invalid presentation fontPointSize value"
                              << settings.value(QStringLiteral("fontPointSize"))
                              << "(expected 6-72); using"
                              << DocumentView::kPresentationFontPointSize;
    }
    bool presMarginOk = false;
    const int presMargin = settings.value(QStringLiteral("extraMarginPercent"),
                                          DocumentView::kDefaultExtraMarginPercent)
                               .toInt(&presMarginOk);
    if (presMarginOk && presMargin >= DocumentView::kMinExtraMarginPercent
        && presMargin <= DocumentView::kMaxExtraMarginPercent) {
        config.presentationExtraMarginPercent = presMargin;
    } else if (settings.contains(QStringLiteral("extraMarginPercent"))) {
        qWarning().noquote() << "Config: invalid presentation extraMarginPercent value"
                              << settings.value(QStringLiteral("extraMarginPercent"))
                              << "(expected" << DocumentView::kMinExtraMarginPercent << "-"
                              << DocumentView::kMaxExtraMarginPercent << "); using"
                              << DocumentView::kDefaultExtraMarginPercent;
    }
    config.presentationColors.text = readColorKey(settings, QStringLiteral("colorText"));
    config.presentationColors.background = readColorKey(settings, QStringLiteral("colorBackground"));
    config.presentationColors.playing = readColorKey(settings, QStringLiteral("colorPlaying"));
    config.presentationColors.preparing = readColorKey(settings, QStringLiteral("colorPreparing"));
    config.presentationColors.stopped = readColorKey(settings, QStringLiteral("colorStopped"));
    config.presentationColors.hover = readColorKey(settings, QStringLiteral("colorHover"));
    settings.endGroup();

    return config;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("text2sprech");
    QApplication::setOrganizationName("text2sprech");
    QApplication::setApplicationVersion("0.1.0");
    // Embedded via resources/text2sprech.qrc (compiled in through AUTORCC) so
    // the icon is available even without the .desktop file's icon-theme
    // lookup finding it -- e.g. running the binary directly during dev.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/text2sprech.png")));
    // Force a plain .ini file on every platform (rather than the Windows
    // registry / macOS plist QSettings would otherwise use), so the config
    // file is always something a user can open in a text editor.
    QSettings::setDefaultFormat(QSettings::IniFormat);

    const FileConfig fileConfig = loadFileConfig();

    QCommandLineParser parser;
    parser.setApplicationDescription("text2sprech -- a local TTS reader with live sentence highlighting");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption voiceOption(
        QStringList() << "voice",
        "Voice to select on startup (must match a voice the piper-server reports). "
        "Falls back to the server's default and prints a warning if not found. "
        "Defaults to the config file's [startup] voice if set.",
        "name", fileConfig.voice);
    QCommandLineOption speedOption(
        QStringList() << "speed",
        "Initial playback speed, 0.5-3.0. Defaults to the config file's "
        "[startup] speed if set, otherwise 1.0.",
        "factor", QString::number(fileConfig.speed));
    QCommandLineOption autoplayOption(
        QStringList() << "autoplay",
        "Start playback automatically as soon as a document is loaded "
        "(via Open…, Paste, or the optional [file] argument below).");
    QCommandLineOption piperSocketOption(
        QStringList() << "piper-socket",
        "Connect to an already-running piper-server at this socket path "
        "instead of starting one automatically. The app will not manage "
        "the lifecycle of a server given this way (it won't be stopped "
        "when the app closes).",
        "path");
    QCommandLineOption pasteClipboardOption(
        QStringList() << "paste-clipboard",
        "Load the system clipboard's contents on startup instead of the "
        "[file] argument (HTML if the clipboard has it, plain text otherwise).");
    QCommandLineOption pasteSelectionOption(
        QStringList() << "paste-selection",
        "Load the primary selection (last text selected/highlighted, X11/"
        "Wayland-with-support only) on startup instead of the [file] argument.");
    parser.addOption(voiceOption);
    parser.addOption(speedOption);
    parser.addOption(autoplayOption);
    parser.addOption(piperSocketOption);
    parser.addOption(pasteClipboardOption);
    parser.addOption(pasteSelectionOption);
    parser.addPositionalArgument(
        "file",
        "Optional text/HTML file to open on startup. Pass - to read the document from stdin.",
        "[file]");

    parser.process(app);

    MainWindow::Options options;
    options.voice = parser.value(voiceOption);

    bool speedOk = false;
    double speed = parser.value(speedOption).toDouble(&speedOk);
    if (!speedOk || speed < 0.5 || speed > 3.0) {
        if (parser.isSet(speedOption)) {
            qWarning().noquote() << "Invalid --speed value" << parser.value(speedOption)
                                  << "(expected 0.5-3.0); using 1.0";
        }
        speed = 1.0;
    }
    options.speed = speed;
    options.autoplay = parser.isSet(autoplayOption);
    options.piperSocket = parser.value(piperSocketOption);
    options.pasteClipboard = parser.isSet(pasteClipboardOption);
    options.pasteSelection = parser.isSet(pasteSelectionOption);
    options.fontFamily = fileConfig.fontFamily; // config-file only, no CLI flag
    options.fontPointSize = fileConfig.fontPointSize; // config-file only, no CLI flag
    options.colors = fileConfig.colors; // config-file only, no CLI flag
    options.extraMarginPercent = fileConfig.extraMarginPercent; // config-file only, no CLI flag
    options.presentationFontPointSize = fileConfig.presentationFontPointSize;
    options.presentationExtraMarginPercent = fileConfig.presentationExtraMarginPercent;
    options.presentationColors = fileConfig.presentationColors;

    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.initialFile = positional.first();
    }

    MainWindow window(options);
    window.show();

    return QApplication::exec();
}
