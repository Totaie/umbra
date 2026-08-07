#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

// Controls the copy of Umbra Host installed on this machine.
//
// Umbra bundles the host, so the same install is usually both client and server.
// This exists so the UI can open the host's web interface and, when the host isn't
// actually running, start it first rather than dropping the user on a connection
// error page.
//
// Starting a host is slow enough to matter (the service has to initialise capture
// before it binds), so the whole flow is asynchronous and reports back by signal.
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

    // Starts the host if needed, then opens its web interface in the default
    // browser. Returns immediately; watch webUiOpened/webUiFailed for the outcome.
    Q_INVOKABLE void openHostWebUi();

    // https://localhost:47990
    Q_INVOKABLE QString hostWebUiUrl() const;

    bool isBusy() const
    {
        return m_Busy;
    }

signals:
    void busyChanged();
    void webUiOpened();
    void webUiFailed(QString error);

private:
    // Absolute path to the host executable, or an empty string if not installed.
    QString findHostExecutable();

    // Asks the service control manager whether our service exists.
    void queryService();

    // Runs `net start`, which needs elevation and may therefore fail.
    void startService();

    // Launches the host executable directly, for installs with no service.
    bool launchExecutable();

    // Polls until the web interface answers, then opens it.
    void beginWaitingForHost();

    void openBrowser();
    void fail(const QString& error);
    void setBusy(bool busy);

    QString m_CachedExecutable;
    bool m_Busy = false;
    QPointer<QProcess> m_Process;
    QTimer m_PollTimer;
    QElapsedTimer m_WaitTimer;
};
