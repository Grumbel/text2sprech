#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>

#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("reader");
    QApplication::setOrganizationName("reader");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Local TTS reader with live sentence highlighting");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption voiceOption(
        QStringList() << "voice",
        "Voice to select on startup (must match a voice the piper-server reports). "
        "Falls back to the server's default and prints a warning if not found.",
        "name");
    QCommandLineOption speedOption(
        QStringList() << "speed",
        "Initial playback speed, 0.5-3.0.",
        "factor", "1.0");
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
    parser.addOption(voiceOption);
    parser.addOption(speedOption);
    parser.addOption(autoplayOption);
    parser.addOption(piperSocketOption);
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

    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        options.initialFile = positional.first();
    }

    MainWindow window(options);
    window.show();

    return QApplication::exec();
}
