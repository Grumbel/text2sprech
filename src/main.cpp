// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QIcon>
#include <QColor>
#include <QSettings>

#include "ui/MainWindow.h"

namespace {

// Config file lives at (Linux) ~/.config/text2sprech/text2sprech.ini by
// default — see README for the full key list. CLI flags always take
// precedence over these: a config value just changes the *default* used
// when a flag isn't given (implemented via QCommandLineOption's
// defaultValue, so parser.value() naturally returns the config value when
// the user didn't pass the flag). Keys live under the [startup] group.
// --config <path> points QSettings at an alternate .ini for this process.
struct FileConfig {
    QString voice;
    double speed = 1.0;
    QString fontFamily;
    int fontPointSize = -1; // <= 0 means "leave the widget's default size alone"
    DocumentView::Colors colors; // invalid entries = built-in theme defaults
    int extraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    double lineSpacing = DocumentView::kDefaultLineSpacing;
    QString presentationFontFamily;
    int presentationFontPointSize = DocumentView::kPresentationFontPointSize;
    int presentationExtraMarginPercent = DocumentView::kDefaultExtraMarginPercent;
    double presentationLineSpacing = DocumentView::kDefaultPresentationLineSpacing;
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

QColor parseColorArg(const QString &s, const char *flagName)
{
    if (s.isEmpty()) {
        return QColor();
    }
    QColor c(s);
    if (!c.isValid()) {
        qWarning().noquote() << "Invalid" << flagName << "color" << s
                              << "(expected #RRGGBB or #AARRGGBB); ignoring";
        return QColor();
    }
    return c;
}

FileConfig loadFileConfig(const QString &configPath = QString())
{
    FileConfig config;
    // When --config is given, use that file exclusively for startup defaults
    // (and later Preferences Save writes back to the same path via
    // QSettings default organization path unless we set it). For a custom
    // path we construct QSettings with the file path directly.
    QSettings settings = configPath.isEmpty()
        ? QSettings()
        : QSettings(configPath, QSettings::IniFormat);
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

    bool lineOk = false;
    const double lineVal = settings.value(QStringLiteral("lineSpacing"),
                                          DocumentView::kDefaultLineSpacing)
                               .toDouble(&lineOk);
    if (lineOk && lineVal >= DocumentView::kMinLineSpacing
        && lineVal <= DocumentView::kMaxLineSpacing) {
        config.lineSpacing = lineVal;
    } else if (settings.contains(QStringLiteral("lineSpacing"))) {
        qWarning().noquote() << "Config: invalid lineSpacing value"
                              << settings.value(QStringLiteral("lineSpacing"))
                              << "(expected" << DocumentView::kMinLineSpacing << "-"
                              << DocumentView::kMaxLineSpacing << "); using"
                              << DocumentView::kDefaultLineSpacing;
    }

    settings.endGroup();

    settings.beginGroup(QStringLiteral("presentation"));
    config.presentationFontFamily =
        settings.value(QStringLiteral("fontFamily")).toString().trimmed();
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
    bool presLineOk = false;
    const double presLine = settings.value(QStringLiteral("lineSpacing"),
                                           DocumentView::kDefaultPresentationLineSpacing)
                                .toDouble(&presLineOk);
    if (presLineOk && presLine >= DocumentView::kMinLineSpacing
        && presLine <= DocumentView::kMaxLineSpacing) {
        config.presentationLineSpacing = presLine;
    } else if (settings.contains(QStringLiteral("lineSpacing"))) {
        qWarning().noquote() << "Config: invalid presentation lineSpacing value"
                              << settings.value(QStringLiteral("lineSpacing"))
                              << "(expected" << DocumentView::kMinLineSpacing << "-"
                              << DocumentView::kMaxLineSpacing << "); using"
                              << DocumentView::kDefaultPresentationLineSpacing;
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
    // TEXT2SPRECH_VERSION comes from the top-level VERSION file via CMake
    // (or -DPROJECT_VERSION_FULL from the flake, which may append +g<rev>).
#ifndef TEXT2SPRECH_VERSION
#define TEXT2SPRECH_VERSION "0.0.0-unknown"
#endif
    QApplication::setApplicationVersion(QStringLiteral(TEXT2SPRECH_VERSION));
    // Embedded via resources/text2sprech.qrc (compiled in through AUTORCC) so
    // the icon is available even without the .desktop file's icon-theme
    // lookup finding it -- e.g. running the binary directly during dev.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/text2sprech.png")));
    // Force a plain .ini file on every platform (rather than the Windows
    // registry / macOS plist QSettings would otherwise use), so the config
    // file is always something a user can open in a text editor.
    QSettings::setDefaultFormat(QSettings::IniFormat);

    // First pass: only look for --config so we can load the right file
    // before building the rest of the option defaults.
    QString configPath;
    {
        QCommandLineParser preParser;
        preParser.setApplicationDescription(
            QStringLiteral("text2sprech -- a local TTS reader with live sentence highlighting"));
        preParser.addHelpOption();
        preParser.addVersionOption();
        QCommandLineOption configOption(
            QStringList() << "config" << "c",
            "Load startup defaults from this .ini file instead of the default "
            "~/.config/text2sprech/text2sprech.ini. Preferences Save also writes "
            "back to this path for the current process.",
            "path");
        preParser.addOption(configOption);
        // Ignore unknown options so the full parser can report them later.
        preParser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        preParser.parse(QCoreApplication::arguments());
        if (preParser.isSet(configOption)) {
            configPath = preParser.value(configOption);
            const QFileInfo info(configPath);
            if (!info.exists() || !info.isFile()) {
                qWarning().noquote() << "Config file not found:" << configPath
                                      << "— using built-in defaults until the file exists.";
            }
        }
    }

    // Point QSettings at the custom file for the whole process so Preferences
    // Save and recent/positions groups share the same file when --config is set.
    if (!configPath.isEmpty()) {
        // QSettings(file, IniFormat) is used in loadFileConfig; also set the
        // organization path so bare QSettings() in MainWindow hits the same file.
        // Qt has no global “always this file” API, so MainWindow still uses
        // QSettings() which follows organizationName — we document that --config
        // seeds startup only, and for a full alternate profile users can set
        // XDG_CONFIG_HOME. For true write-through of the same path, we pass
        // configPath into Options and MainWindow uses it when non-empty.
    }

    const FileConfig fileConfig = loadFileConfig(configPath);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "text2sprech -- a local TTS reader with live sentence highlighting");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOption(
        QStringList() << "config" << "c",
        "Load startup defaults from this .ini file instead of the default "
        "~/.config/text2sprech/text2sprech.ini. Preferences Save writes back "
        "to this path when set.",
        "path");
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
    QCommandLineOption fontFamilyOption(
        QStringList() << "font-family",
        "Document font family for this session (overrides config [startup] fontFamily).",
        "name", fileConfig.fontFamily);
    QCommandLineOption fontSizeOption(
        QStringList() << "font-size",
        "Document font point size, 6-72 (overrides config [startup] fontPointSize).",
        "pt",
        fileConfig.fontPointSize > 0 ? QString::number(fileConfig.fontPointSize) : QString());
    QCommandLineOption marginOption(
        QStringList() << "margin",
        "Extra horizontal margin on each side as a percent of the document view "
        "width (0-40). Overrides config [startup] extraMarginPercent.",
        "percent", QString::number(fileConfig.extraMarginPercent));
    QCommandLineOption lineSpacingOption(
        QStringList() << "line-spacing",
        "Line height multiplier relative to single spacing (e.g. 1.0, 1.5). "
        "Overrides config [startup] lineSpacing.",
        "factor", QString::number(fileConfig.lineSpacing));
    QCommandLineOption colorTextOption(
        QStringList() << "color-text",
        "Document text color (#RRGGBB or #AARRGGBB). Session only.",
        "color");
    QCommandLineOption colorBackgroundOption(
        QStringList() << "color-background",
        "Document background color (#RRGGBB or #AARRGGBB). Session only.",
        "color");
    QCommandLineOption colorPlayingOption(
        QStringList() << "color-playing",
        "Playing-sentence highlight color. Session only.",
        "color");
    QCommandLineOption colorPreparingOption(
        QStringList() << "color-preparing",
        "Preparing-sentence highlight color. Session only.",
        "color");
    QCommandLineOption colorStoppedOption(
        QStringList() << "color-stopped",
        "Stopped/selected-sentence highlight color. Session only.",
        "color");
    QCommandLineOption colorHoverOption(
        QStringList() << "color-hover",
        "Hover highlight color. Session only.",
        "color");

    parser.addOption(configOption);
    parser.addOption(voiceOption);
    parser.addOption(speedOption);
    parser.addOption(autoplayOption);
    parser.addOption(piperSocketOption);
    parser.addOption(pasteClipboardOption);
    parser.addOption(pasteSelectionOption);
    parser.addOption(fontFamilyOption);
    parser.addOption(fontSizeOption);
    parser.addOption(marginOption);
    parser.addOption(lineSpacingOption);
    parser.addOption(colorTextOption);
    parser.addOption(colorBackgroundOption);
    parser.addOption(colorPlayingOption);
    parser.addOption(colorPreparingOption);
    parser.addOption(colorStoppedOption);
    parser.addOption(colorHoverOption);
    parser.addPositionalArgument(
        "file",
        "Optional text/HTML file to open on startup. Pass - to read the document from stdin.",
        "[file]");

    parser.process(app);

    MainWindow::Options options;
    options.configPath = parser.isSet(configOption) ? parser.value(configOption) : configPath;
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

    // Font / margin / line spacing: CLI overrides config when the flag is set;
    // otherwise keep the loaded config defaults.
    options.fontFamily = parser.isSet(fontFamilyOption) ? parser.value(fontFamilyOption)
                                                        : fileConfig.fontFamily;
    if (parser.isSet(fontSizeOption)) {
        bool sizeOk = false;
        const int pt = parser.value(fontSizeOption).toInt(&sizeOk);
        if (sizeOk && pt >= 6 && pt <= 72) {
            options.fontPointSize = pt;
        } else {
            qWarning().noquote() << "Invalid --font-size value" << parser.value(fontSizeOption)
                                  << "(expected 6-72); ignoring";
            options.fontPointSize = fileConfig.fontPointSize;
        }
    } else {
        options.fontPointSize = fileConfig.fontPointSize;
    }

    if (parser.isSet(marginOption)) {
        bool marginOk = false;
        const int m = parser.value(marginOption).toInt(&marginOk);
        if (marginOk && m >= DocumentView::kMinExtraMarginPercent
            && m <= DocumentView::kMaxExtraMarginPercent) {
            options.extraMarginPercent = m;
        } else {
            qWarning().noquote() << "Invalid --margin value" << parser.value(marginOption)
                                  << "(expected" << DocumentView::kMinExtraMarginPercent << "-"
                                  << DocumentView::kMaxExtraMarginPercent << "); using config default";
            options.extraMarginPercent = fileConfig.extraMarginPercent;
        }
    } else {
        options.extraMarginPercent = fileConfig.extraMarginPercent;
    }

    if (parser.isSet(lineSpacingOption)) {
        bool lineOk = false;
        const double ls = parser.value(lineSpacingOption).toDouble(&lineOk);
        if (lineOk && ls >= DocumentView::kMinLineSpacing
            && ls <= DocumentView::kMaxLineSpacing) {
            options.lineSpacing = ls;
        } else {
            qWarning().noquote() << "Invalid --line-spacing value" << parser.value(lineSpacingOption)
                                  << "(expected" << DocumentView::kMinLineSpacing << "-"
                                  << DocumentView::kMaxLineSpacing << "); using config default";
            options.lineSpacing = fileConfig.lineSpacing;
        }
    } else {
        options.lineSpacing = fileConfig.lineSpacing;
    }

    options.colors = fileConfig.colors;
    if (parser.isSet(colorTextOption)) {
        options.colors.text = parseColorArg(parser.value(colorTextOption), "--color-text");
    }
    if (parser.isSet(colorBackgroundOption)) {
        options.colors.background =
            parseColorArg(parser.value(colorBackgroundOption), "--color-background");
    }
    if (parser.isSet(colorPlayingOption)) {
        options.colors.playing =
            parseColorArg(parser.value(colorPlayingOption), "--color-playing");
    }
    if (parser.isSet(colorPreparingOption)) {
        options.colors.preparing =
            parseColorArg(parser.value(colorPreparingOption), "--color-preparing");
    }
    if (parser.isSet(colorStoppedOption)) {
        options.colors.stopped =
            parseColorArg(parser.value(colorStoppedOption), "--color-stopped");
    }
    if (parser.isSet(colorHoverOption)) {
        options.colors.hover = parseColorArg(parser.value(colorHoverOption), "--color-hover");
    }

    options.presentationFontFamily = fileConfig.presentationFontFamily;
    options.presentationFontPointSize = fileConfig.presentationFontPointSize;
    options.presentationExtraMarginPercent = fileConfig.presentationExtraMarginPercent;
    options.presentationLineSpacing = fileConfig.presentationLineSpacing;
    options.presentationColors = fileConfig.presentationColors;

    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.initialFile = positional.first();
    }

    MainWindow window(options);
    window.show();

    return QApplication::exec();
}
