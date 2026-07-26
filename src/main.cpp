#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>
#include <QSettings>

#include "ui/MainWindow.h"

namespace {

// Config file lives at (Linux) ~/.config/text2sprech/text2sprech.ini -- see
// README for the full key list. CLI flags always take precedence over these:
// a config value just changes the *default* used when a flag isn't given
// (implemented via QCommandLineOption's defaultValue, so parser.value()
// naturally returns the config value when the user didn't pass the flag).
//
// Group name is "startup" (not "general"): QSettings IniFormat treats a
// group called General specially and writes it as [%General], which on
// repeated saves can leave duplicate sections in the .ini and make later
// values unreadable. Old installs may still have [general] / [%General];
// loadFileConfig() falls back to that group when "startup" is empty.
struct FileConfig {
    QString voice;
    double speed = 1.0;
    QString fontFamily;
    int fontPointSize = -1; // <= 0 means "leave the widget's default size alone"
};

void readConfigGroup(QSettings &settings, FileConfig *config)
{
    config->voice = settings.value(QStringLiteral("voice")).toString();
    config->fontFamily = settings.value(QStringLiteral("fontFamily")).toString();

    bool speedOk = false;
    double speed = settings.value(QStringLiteral("speed"), 1.0).toDouble(&speedOk);
    if (speedOk && speed >= 0.5 && speed <= 3.0) {
        config->speed = speed;
    } else if (settings.contains(QStringLiteral("speed"))) {
        qWarning().noquote() << "Config: invalid speed value" << settings.value(QStringLiteral("speed"))
                              << "(expected 0.5-3.0); using 1.0";
    }

    bool sizeOk = false;
    int pointSize = settings.value(QStringLiteral("fontPointSize"), -1).toInt(&sizeOk);
    if (sizeOk && pointSize >= 6 && pointSize <= 72) {
        config->fontPointSize = pointSize;
    } else if (settings.contains(QStringLiteral("fontPointSize"))) {
        qWarning().noquote() << "Config: invalid fontPointSize value"
                              << settings.value(QStringLiteral("fontPointSize"))
                              << "(expected 6-72); using the widget default";
    }
}

bool groupHasAnyKeys(QSettings &settings)
{
    return settings.contains(QStringLiteral("voice"))
        || settings.contains(QStringLiteral("speed"))
        || settings.contains(QStringLiteral("fontFamily"))
        || settings.contains(QStringLiteral("fontPointSize"));
}

FileConfig loadFileConfig()
{
    FileConfig config;
    QSettings settings;

    settings.beginGroup(QStringLiteral("startup"));
    const bool haveStartup = groupHasAnyKeys(settings);
    if (haveStartup) {
        readConfigGroup(settings, &config);
    }
    settings.endGroup();

    if (!haveStartup) {
        // Migrate from the old "general" / [%General] group if present.
        settings.beginGroup(QStringLiteral("general"));
        if (groupHasAnyKeys(settings)) {
            readConfigGroup(settings, &config);
        }
        settings.endGroup();
    }

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
    parser.addPositionalArgument("file", "Optional text/HTML file to open on startup.", "[file]");

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

    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.initialFile = positional.first();
    }

    MainWindow window(options);
    window.show();

    return QApplication::exec();
}
