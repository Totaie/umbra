#include "hostmanager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTcpSocket>
#include <QUrl>
#include <QtDebug>

#ifdef Q_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#endif

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

            // Distinguish the two ways this ends up here, because the fixes are
            // completely different. No service at all means the install didn't
            // finish its job and reinstalling is the answer; a registered service
            // that never binds is a host-side failure worth reading the log for.
            if (m_ServiceExists) {
                fail(tr("The Umbra Host service is registered but nothing is answering on "
                        "port %1. Check the host's log at "
                        "%2\\config\\sunshine.log.")
                     .arg(HOST_WEB_UI_PORT)
                     .arg(QFileInfo(findHostExecutable()).absolutePath()));
            }
            else {
                fail(tr("Umbra Host is installed but not registered as a service, so it "
                        "can't start on its own. Reinstall Umbra Host to register it."));
            }
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

        m_ServiceExists = (exitCode == 0);

        // With a service, start that; without one, run the host directly. Either
        // way a single UAC prompt follows, and then we wait for it to bind.
        if (m_ServiceExists ? startService() : launchExecutable()) {
            beginWaitingForHost();
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

bool HostManager::runElevated(const QString& program, const QString& arguments,
                              const QString& workingDirectory)
{
#ifdef Q_OS_WIN32
    // QProcess cannot elevate, so this goes through ShellExecuteEx with the runas
    // verb. The UAC prompt is the honest thing to show: the user asked to start
    // something that captures their screen and injects input.
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<LPCWSTR>(program.utf16());
    info.lpParameters = arguments.isEmpty() ? nullptr
                                            : reinterpret_cast<LPCWSTR>(arguments.utf16());
    info.lpDirectory = workingDirectory.isEmpty()
                           ? nullptr
                           : reinterpret_cast<LPCWSTR>(workingDirectory.utf16());
    info.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&info)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            // The user dismissed the prompt. That's a decision, not a fault.
            fail(tr("Umbra Host needs administrator permission to start."));
        }
        else {
            fail(tr("Umbra Host could not be started (Windows error %1).").arg(error));
        }
        return false;
    }

    if (info.hProcess != nullptr) {
        CloseHandle(info.hProcess);
    }

    return true;
#else
    Q_UNUSED(arguments);
    if (!QProcess::startDetached(program, {}, workingDirectory)) {
        fail(tr("Umbra Host could not be started."));
        return false;
    }
    return true;
#endif
}

bool HostManager::startService()
{
    // net.exe rather than sc.exe because it waits for the service to finish
    // starting instead of returning the moment the request is queued.
    return runElevated(QStringLiteral("net.exe"),
                       QStringLiteral("start ") + QStringLiteral(HOST_SERVICE_NAME),
                       QString());
}

bool HostManager::launchExecutable()
{
    QString executable = findHostExecutable();
    if (executable.isEmpty()) {
        fail(tr("Umbra Host does not appear to be installed on this PC."));
        return false;
    }

    // --shortcut is what the Start menu entry passes: it puts the host in the tray
    // rather than leaving a console window behind. The working directory is the
    // install folder because the host loads its web assets relative to itself.
    return runElevated(executable, QStringLiteral("--shortcut"),
                       QFileInfo(executable).absolutePath());
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
