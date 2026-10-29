#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>

class NvComputer;
class QNetworkAccessManager;
class QTimer;

// Knows the newest Umbra Host release, and asks a paired host that is behind it to
// install it (umbra-host's POST /actions/host/update). The host picks the release
// and verifies the installer itself; all this sends is "update yourself", over the
// paired HTTPS connection with both certificates pinned.
class RemoteHostUpdater : public QObject
{
    Q_OBJECT

public:
    // One for the whole app, so the release is looked up once and an update in flight
    // is still known when the PC list is rebuilt. Created on first use (GUI thread).
    static RemoteHostUpdater* get();

    // The newest stable Umbra Host release with an installer, or empty until known.
    QString latestVersion() const { return m_LatestVersion; }

    // The version this host should be updated to, or empty if it is current, is not
    // Umbra Host, or the newest release is not known yet.
    QString updateFor(const NvComputer* computer) const;

    // Whether an update this asked for is still on its way.
    bool isUpdating(const NvComputer* computer) const;

    void requestUpdate(NvComputer* computer);

    // Called when a computer's state changes: says when an update has landed.
    void noteComputer(NvComputer* computer);

    void refreshLatest();

    // -1, 0 or 1, comparing dotted version numbers ("v" prefix and suffixes ignored).
    static int compareVersions(const QString& a, const QString& b);

signals:
    void latestVersionChanged();
    void updatingChanged(QString uuid);
    // A sentence for the user: started, already current, refused, failed.
    void message(QString text);

private:
    explicit RemoteHostUpdater(QObject* parent = nullptr);

    QNetworkAccessManager* m_Nam;
    QTimer* m_RefreshTimer;
    QString m_LatestVersion;
    // uuid -> (target version, when asked); cleared once the host reports it.
    QHash<QString, QPair<QString, QDateTime>> m_Updating;
};
