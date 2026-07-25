#include "PiperServerManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>

PiperServerManager::PiperServerManager(QObject *parent) : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        emit serverOutput(QString::fromUtf8(m_process.readAllStandardError()));
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        emit serverOutput(QString::fromUtf8(m_process.readAllStandardOutput()));
    });
}

PiperServerManager::~PiperServerManager()
{
    stop();
}

QString PiperServerManager::generateUniqueSocketPath()
{
    // PID alone isn't quite enough -- a stale socket file left behind by a
    // crashed previous run (or a reused PID after a reboot) could collide.
    // A UUID guarantees no two instances, even started in the same
    // millisecond, ever pick the same path.
    QString name = QStringLiteral("text2sprech-piper-%1-%2.sock")
        .arg(QCoreApplication::applicationPid())
        .arg(QUuid::createUuid().toString(QUuid::Id128));
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(name);
}

QString PiperServerManager::findServerCommand(QStringList *argsOut)
{
    // 1. Explicit override -- e.g. set by a packaging wrapper, or by a user
    //    who has their own preferred piper-server binary.
    QString envBin = QString::fromLocal8Bit(qgetenv("TEXT2SPRECH_PIPER_SERVER_BIN"));
    if (!envBin.isEmpty() && QFileInfo::exists(envBin)) {
        return envBin;
    }

    // 2. An installed `piper-server` launcher on PATH -- this is what the
    //    flake's piper-server package provides, and flake.nix wraps the
    //    `text2sprech` binary's PATH to include it (see qtWrapperArgs there).
    QString onPath = QStandardPaths::findExecutable(QStringLiteral("piper-server"));
    if (!onPath.isEmpty()) {
        return onPath;
    }

    // 3. Dev-tree fallback: run the script directly with python3, using the
    //    source location baked in at configure time (see CMakeLists.txt).
    //    Won't exist in an installed/packaged build -- options 1/2 are
    //    meant to cover that case; this is just so `cmake --build build &&
    //    ./build/text2sprech` works out of the box while developing.
#ifdef PIPER_SERVER_SCRIPT_PATH
    QString script = QStringLiteral(PIPER_SERVER_SCRIPT_PATH);
    if (QFileInfo::exists(script)) {
        QString python3 = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (!python3.isEmpty()) {
            *argsOut << script;
            return python3;
        }
    }
#endif
    return QString();
}

QString PiperServerManager::startWithUniqueSocket()
{
    QString socketPath = generateUniqueSocketPath();
    QStringList args;
    QString command = findServerCommand(&args);
    if (command.isEmpty()) {
        emit failedToStart(tr(
            "No piper-server executable or script found. Install the piper-server "
            "package (see flake.nix), set TEXT2SPRECH_PIPER_SERVER_BIN, or start one "
            "yourself and pass --piper-socket."));
        return QString();
    }

    args << "--socket" << socketPath;
    m_process.setProgram(command);
    m_process.setArguments(args);
    m_process.start();
    if (!m_process.waitForStarted(3000)) {
        emit failedToStart(tr("piper-server failed to start (%1): %2")
                                .arg(command, m_process.errorString()));
        return QString();
    }
    m_started = true;
    return socketPath;
}

void PiperServerManager::stop()
{
    if (!m_started || m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_process.terminate(); // SIGTERM on Unix; server.py handles this for clean shutdown
    if (!m_process.waitForFinished(2000)) {
        m_process.kill(); // SIGKILL if it didn't exit in time
        m_process.waitForFinished(1000);
    }
    m_started = false;
}
