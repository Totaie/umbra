#include "hostmanager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTcpSocket>
#include <QUrl>
#include <QtDebug>

// The host serves its web interface on the base port + 1. Both are the host's
// defaults; a host reconfigured onto another port has to be opened by hand.
#define HOST_WEB_UI_PORT 47990
#define HOST_WEB_UI_URL "https://localhost:47990"

#define HOST_SERVICE_NAME "UmbraService"

// How long to wait for a freshly started host to come up. The service has to
// initialise capture before it binds, which is not instant on a cold start.
#define HOST_STARTUP_TIMEOUT_MS 20000
#define HOST_POLL_INTERVAL_MS 250

// A refused connection to localhost comes back immediately, so this only actually
// elapses if something is listening but not completing the handshake.
#define HOST_PROBE_TIMEOUT_MS 500

HostManager::HostManager(QObject* parent)
    : QObject(parent)
{
    m_PollTimer.setInterval(HOST_POLL_INTERVAL_MS);
    connect(&m_PollTimer, &QTimer::timeout, this, [this]() {
        if (isHostRunning()) {
            m_PollTimer.stop();
            openBrowser();
        }
        else if (m_WaitTimer.elapsed() >= HOST_STARTUP_TIMEOUT_MS) {
            m_PollTimer.stop();
            fail(tr("Umbra Host was started but its web interface has not come up yet. "
                    "Try again in a moment."));
        }
    });
}

QString HostManager::hostWebUiUrl() const
{
    return QStringLiteral(HOST_WEB_UI_URL);
}

void HostManager::setBusy(bool busy)
{
    if (m_Busy != busy) {
        m_Busy = busy;
        emit busyChanged();
    }
}

void HostManager::fail(const QString& error)
{
    setBusy(false);
    emit webUiFailed(error);
}

QString HostManager::findHostExecutable()
{
    if (!m_CachedExecutable.isEmpty() && QFileInfo::exists(m_CachedExecutable)) {
        return m_CachedExecutable;
    }
    m_CachedExecutable.clear();

#ifdef Q_OS_WIN32
    QStringList candidates;

    // The installer records where it put things. Prefer that over guessing, since
    // the user can choose a non-default location. The Vibepollo key is checked too
    // so an install made before the Umbra rename is still found.
    for (const char* key : {"HKEY_LOCAL_MACHINE\\SOFTWARE\\Umbra Host",
                            "HKEY_LOCAL_MACHINE\\SOFTWARE\\Vibepollo"}) {
        QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
        QString installPath = reg.value(QStringLiteral("InstallPath")).toString();
        if (installPath.isEmpty()) {
            installPath = reg.value(QStringLiteral("Default")).toString();
        }
        if (!installPath.isEmpty()) {
            candidates.append(QDir(installPath).filePath(QStringLiteral("sunshine.exe")));
        }
    }

    for (const char* env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
        QString programFiles = qEnvironmentVariable(env);
        if (programFiles.isEmpty()) {
            continue;
        }
        candidates.append(QDir(programFiles).filePath(QStringLiteral("Umbra Host/sunshine.exe")));
    }

    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            m_CachedExecutable = QDir::toNativeSeparators(candidate);
            return m_CachedExecutable;
        }
    }
#endif

    return QString();
}

bool HostManager::isHostInstalled()
{
    return !findHostExecutable().isEmpty();
}

bool HostManager::isHostRunning()
{
    // "Running" means the web interface answers, which is the thing we're about to
    // open. Checking the service state instead would call a starting-but-not-yet-
    // listening host running and send the browser to a connection error.
    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), HOST_WEB_UI_PORT);
    return socket.waitForConnected(HOST_PROBE_TIMEOUT_MS);
}

void HostManager::openHostWebUi()
{
    if (m_Busy) {
        return;
    }

    if (isHostRunning()) {
        openBrowser();
        return;
    }

    if (!isHostInstalled()) {
        fail(tr("Umbra Host does not appear to be installed on this PC."));
        return;
    }

    setBusy(true);
    queryService();
}

void HostManager::queryService()
{
#ifdef Q_OS_WIN32
    auto* process = new QProcess(this);
    m_Process = process;

    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
        process->disconnect(this);
        process->deleteLater();

        if (exitCode == 0) {
            startService();
        }
        else {
            // No service registered, so this is a portable or user-mode install.
            if (launchExecutable()) {
                beginWaitingForHost();
            }
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        // A failure to start also emits finished(), so drop that before it runs the
        // fallback a second time.
        process->disconnect(this);
        process->deleteLater();
        if (launchExecutable()) {
            beginWaitingForHost();
        }
    });

    process->start(QStringLiteral("sc.exe"),
                   {QStringLiteral("query"), QStringLiteral(HOST_SERVICE_NAME)});
#else
    fail(tr("Starting Umbra Host from here is only supported on Windows."));
#endif
}

void HostManager::startService()
{
#ifdef Q_OS_WIN32
    // Prefer the service. It runs with the privileges the host needs for input
    // injection and display changes, which a process we spawn would not inherit.
    auto* process = new QProcess(this);
    m_Process = process;

    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus) {
        process->disconnect(this);
        process->deleteLater();

        // Exit code 2 is "the service is already running", which is a success here.
        if (exitCode == 0 || exitCode == 2) {
            beginWaitingForHost();
            return;
        }

        // Starting a service needs elevation. Fall back to the executable, which
        // asks for it via its own manifest.
        qInfo() << "Could not start" << HOST_SERVICE_NAME << "- exit code" << exitCode
                << "- falling back to launching the host directly";
        if (launchExecutable()) {
            beginWaitingForHost();
        }
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        process->disconnect(this);
        process->deleteLater();
        if (launchExecutable()) {
            beginWaitingForHost();
        }
    });

    process->start(QStringLiteral("net.exe"),
                   {QStringLiteral("start"), QStringLiteral(HOST_SERVICE_NAME)});
#endif
}

bool HostManager::launchExecutable()
{
    QString executable = findHostExecutable();
    if (executable.isEmpty()) {
        fail(tr("Umbra Host does not appear to be installed on this PC."));
        return false;
    }

    // Detached, so the host outlives Umbra. The working directory is the install
    // folder because the host loads its web assets relative to itself.
    if (!QProcess::startDetached(executable, {}, QFileInfo(executable).absolutePath())) {
        fail(tr("Umbra Host could not be started. Try starting it from the Start menu."));
        return false;
    }

    return true;
}

void HostManager::beginWaitingForHost()
{
    m_WaitTimer.start();
    m_PollTimer.start();
}

void HostManager::openBrowser()
{
    setBusy(false);

    if (!QDesktopServices::openUrl(QUrl(QStringLiteral(HOST_WEB_UI_URL)))) {
        emit webUiFailed(tr("Could not open a browser. The host's web interface is at %1.")
                         .arg(QStringLiteral(HOST_WEB_UI_URL)));
        return;
    }

    emit webUiOpened();
}
