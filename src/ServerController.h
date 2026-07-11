#pragma once

#include <QObject>
#include <QProcess>
#include <sys/types.h>

// Detects how Ollama is currently running — systemd user service, systemd
// system service, a loose/manually-started process, or not running at all —
// and dispatches Start/Stop through whichever mechanism actually owns it.
//
// This matters because "just terminate() our QProcess" only works if this
// app is the thing that started the server. If it's systemd-managed,
// killing the process directly just fights systemd's restart policy; if it
// was started by hand in a terminal, there's no QProcess handle to use at
// all after this app restarts. Detecting the real owner avoids both traps.
class ServerController : public QObject
{
    Q_OBJECT

public:
    enum class RunMode {
        Unknown,       // not yet detected
        SystemdUser,   // `systemctl --user` manages a unit named ollama.service
        SystemdSystem, // system-wide systemd manages it (needs polkit for start/stop)
        Loose,         // a bare `ollama serve` process, no service manager involved
        NotRunning
    };
    Q_ENUM(RunMode)

    explicit ServerController(QObject *parent = nullptr);

    // Synchronous — runs a couple of near-instant local subprocess checks
    // (systemctl is-active x2, pgrep). Safe to call on the GUI thread; each
    // check is capped with a short timeout so a wedged system can't hang the UI.
    RunMode detectMode();

    RunMode lastKnownMode() const { return m_lastMode; }

public slots:
    // Detects current mode, then starts/stops via the appropriate mechanism.
    // Emits actionSucceeded()/actionFailed() when the dispatched command
    // finishes — NOT when Ollama actually becomes reachable/unreachable,
    // since that's OllamaClient's job and can lag behind the command by a
    // second or two as the server binds its port.
    void start();
    void stop();

signals:
    void modeChanged(ServerController::RunMode mode);
    void actionSucceeded(QString description);
    void actionFailed(QString reason);

private:
    // Returns true if `systemctl <userFlag> is-active ollama.service` reports "active".
    bool isSystemdUnitActive(bool userScope);
    // Returns true if a unit file named ollama.service is known to systemd at all
    // (active or not) — used when deciding how to *start* it.
    bool isSystemdUnitKnown(bool userScope);

    // Finds PIDs of processes matching `ollama serve` via pgrep -f.
    QList<qint64> findLooseServerPids();

    // Runs a short-lived command synchronously, capped at timeoutMs.
    // Returns exit code, or -1 if it failed to start/timed out.
    int runSync(const QString &program, const QStringList &args, QString *stdOut = nullptr, int timeoutMs = 1500);

    RunMode m_lastMode = RunMode::Unknown;

    // Holds the child process handle only when *this app* spawned a loose
    // `ollama serve` — lets us prefer a clean terminate() over signaling by
    // PID when we're the known owner.
    QProcess m_ownedProcess;
    bool m_weOwnLooseProcess = false;
};
