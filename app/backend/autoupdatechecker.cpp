#include "autoupdatechecker.h"

#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#define SER_GITHUBTOKEN "githubtoken"

QString AutoUpdateChecker::getGitHubToken()
{
    // A private repo's releases API returns 404 without credentials, so update checks
    // need a token. Being signed into github.com in a browser doesn't help here: Umbra
    // is a separate application and has no access to that session. (The *download* is
    // different — that opens in the browser and does ride your session.)
    //
    // Checked in order of how explicit the user was about it.

    QByteArray envToken = qgetenv("UMBRA_GITHUB_TOKEN");
    if (!envToken.isEmpty()) {
        return QString::fromUtf8(envToken).trimmed();
    }

    QSettings settings;
    QString savedToken = settings.value(SER_GITHUBTOKEN).toString().trimmed();
    if (!savedToken.isEmpty()) {
        return savedToken;
    }

    // Fall back to the GitHub CLI's token if it's installed and logged in. This is what
    // makes "I'm already authenticated on this device" actually work, without Umbra
    // storing a second copy of a credential.
    QString ghPath = QStandardPaths::findExecutable(QStringLiteral("gh"));
    if (!ghPath.isEmpty()) {
        QProcess gh;
        gh.start(ghPath, {QStringLiteral("auth"), QStringLiteral("token")});
        // Short timeout: a hung gh must not delay startup.
        if (gh.waitForFinished(3000) && gh.exitCode() == 0) {
            QString token = QString::fromUtf8(gh.readAllStandardOutput()).trimmed();
            if (!token.isEmpty()) {
                qInfo() << "Using the GitHub CLI's token for update checks";
                return token;
            }
        }
    }

    return QString();
}

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);

    QString currentVersion(VERSION_STR);
    qDebug() << "Current Umbra version:" << currentVersion;
    parseStringToVersionQuad(currentVersion, m_CurrentVersionQuad);

    // Should at least have a 1.0-style version number
    Q_ASSERT(m_CurrentVersionQuad.count() > 1);
}

void AutoUpdateChecker::start()
{
    if (!m_Nam) {
        Q_ASSERT(m_Nam);
        return;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE) // Only run update checker on platforms without auto-update
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // Umbra publishes builds to GitHub Releases rather than hosting a manifest, so we
    // read the releases API directly. /releases/latest deliberately ignores drafts and
    // prereleases, so pushing a prerelease won't offer itself to everyone.
    QUrl url(QStringLiteral("https://api.github.com/repos/" UMBRA_UPDATE_REPO "/releases/latest"));
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub rejects API requests without one
    request.setRawHeader("User-Agent", "Umbra");

    // Only needed while the repo is private, but harmless when it isn't: an authenticated
    // request also gets a far higher rate limit than the anonymous 60/hour.
    QString token = getGitHubToken();
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    }
    else {
        m_CheckedWithoutToken = true;
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

void AutoUpdateChecker::checkNow()
{
    // The automatic check deletes its QNetworkAccessManager once finished, so a manual
    // check has to stand one back up. Doing so also means an on-demand check works even
    // when automatic checks are switched off.
    if (!m_Nam) {
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setStrictTransportSecurityEnabled(true);
        m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        connect(m_Nam, &QNetworkAccessManager::finished,
                this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);
    }

    m_ManualCheck = true;
    start();
}

void AutoUpdateChecker::parseStringToVersionQuad(QString& string, QVector<int>& version)
{
    QStringList list = string.split('.');
    for (const QString& component : std::as_const(list)) {
        version.append(component.toInt());
    }
}

QString AutoUpdateChecker::getPlatform()
{
#if defined(STEAM_LINK)
    return QStringLiteral("steamlink");
#elif defined(APP_IMAGE)
    return QStringLiteral("appimage");
#elif defined(Q_OS_DARWIN) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 changed this from 'osx' to 'macos'. Use the old one
    // to be consistent (and not require another entry in the manifest).
    return QStringLiteral("osx");
#else
    return QSysInfo::productType();
#endif
}

int AutoUpdateChecker::compareVersion(QVector<int>& version1, QVector<int>& version2) {
    for (int i = 0;; i++) {
        int v1Val = 0;
        int v2Val = 0;

        // Treat missing decimal places as 0
        if (i < version1.count()) {
            v1Val = version1[i];
        }
        if (i < version2.count()) {
            v2Val = version2[i];
        }
        if (i >= version1.count() && i >= version2.count()) {
            // Equal versions
            return 0;
        }

        if (v1Val < v2Val) {
            return -1;
        }
        else if (v1Val > v2Val) {
            return 1;
        }
    }
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        stream.setEncoding(QStringConverter::Utf8);
#else
        stream.setCodec("UTF-8");
#endif

        // Read all data and queue the reply for deletion
        QString jsonString = stream.readAll();
        reply->deleteLater();

        QJsonParseError error;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonString.toUtf8(), &error);
        if (jsonDoc.isNull()) {
            qWarning() << "Update manifest malformed:" << error.errorString();
            return;
        }

        // GitHub's releases API returns a single release object, not an array.
        QJsonObject release = jsonDoc.object();
        if (release.isEmpty()) {
            qWarning() << "Release data was not an object";
            return;
        }

        // Tags are conventionally v1.2.3; the version comparison wants bare digits.
        QString latestVersion = release["tag_name"].toString();
        if (latestVersion.startsWith('v') || latestVersion.startsWith('V')) {
            latestVersion.remove(0, 1);
        }
        if (latestVersion.isEmpty()) {
            qWarning() << "Release is missing a tag name";
            return;
        }

        QVector<int> latestVersionQuad;
        parseStringToVersionQuad(latestVersion, latestVersionQuad);

        int res = compareVersion(m_CurrentVersionQuad, latestVersionQuad);
        if (res > 0) {
            qDebug() << "Running a newer build than the latest release:" << latestVersion;
            emit onUpToDate(QString(VERSION_STR));
            m_ManualCheck = false;
            return;
        }
        else if (res == 0) {
            qDebug() << "Already running the latest release:" << latestVersion;
            emit onUpToDate(latestVersion);
            m_ManualCheck = false;
            return;
        }

        // Find the installer built for this machine. Asset names carry the architecture
        // so one release can serve x64 and ARM64; see scripts/publish-release.ps1.
        QString arch = QSysInfo::buildCpuArchitecture();
        QString downloadUrl;
        const QJsonArray assets = release["assets"].toArray();
        for (const auto& assetEntry : assets) {
            QJsonObject asset = assetEntry.toObject();
            QString name = asset["name"].toString();
            if (!name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
                continue;
            }
            if (name.contains(arch, Qt::CaseInsensitive)) {
                downloadUrl = asset["browser_download_url"].toString();
                break;
            }
        }

        m_ManualCheck = false;
        if (downloadUrl.isEmpty()) {
            // A release with no installer for this architecture is not an update we can
            // offer, so stay quiet rather than nagging with a link that goes nowhere.
            qWarning() << "Release" << latestVersion << "has no installer for" << arch;
            return;
        }

        qDebug() << "Update available:" << latestVersion;
        m_ManualCheck = false;
        emit onUpdateAvailable(latestVersion, downloadUrl);
        return;
    }
    else {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404 && m_CheckedWithoutToken) {
            // A private repo is indistinguishable from a missing one when unauthenticated,
            // so say what to do rather than leaving a bare 404 in the log.
            qWarning() << "Update check got 404. If" << UMBRA_UPDATE_REPO << "is private, Umbra needs a "
                          "token: set UMBRA_GITHUB_TOKEN, or log in with the GitHub CLI (gh auth login).";
        }
        else {
            qWarning() << "Update checking failed with error:" << reply->error();
        }

        if (m_ManualCheck) {
            // Silence is indistinguishable from success when the user pressed a button,
            // so always say something.
            emit onCheckFailed(status == 404 && m_CheckedWithoutToken
                                   ? QObject::tr("No releases found. If the repository is private, "
                                                 "sign in with the GitHub CLI or set UMBRA_GITHUB_TOKEN.")
                                   : reply->errorString());
            m_ManualCheck = false;
        }

        reply->deleteLater();
    }
}
