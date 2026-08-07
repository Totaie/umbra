#include "hostmanager.h"

#include <QDesktopServices>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QTcpSocket>
#include <QUrl>
#include <QVersionNumber>
#include <QtDebug>

#ifdef Q_OS_WIN32
#include <windows.h>
// GetFileVersionInfo and friends; app.pro links version.lib for them.
#include <winver.h>
#endif

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

// The host publishes its own releases; the client reads the same feed.
#define HOST_RELEASES_URL "https://api.github.com/repos/Totaie/umbra-host/releases?per_page=30"

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

QString HostManager::installedHostVersion()
{
    QString executable = findHostExecutable();
    if (executable.isEmpty()) {
        return QString();
    }

#ifdef Q_OS_WIN32
    // Read the version resource rather than an installer registry key: this is the
    // number the host stamps from its own version.txt, so it lines up with the
    // release tags being compared against.
    const std::wstring path = executable.toStdWString();

    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return QString();
    }

    QByteArray buffer(static_cast<int>(size), 0);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, buffer.data())) {
        return QString();
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoLength = 0;
    if (!VerQueryValueW(buffer.constData(), L"\\", reinterpret_cast<LPVOID*>(&info), &infoLength)
            || info == nullptr) {
        return QString();
    }

    return QStringLiteral("%1.%2.%3")
            .arg(HIWORD(info->dwFileVersionMS))
            .arg(LOWORD(info->dwFileVersionMS))
            .arg(HIWORD(info->dwFileVersionLS));
#else
    return QString();
#endif
}

bool HostManager::isHostStreaming()
{
    if (!isHostRunning()) {
        return false;
    }

    // 47984 is the RTSP port the host only listens on while a session is set up, and
    // 47998-48000 carry the stream itself. Asking the host over HTTP would need a
    // paired client certificate, so this reads the same thing from the outside.
    for (quint16 port : {47998, 47999, 48000}) {
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("127.0.0.1"), port);
        if (socket.waitForConnected(HOST_PROBE_TIMEOUT_MS)) {
            return true;
        }
    }

    return false;
}

void HostManager::checkHostForUpdate()
{
    // No host installed is not a problem to report. Declining it during setup is a
    // supported choice, and a client-only machine should never be told its host is
    // out of date.
    if (!isHostInstalled()) {
        return;
    }

    if (!m_Nam) {
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setStrictTransportSecurityEnabled(true);
        m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        connect(m_Nam, &QNetworkAccessManager::finished,
                this, &HostManager::handleHostReleasesReply);
    }

    QNetworkRequest request{QUrl(QStringLiteral(HOST_RELEASES_URL))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Umbra"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    m_Nam->get(request);
}

void HostManager::handleHostReleasesReply(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        emit hostCheckFailed(tr("Couldn't check Umbra Host for updates: %1")
                             .arg(reply->errorString()));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isArray()) {
        emit hostCheckFailed(tr("The update server sent a response Umbra couldn't read."));
        return;
    }

    // Highest version wins, not the first entry: releases published close together
    // can share a creation timestamp, and GitHub's order is then arbitrary.
    QVersionNumber best;
    QString bestTag;
    QString bestUrl;

    const QJsonArray releases = doc.array();
    for (const QJsonValue& value : releases) {
        if (!value.isObject()) {
            continue;
        }
        QJsonObject release = value.toObject();
        if (release["draft"].toBool(false)) {
            continue;
        }

        QString tag = release["tag_name"].toString();
        if (tag.isEmpty()) {
            continue;
        }

        int suffix = 0;
        QVersionNumber candidate = QVersionNumber::fromString(
            tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag, &suffix);
        if (candidate.isNull()) {
            continue;
        }

        if (best.isNull() || best < candidate) {
            best = candidate;
            bestTag = tag;
            bestUrl = release["html_url"].toString();
        }
    }

    if (best.isNull()) {
        emit hostCheckFailed(tr("No Umbra Host releases were found to compare against."));
        return;
    }

    QString installed = installedHostVersion();
    QVersionNumber current = QVersionNumber::fromString(installed);

    if (current.isNull()) {
        emit hostCheckFailed(tr("Couldn't read the installed Umbra Host version."));
        return;
    }

    if (current < best) {
        emit hostUpdateAvailable(best.toString(), bestUrl);
    }
    else {
        emit hostUpToDate(installed);
    }
}
