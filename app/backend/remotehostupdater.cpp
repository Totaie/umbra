#include "remotehostupdater.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "umbraversion.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QReadLocker>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>

namespace {
// How often the newest host release is looked up again while Umbra stays open.
constexpr int REFRESH_INTERVAL_MS = 6 * 60 * 60 * 1000;
// A host that has not come back on the new version by then is not going to.
constexpr qint64 UPDATE_TIMEOUT_SECS = 10 * 60;

QList<int> versionParts(QString version)
{
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.remove(0, 1);
    }
    const int cut = version.indexOf(QRegularExpression(QStringLiteral("[^0-9.]")));
    if (cut >= 0) {
        version.truncate(cut);
    }
    QList<int> parts;
    for (const QString& part : version.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        parts.append(part.toInt());
    }
    return parts;
}
}

RemoteHostUpdater* RemoteHostUpdater::get()
{
    static RemoteHostUpdater* instance = nullptr;
    if (instance == nullptr) {
        instance = new RemoteHostUpdater(qApp);
        instance->refreshLatest();
    }
    return instance;
}

RemoteHostUpdater::RemoteHostUpdater(QObject* parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_RefreshTimer(new QTimer(this))
{
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_RefreshTimer, &QTimer::timeout, this, &RemoteHostUpdater::refreshLatest);
    m_RefreshTimer->start(REFRESH_INTERVAL_MS);
}

int RemoteHostUpdater::compareVersions(const QString& a, const QString& b)
{
    const QList<int> x = versionParts(a);
    const QList<int> y = versionParts(b);
    for (int i = 0; i < qMax(x.size(), y.size()); i++) {
        const int p = i < x.size() ? x[i] : 0;
        const int q = i < y.size() ? y[i] : 0;
        if (p != q) {
            return p < q ? -1 : 1;
        }
    }
    return 0;
}

void RemoteHostUpdater::refreshLatest()
{
    // The host is released with Umbra, from this repository. Hosts from before it moved
    // here still read Totaie/umbra-host, where every release also gets a copy of the
    // host installer (scripts\publish-release.bat), so the newest of the two is what a
    // host can be updated to, whichever it reads.
    for (const QString& repo : {QStringLiteral("Totaie/umbra"), QStringLiteral("Totaie/umbra-host")}) {
        refreshLatestFrom(repo);
    }
}

void RemoteHostUpdater::refreshLatestFrom(const QString& repo)
{
    // Same rule as the host itself uses (host/src/self_update.cpp): the newest release
    // that is not a draft or prerelease and carries an installer with a checksum.
    // Anything else the host would refuse to install.
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repo)));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Umbra/%1").arg(VERSION_STR));
    request.setRawHeader("Accept", "application/vnd.github+json");

    QNetworkReply* reply = m_Nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, repo]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Could not look up the newest Umbra Host release in" << repo << ":" << reply->errorString();
            return;
        }

        QString best;
        for (const QJsonValue& value : QJsonDocument::fromJson(reply->readAll()).array()) {
            const QJsonObject release = value.toObject();
            if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()) {
                continue;
            }
            const QString tag = release.value(QStringLiteral("tag_name")).toString();
            bool hasInstaller = false;
            for (const QJsonValue& asset : release.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject a = asset.toObject();
                const QString name = a.value(QStringLiteral("name")).toString();
                if (name.startsWith(QStringLiteral("UmbraHostSetup-x64-")) && name.endsWith(QStringLiteral(".exe")) &&
                        a.value(QStringLiteral("digest")).toString().startsWith(QStringLiteral("sha256:"))) {
                    hasInstaller = true;
                }
            }
            if (hasInstaller && (best.isEmpty() || compareVersions(best, tag) < 0)) {
                best = tag;
            }
        }

        if (!best.isEmpty() && (m_LatestVersion.isEmpty() || compareVersions(m_LatestVersion, best) < 0)) {
            m_LatestVersion = best;
            emit latestVersionChanged();
        }
    });
}

QString RemoteHostUpdater::updateFor(const NvComputer* computer) const
{
    if (computer == nullptr || m_LatestVersion.isEmpty()) {
        return QString();
    }
    // Only Umbra Host says its version; anything else (stock Sunshine, Apollo) cannot
    // be asked to update itself.
    if (computer->hostVersion.isEmpty() || computer->pairState != NvComputer::PS_PAIRED ||
            computer->state != NvComputer::CS_ONLINE) {
        return QString();
    }
    return compareVersions(computer->hostVersion, m_LatestVersion) < 0 ? m_LatestVersion : QString();
}

bool RemoteHostUpdater::isUpdating(const NvComputer* computer) const
{
    if (computer == nullptr) {
        return false;
    }
    const auto it = m_Updating.constFind(computer->uuid);
    if (it == m_Updating.constEnd()) {
        return false;
    }
    // Finished once the host reports the new version, given up on after a while.
    return compareVersions(computer->hostVersion, it->first) < 0 &&
            it->second.secsTo(QDateTime::currentDateTimeUtc()) < UPDATE_TIMEOUT_SECS;
}

void RemoteHostUpdater::requestUpdate(NvComputer* computer)
{
    QString address;
    quint16 port = 0;
    QSslCertificate serverCert;
    QString uuid;
    QString name;
    QString target;
    {
        QReadLocker lock(&computer->lock);
        address = computer->activeAddress.address();
        port = computer->activeHttpsPort;
        serverCert = computer->serverCert;
        uuid = computer->uuid;
        name = computer->name;
        target = updateFor(computer);
    }
    if (address.isEmpty() || port == 0 || serverCert.isNull() || target.isEmpty()) {
        emit message(tr("%1 can't be updated right now.").arg(name));
        return;
    }

    const QString host = address.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(address) : address;
    QNetworkRequest request(QUrl(QStringLiteral("https://%1:%2/actions/host/update").arg(host).arg(port)));
    // The paired identity, as every other request to the host carries.
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(60000);

    QNetworkReply* reply = m_Nam->post(request, QByteArray("{}"));
    // The host's certificate is self-signed: accepted exactly when it is the one pinned
    // at pairing, never otherwise.
    connect(reply, &QNetworkReply::sslErrors, this, [reply, serverCert](const QList<QSslError>& errors) {
        for (const QSslError& error : errors) {
            if (error.certificate() != serverCert) {
                return;
            }
        }
        reply->ignoreSslErrors(errors);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, uuid, name, target]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
        const QString version = answer.value(QStringLiteral("version")).toString();

        if (status == 202) {
            QString installing = version.isEmpty() ? target : version;
            m_Updating.insert(uuid, qMakePair(installing, QDateTime::currentDateTimeUtc()));
            emit updatingChanged(uuid);
            // So the tile stops saying "Updating" even if the host never comes back.
            QTimer::singleShot((UPDATE_TIMEOUT_SECS + 5) * 1000, this, [this, uuid]() {
                emit updatingChanged(uuid);
            });
            emit message(tr("%1 is updating to Umbra Host %2. It restarts by itself; a stream to it will drop for a minute.")
                         .arg(name, installing.remove(QRegularExpression(QStringLiteral("^v")))));
        }
        else if (status == 200) {
            emit message(tr("%1 is already up to date.").arg(name));
        }
        else if (status == 409) {
            emit message(tr("%1 is already updating.").arg(name));
        }
        else if (status == 401) {
            emit message(tr("%1 doesn't let this device update it. Allow \"Run server commands\" for this device in that host's web interface.").arg(name));
        }
        else if (status == 404) {
            emit message(tr("%1 runs a version of Umbra Host too old to update remotely. Update it on that PC once; after that it can be updated from here.").arg(name));
        }
        else {
            const QString detail = answer.value(QStringLiteral("detail")).toString();
            emit message(tr("%1 couldn't start the update: %2")
                         .arg(name, detail.isEmpty() ? reply->errorString() : detail));
        }
    });
}

void RemoteHostUpdater::noteComputer(NvComputer* computer)
{
    QString name;
    QString version;
    {
        QReadLocker lock(&computer->lock);
        const auto it = m_Updating.constFind(computer->uuid);
        if (it == m_Updating.constEnd() || compareVersions(computer->hostVersion, it->first) < 0) {
            return;
        }
        m_Updating.remove(computer->uuid);
        name = computer->name;
        version = computer->hostVersion;
    }
    emit message(tr("%1 is now on Umbra Host %2.").arg(name, version));
}
