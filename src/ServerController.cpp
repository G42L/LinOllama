#include "ServerController.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QDebug>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <csignal>

ServerController::ServerController(QObject *parent) : QObject(parent) {}

int ServerController::runSync(const QString &program, const QStringList &args, QString *stdOut, int timeoutMs)
{
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(timeoutMs)) {
        // Program not found on PATH, or couldn't fork — either way, treat as
        // "this mechanism isn't available" rather than a hard error.
        return -1;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(200);
        return -1;
    }
    if (stdOut)
        *stdOut = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    return proc.exitCode();
}

bool ServerController::isSystemdUnitActive(bool userScope)
{
    QStringList args;
    if (userScope)
        args << "--user";
    args << "is-active" << "ollama.service";

    QString out;
    const int code = runSync("systemctl", args, &out);
    // `is-active` exits 0 and prints "active" only when genuinely running;
    // any other state (inactive/failed/unknown/not-found) is a non-zero exit.
    return code == 0 && out == "active";
}

bool ServerController::isSystemdUnitKnown(bool userScope)
{
    QStringList args;
    if (userScope)
        args << "--user";
    args << "list-unit-files" << "ollama.service";

    QString out;
    const int code = runSync("systemctl", args, &out);
    return code == 0 && out.contains("ollama.service");
}

QList<qint64> ServerController::findLooseServerPids()
{
    QString out;
    const int code = runSync("pgrep", {"-f", "ollama serve"}, &out);
    QList<qint64> pids;
    if (code != 0 || out.isEmpty())
        return pids; // pgrep exits 1 when nothing matches — that's a normal "not found", not an error
    for (const QString &line : out.split('\n', Qt::SkipEmptyParts)) {
        bool ok = false;
        const qint64 pid = line.toLongLong(&ok);
        if (ok)
            pids << pid;
    }
    return pids;
}

QMap<QString, QString> ServerController::configuredEnvironmentOverrides() const
{
    QSettings settings;
    QMap<QString, QString> env;

    const QString modelsPath = settings.value("ollamaServer/modelsPath").toString().trimmed();
    if (!modelsPath.isEmpty())
        env.insert("OLLAMA_MODELS", modelsPath);

    const QString keepAlive = settings.value("ollamaServer/keepAlive").toString().trimmed();
    if (!keepAlive.isEmpty())
        env.insert("OLLAMA_KEEP_ALIVE", keepAlive);

    if (settings.value("ollamaServer/flashAttention", false).toBool())
        env.insert("OLLAMA_FLASH_ATTENTION", "1");

    // 0 is the Settings spin box's "Auto (default)" special value — omit
    // the variable entirely rather than sending OLLAMA_NUM_PARALLEL=0,
    // which would mean something different (Ollama wouldn't parse that as
    // "unset").
    const int numParallel = settings.value("ollamaServer/numParallel", 0).toInt();
    if (numParallel > 0)
        env.insert("OLLAMA_NUM_PARALLEL", QString::number(numParallel));

    return env;
}

void ServerController::applyUserSystemdEnvironmentOverride()
{
    if (!isSystemdUnitKnown(/*userScope=*/true))
        return;

    const QString dropInDir = QDir::homePath() + "/.config/systemd/user/ollama.service.d";
    const QString overridePath = dropInDir + "/override.conf";
    const QMap<QString, QString> env = configuredEnvironmentOverrides();

    if (env.isEmpty()) {
        // Nothing configured (any more) — remove a stale override from an
        // earlier session instead of leaving it silently in effect.
        if (QFile::exists(overridePath))
            QFile::remove(overridePath);
    } else {
        QDir().mkpath(dropInDir);
        QFile file(overridePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream out(&file);
            out << "[Service]\n";
            for (auto it = env.constBegin(); it != env.constEnd(); ++it)
                out << "Environment=\"" << it.key() << "=" << it.value() << "\"\n";
        } else {
            qWarning() << "ServerController: couldn't write" << overridePath;
            return;
        }
    }

    // Cheap and idempotent — safe to run even when nothing actually changed.
    runSync("systemctl", {"--user", "daemon-reload"}, nullptr, 5000);
}

ServerController::RunMode ServerController::detectMode()
{
    RunMode mode = RunMode::NotRunning;

    if (isSystemdUnitActive(/*userScope=*/true)) {
        mode = RunMode::SystemdUser;
    } else if (isSystemdUnitActive(/*userScope=*/false)) {
        mode = RunMode::SystemdSystem;
    } else if (!findLooseServerPids().isEmpty()) {
        mode = RunMode::Loose;
    }

    if (mode != m_lastMode) {
        m_lastMode = mode;
        emit modeChanged(mode);
    }
    return mode;
}

void ServerController::start()
{
    const RunMode current = detectMode();
    if (current != RunMode::NotRunning) {
        emit actionFailed("Ollama already appears to be running.");
        return;
    }

    // Refreshes (or clears) the systemd user drop-in from whatever's
    // currently configured in Settings before actually starting anything —
    // a no-op if no user-scope unit exists, so harmless to call unconditionally.
    applyUserSystemdEnvironmentOverride();

    // Prefer whichever systemd scope already has a unit defined, since that
    // respects however the user (or a package installer) set things up.
    if (isSystemdUnitKnown(/*userScope=*/true)) {
        QString out;
        const int code = runSync("systemctl", {"--user", "start", "ollama.service"}, &out, 5000);
        if (code == 0) {
            emit actionSucceeded("Started via systemd (user service).");
        } else {
            emit actionFailed("systemctl --user start failed — check `systemctl --user status ollama`.");
        }
        detectMode();
        return;
    }

    if (isSystemdUnitKnown(/*userScope=*/false)) {
        // System-wide units need elevated privileges. pkexec pops a native
        // polkit auth dialog — no need to shell out to sudo, which would
        // just fail non-interactively from a GUI app anyway.
        QString out;
        const int code = runSync("pkexec", {"systemctl", "start", "ollama.service"}, &out, 15000);
        if (code == 0) {
            emit actionSucceeded("Started via systemd (system service).");
        } else {
            emit actionFailed("systemctl start failed or the authentication prompt was cancelled.");
        }
        detectMode();
        return;
    }

    // No known systemd unit either way — fall back to spawning it directly.
    // We keep the QProcess handle so a same-session Stop can use a clean
    // terminate() instead of hunting for the PID again.
    m_ownedProcess.setProgram("ollama");
    m_ownedProcess.setArguments({"serve"});

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QMap<QString, QString> overrides = configuredEnvironmentOverrides();
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        env.insert(it.key(), it.value());
    m_ownedProcess.setProcessEnvironment(env);

    m_ownedProcess.start();
    if (!m_ownedProcess.waitForStarted(3000)) {
        emit actionFailed("Couldn't start `ollama serve` — is Ollama installed and on PATH?");
        return;
    }
    m_weOwnLooseProcess = true;
    emit actionSucceeded("Started as a plain process (no systemd unit found for ollama.service).");
    detectMode();
}

void ServerController::stop()
{
    const RunMode current = detectMode();

    switch (current) {
    case RunMode::SystemdUser: {
        const int code = runSync("systemctl", {"--user", "stop", "ollama.service"}, nullptr, 5000);
        if (code == 0)
            emit actionSucceeded("Stopped systemd user service.");
        else
            emit actionFailed("systemctl --user stop failed.");
        break;
    }
    case RunMode::SystemdSystem: {
        const int code = runSync("pkexec", {"systemctl", "stop", "ollama.service"}, nullptr, 15000);
        if (code == 0)
            emit actionSucceeded("Stopped systemd system service.");
        else
            emit actionFailed("systemctl stop failed or the authentication prompt was cancelled.");
        break;
    }
    case RunMode::Loose: {
        if (m_weOwnLooseProcess && m_ownedProcess.state() == QProcess::Running) {
            m_ownedProcess.terminate();
            if (!m_ownedProcess.waitForFinished(3000))
                m_ownedProcess.kill();
            emit actionSucceeded("Stopped (process we started).");
        } else {
            // We didn't start it — most likely launched by hand in a
            // terminal, possibly before this app was running at all. Signal
            // every matching PID directly with SIGTERM rather than SIGKILL,
            // so Ollama gets a chance to shut down cleanly.
            const QList<qint64> pids = findLooseServerPids();
            if (pids.isEmpty()) {
                emit actionFailed("No running `ollama serve` process found.");
                break;
            }
            bool allSucceeded = true;
            for (qint64 pid : pids) {
                if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0)
                    allSucceeded = false;
            }
            if (allSucceeded)
                emit actionSucceeded(QString("Sent SIGTERM to %1 process(es).").arg(pids.size()));
            else
                emit actionFailed("Found the process but didn't have permission to stop it.");
        }
        break;
    }
    case RunMode::NotRunning:
        emit actionFailed("Ollama isn't running.");
        break;
    case RunMode::Unknown:
        emit actionFailed("Couldn't determine how Ollama is running.");
        break;
    }

    detectMode();
}
