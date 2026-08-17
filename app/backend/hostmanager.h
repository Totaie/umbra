#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QNetworkAccessManager>
#include <QTimer>

class QNetworkReply;

// Controls the copy of Umbra Host installed on this machine.
//
// Umbra bundles the host, so the same install is usually both client and server.
// This exists so the UI can open the host's web interface and, when the host isn't
// actually running, start it first rather than dropping the user on a connection
// error page.
//
// Starting is delegated to `sunshine.exe --shortcut`, which is the host's own
// supported entry point: it starts the service (elevating itself if it has to),
// waits for the interface to be ready, and opens it. Doing any of that from here
// would be reimplementing it worse - in particular the host tolerates a missing
// config directory on this path, which it does not when launched plainly.
//
// Everything here is deliberately local-only: the executable path comes from the
// installer's registry key or a fixed Program Files location, never from anything
// received over the network.
class HostManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)

public:
    explicit HostManager(QObject* parent = nullptr);

    // True when Umbra Host appears to be installed on this machine.
    Q_INVOKABLE bool isHostInstalled();

    // True when the host's web interface is accepting connections right now.
    Q_INVOKABLE bool isHostRunning();

    // Opens the host's web interface, starting the host first if needed. Returns
    // immediately; watch webUiOpened/webUiFailed for the outcome.
    Q_INVOKABLE void openHostWebUi();

    // https://localhost:47990
    Q_INVOKABLE QString hostWebUiUrl() const;

    // The installed host's version, or an empty string when none is installed.
    Q_INVOKABLE QString installedHostVersion();

    // Asks GitHub whether a newer host exists. Reports through hostUpdateAvailable,
    // hostUpToDate or hostCheckFailed. Does nothing at all when no host is
    // installed - declining the host during setup is a supported choice, not a
    // problem to nag about.
    Q_INVOKABLE void checkHostForUpdate();

    // The installer from the newest host release, and the checksum GitHub published
    // for it. Both empty unless a check has found an update we can actually install:
    // a release that shipped no verifiable package is one to fetch by hand.
    Q_INVOKABLE QString hostUpdateAssetUrl() const
    {
        return m_HostUpdateAssetUrl;
    }

    Q_INVOKABLE QString hostUpdateAssetDigest() const
    {
        return m_HostUpdateAssetDigest;
    }

    Q_INVOKABLE bool canInstallHostUpdate() const
    {
        return !m_HostUpdateAssetUrl.isEmpty() && !m_HostUpdateAssetDigest.isEmpty();
    }

    // True while the host is serving a stream. Installing over it would stop the
    // service and drop the session, so updates wait.
    Q_INVOKABLE bool isHostStreaming();

    bool isBusy() const
    {
        return m_Busy;
    }

signals:
    void busyChanged();
    void webUiOpened();
    void webUiFailed(QString error);

    void hostUpdateAvailable(QString newVersion, QString url);
    void hostUpToDate(QString currentVersion, QString latestVersion);
    void hostCheckFailed(QString error);

private:
    // Absolute path to the host executable, or an empty string if not installed.
    QString findHostExecutable();

    // Runs `sunshine.exe --shortcut`. Returns false and reports why on failure.
    bool launchHostShortcut();

    // Polls until the web interface answers.
    void beginWaitingForHost();

    void openBrowser();
    void fail(const QString& error);
    void setBusy(bool busy);

    void handleHostReleasesReply(QNetworkReply* reply);

    // Set by the last successful check that found something newer.
    QString m_HostUpdateAssetUrl;
    QString m_HostUpdateAssetDigest;

    QString m_CachedExecutable;
    QNetworkAccessManager* m_Nam = nullptr;
    bool m_Busy = false;
    QTimer m_PollTimer;
    QElapsedTimer m_WaitTimer;
};
