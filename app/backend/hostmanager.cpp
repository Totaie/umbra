#include "hostmanager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTcpSocket>
#include <QUrl>
#include <QtDebug>

// The host serves its web interface on the base port + 1. Both are the host's
// defaults; a host reconfigured onto another port has to be opened by hand.
#define HOST_WEB_UI_PORT 47990
#define HOST_WEB_UI_URL "https://localhost:47990"

// How long to wait for a freshly started host to come up. Starting it may involve
// a UAC prompt the user has to answer, and the service then has to initialise
// capture before it binds, so this is generous on purpose.
#define HOST_STARTUP_TIMEOUT_MS 60000
#define HOST_POLL_INTERVAL_MS 500

// A refused connection to localhost comes back immediately, so this only actually
// elapses if something is listening but not completing the handshake.
#define HOST_PROBE_TIMEOUT_MS 500

HostManager::HostManager(QObject* parent)
    : QObject(parent)
{
    m_PollTimer.setInterval(HOST_POLL_INTERVAL_MS);
    connect(&m_PollTimer, &QTimer::timeout, this, [this]() {
        if (isHostRunning()) {
            // The host opens its own interface once it's up, so there's nothing
            // left to do but stop waiting.
            m_PollTimer.stop();
            setBusy(false);
            emit webUiOpened();
        }
        else if (m_WaitTimer.elapsed() >= HOST_STARTUP_TIMEOUT_MS) {
            m_PollTimer.stop();
            fail(tr("Umbra Host didn't finish starting. If it asked for administrator "
                    "permission and you declined, try again and accept. Otherwise check "
                    "its log at %1\\config\\sunshine.log.")
                 .arg(QFileInfo(findHostExecutable()).absolutePath()));
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

    // Already up, so this is just a link. Opening it ourselves avoids waking the
    // host process for something it doesn't need to be involved in.
    if (isHostRunning()) {
        openBrowser();
        return;
    }

    if (!isHostInstalled()) {
        fail(tr("Umbra Host does not appear to be installed on this PC."));
        return;
    }

    setBusy(true);

    if (launchHostShortcut()) {
        beginWaitingForHost();
    }
}

bool HostManager::launchHostShortcut()
{
    QString executable = findHostExecutable();
    if (executable.isEmpty()) {
        fail(tr("Umbra Host does not appear to be installed on this PC."));
        return false;
    }

    // --shortcut is the host's supported entry point and does the whole job: it
    // starts the service, re-launching itself elevated if that needs administrator
    // rights, waits for the interface, and opens it. Launched unelevated on
    // purpose - the host asks for elevation only when it actually needs it, so a
    // host that's already registered and running never prompts at all.
    //
    // Detached so the host outlives Umbra. The working directory is the install
    // folder because the host loads its assets relative to itself.
    if (!QProcess::startDetached(executable, {QStringLiteral("--shortcut")},
                                 QFileInfo(executable).absolutePath())) {
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
