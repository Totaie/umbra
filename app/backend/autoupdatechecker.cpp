#include <QCryptographicHash>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFileInfo>
#include <memory>
#include "autoupdatechecker.h"
#include "umbraversion.h"
#include "portableupdateinstaller.h"
#include "path.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSysInfo>
#include <QTextStream>

// GitHub repository for update checks
#define GITHUB_OWNER "Totaie"
#define GITHUB_REPO  "umbra"

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
    m_PortableUpdateInstaller = new PortableUpdateInstaller(this);

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

    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateStatusChanged,
            this, &AutoUpdateChecker::onPortableUpdateStatusChanged);
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateFailed,
            this, &AutoUpdateChecker::onPortableUpdateFailed);
}

bool AutoUpdateChecker::supportsInAppUpdate() const
{
    if (m_PortableUpdateInstaller->supportsInAppUpdate()) {
        return true;
    }

#if defined(Q_OS_WIN32)
    // An installed copy updates by running the installer, which already closes the
    // running Umbra and elevates itself. Nothing for us to do but fetch it and hand
    // over - but only if the release actually published a package we can verify.
    return !isPortableInstall() &&
            m_UpdateDownloadUrl.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) &&
            !m_UpdateAssetDigest.isEmpty();
#else
    return false;
#endif
}

void AutoUpdateChecker::installUpdate(QString url)
{
    const QString expectedDigest = url == m_UpdateDownloadUrl ? m_UpdateAssetDigest : QString();

    if (m_PortableUpdateInstaller->supportsInAppUpdate()) {
        m_PortableUpdateInstaller->installUpdate(url, expectedDigest);
        return;
    }

#if defined(Q_OS_WIN32)
    m_SetupIsHostPackage = false;
    downloadAndRunSetup(url, expectedDigest);
#else
    Q_UNUSED(expectedDigest);
    emit onPortableUpdateFailed(tr("In-app update is not supported for this installation."));
#endif
}

void AutoUpdateChecker::installHostPackage(QString url, QString sha256)
{
#if defined(Q_OS_WIN32)
    m_SetupIsHostPackage = true;
    downloadAndRunSetup(url, sha256);
#else
    Q_UNUSED(url);
    Q_UNUSED(sha256);
    emit onPortableUpdateFailed(tr("Umbra Host is only available on Windows."));
#endif
}

bool AutoUpdateChecker::isTrustedReleaseHost(const QUrl& url)
{
    if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    // Where our releases and their assets actually live. Checked again after every
    // redirect, because browser_download_url redirects to the storage host and a
    // redirect is somewhere we did not choose to go.
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com") ||
           host.endsWith(QStringLiteral(".githubusercontent.com"));
}

void AutoUpdateChecker::downloadAndRunSetup(const QString& url, const QString& expectedDigest)
{
    if (m_SetupReply != nullptr) {
        emit onPortableUpdateStatusChanged(tr("An update is already in progress."));
        return;
    }

    const QUrl downloadUrl(url);
    if (!downloadUrl.isValid() || !isTrustedReleaseHost(downloadUrl) ||
            !downloadUrl.path().endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        emit onPortableUpdateFailed(tr("That update package didn't come from Umbra's releases."));
        return;
    }

    // Required, not optional. This runs what it downloads, on a machine the user
    // intends to put on the internet - a package we can't check against what GitHub
    // published is one they should fetch themselves.
    QString normalizedDigest = expectedDigest.trimmed();
    if (normalizedDigest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
        normalizedDigest.remove(0, 7);
    }

    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!sha256Pattern.match(normalizedDigest).hasMatch()) {
        emit onPortableUpdateFailed(tr("This release didn't publish a checksum for its installer, so Umbra won't run it. Download it from the release page instead."));
        return;
    }

    m_SetupExpectedDigest = normalizedDigest.toLower();

    // Somewhere the installer can still be read after we exit, and that the running
    // Umbra doesn't have open when the installer tries to replace it.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_SetupPath = QDir(dir).filePath(QFileInfo(downloadUrl.path()).fileName());

    m_SetupFile = new QFile(m_SetupPath, this);
    if (!m_SetupFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete m_SetupFile;
        m_SetupFile = nullptr;
        emit onPortableUpdateFailed(tr("Umbra couldn't write the update to disk."));
        return;
    }

    QNetworkRequest request(downloadUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Umbra"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    emit onPortableUpdateStatusChanged(tr("Downloading update..."));

    // Never m_Nam: it is null by now, and even when it isn't, its finished signal goes
    // to the release-list parser.
    if (m_DownloadNam == nullptr) {
        m_DownloadNam = new QNetworkAccessManager(this);
        m_DownloadNam->setStrictTransportSecurityEnabled(true);
        m_DownloadNam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }

    m_SetupReply = m_DownloadNam->get(request);
    connect(m_SetupReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_SetupFile != nullptr) {
            m_SetupFile->write(m_SetupReply->readAll());
        }
    });
    connect(m_SetupReply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        if (total > 0) {
            emit onPortableUpdateStatusChanged(tr("Downloading update... %1%")
                                               .arg(received * 100 / total));
        }
    });
    connect(m_SetupReply, &QNetworkReply::finished, this, &AutoUpdateChecker::finishSetupDownload);
}

void AutoUpdateChecker::finishSetupDownload()
{
    QNetworkReply* reply = m_SetupReply;
    m_SetupReply = nullptr;

    std::unique_ptr<QFile> file(m_SetupFile);
    m_SetupFile = nullptr;

    if (reply != nullptr) {
        reply->deleteLater();
    }

    auto discard = [this, &file]() {
        if (file) {
            file->close();
            file->remove();
        }
        m_SetupPath.clear();
    };

    if (reply == nullptr || reply->error() != QNetworkReply::NoError) {
        discard();
        emit onPortableUpdateFailed(tr("The update download failed: %1")
                                    .arg(reply != nullptr ? reply->errorString() : tr("unknown error")));
        return;
    }

    // Where we ended up, not where we asked to go.
    if (!isTrustedReleaseHost(reply->url())) {
        discard();
        emit onPortableUpdateFailed(tr("The update download was redirected somewhere unexpected."));
        return;
    }

    if (file) {
        file->write(reply->readAll());
        file->flush();
        file->close();
    }

    QFile verify(m_SetupPath);
    if (!verify.open(QIODevice::ReadOnly)) {
        discard();
        emit onPortableUpdateFailed(tr("Umbra couldn't read the update it just downloaded."));
        return;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&verify)) {
        verify.close();
        discard();
        emit onPortableUpdateFailed(tr("Umbra couldn't check the update it just downloaded."));
        return;
    }
    verify.close();

    if (hash.result().toHex().toLower() != m_SetupExpectedDigest.toLatin1()) {
        discard();
        emit onPortableUpdateFailed(tr("The update didn't match the checksum GitHub published for it, so Umbra won't run it."));
        return;
    }

    // Detached on purpose: the installer closes this process as part of its own work,
    // so it must outlive us. It elevates itself; nothing here needs administrator
    // rights, which is what makes this work over a remote session.
    if (!QProcess::startDetached(m_SetupPath, QStringList())) {
        emit onPortableUpdateFailed(tr("Umbra couldn't start the installer. It was saved to %1.")
                                    .arg(QDir::toNativeSeparators(m_SetupPath)));
        return;
    }

    // Said last, because the installer takes over from here: it asks for administrator
    // rights, and then either closes this copy of Umbra to replace it or restarts the
    // host service, depending on which package this was.
    emit onPortableUpdateStatusChanged(
        m_SetupIsHostPackage
            ? tr("The Umbra Host installer is starting. It will ask for administrator permission, "
                 "then restart the host. Umbra itself stays open.")
            : tr("The installer is starting. Umbra will close to finish updating."));
}

void AutoUpdateChecker::checkNow()
{
    // The reply handler tears the QNetworkAccessManager down after every check so the
    // bearer plugin stops polling in the background, which means a second check has to
    // build a new one.
    if (!m_Nam) {
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setStrictTransportSecurityEnabled(true);
        m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        connect(m_Nam, &QNetworkAccessManager::finished,
                this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);
    }

    m_ManualCheck = true;
    start();

#if !defined(Q_OS_WIN32) && !defined(Q_OS_DARWIN) && !defined(STEAM_LINK) && !defined(APP_IMAGE)
    // start() compiles to nothing on platforms without an update feed, so the button
    // would otherwise spin forever.
    m_ManualCheck = false;
    emit onCheckFailed(tr("Update checking isn't available in this build of Umbra."));
#endif
}

void AutoUpdateChecker::start()
{
    // The reply handler drops the manager after every check so the bearer plugin stops
    // polling. Rebuild rather than return: this used to be a bare Q_ASSERT, which
    // compiles out in release, so a second automatic check did nothing at all and left
    // the previous result on screen looking like a stale version number.
    if (!m_Nam) {
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setStrictTransportSecurityEnabled(true);
        m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        connect(m_Nam, &QNetworkAccessManager::finished,
                this, &AutoUpdateChecker::handleUpdateCheckRequestFinished);
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // Query GitHub Releases API for the latest release
    // The list endpoint rather than /releases/latest, which by definition only
    // returns a release marked neither prerelease nor draft. Every Umbra release so
    // far is a prerelease, so /releases/latest simply 404s and the updater could
    // never see anything - while reporting the machine as up to date.
    QUrl url(QString("https://api.github.com/repos/%1/%2/releases?per_page=30")
                 .arg(GITHUB_OWNER, GITHUB_REPO));
    QNetworkRequest request(url);

    // GitHub API requires a User-Agent header
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QString("Umbra/%1").arg(VERSION_STR));
    // Request JSON response
    request.setRawHeader("Accept", "application/vnd.github+json");

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    m_Nam->get(request);
#endif
}

void AutoUpdateChecker::parseStringToVersionQuad(const QString& string, QVector<int>& version)
{
    version.clear();

    // Strip leading 'v' and ignore SemVer suffixes/build metadata:
    //   v6.2.82                  -> 6.2.82
    //   6.2.82+14.g13ca12da.dirty -> 6.2.82
    //   v6.2.82-14-g13ca12da      -> 6.2.82
    QString versionStr = string.trimmed();
    if (versionStr.startsWith('v') || versionStr.startsWith('V')) {
        versionStr = versionStr.mid(1);
    }

    int suffixIndex = versionStr.indexOf('+');
    int prereleaseIndex = versionStr.indexOf('-');
    if (suffixIndex < 0 || (prereleaseIndex >= 0 && prereleaseIndex < suffixIndex)) {
        suffixIndex = prereleaseIndex;
    }
    if (suffixIndex >= 0) {
        versionStr = versionStr.left(suffixIndex);
    }

    QStringList list = versionStr.split('.');
    for (const QString& component : std::as_const(list)) {
        bool ok = false;
        int value = component.toInt(&ok);
        if (!ok) {
            break;
        }
        version.append(value);
    }
}

QString AutoUpdateChecker::getPreferredAssetSuffix() const
{
#if defined(Q_OS_DARWIN)
    // CI 出的 DMG 现在带架构后缀（Umbra-<版本>-arm64.dmg）。
    // QSysInfo::buildCpuArchitecture() 给的是 arm64 / x86_64，和 generate-dmg.sh
    // 里的 UMBRA_ARCH 用词一致。
    //
    // 只是「优先」而不是「必须」：这个后缀是从某个版本才开始有的，旧 release 里是
    // Umbra-<版本>.dmg。匹配不到就退回任意 .dmg，否则老版本的用户会看到
    // 「找不到更新包」。
    return QStringLiteral("-") + QSysInfo::buildCpuArchitecture() + QStringLiteral(".dmg");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getExpectedAssetSuffix() const
{
#if defined(Q_OS_WIN32)
    return isPortableInstall() ? QStringLiteral(".zip") : QStringLiteral(".exe");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    return QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

bool AutoUpdateChecker::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir(Path::getPortableRootDir()).filePath("portable.dat"));
#else
    return false;
#endif
}

QString AutoUpdateChecker::getExpectedAssetPrefix() const
{
#if defined(Q_OS_WIN32)
    if (isPortableInstall()) {
        return QStringLiteral("UmbraPortable-%1-").arg(getCurrentBuildArch());
    }

    // The releases publish UmbraSetup-x64-<version>.exe. This said MoonlightSetup-
    // until now, so nothing ever matched and every update fell back to opening the
    // release page in a browser.
    return QStringLiteral("UmbraSetup-");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getCurrentBuildArch() const
{
    QString buildArch = QSysInfo::buildCpuArchitecture();

    if (buildArch == "x86_64") {
        return QStringLiteral("x64");
    }
    else if (buildArch == "i386") {
        return QStringLiteral("x86");
    }

    return buildArch.toLower();
}

int AutoUpdateChecker::compareVersion(const QVector<int>& version1, const QVector<int>& version2) {
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

    // Consumed once per reply, so the paths below can return without worrying about
    // leaving a manual check latched on.
    const bool manualCheck = m_ManualCheck;
    m_ManualCheck = false;

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
            qWarning() << "GitHub release response malformed:" << error.errorString();
            if (manualCheck) {
                emit onCheckFailed(tr("The update server sent a response Umbra couldn't read."));
            }
            return;
        }

        if (!jsonDoc.isArray()) {
            qWarning() << "GitHub releases response is not a JSON array";
            if (manualCheck) {
                emit onCheckFailed(tr("The update server sent a response Umbra couldn't read."));
            }
            return;
        }

        // Pick the highest version rather than the first entry. GitHub orders by
        // creation date, and releases published close together can share a timestamp,
        // at which point the order is arbitrary.
        QJsonArray releases = jsonDoc.array();
        QJsonObject releaseObj;
        QVector<int> bestVersion;

        for (const QJsonValue& value : releases) {
            if (!value.isObject()) {
                continue;
            }

            QJsonObject candidate = value.toObject();

            // Drafts are not published to anyone. Prereleases are what we ship, so
            // they count.
            if (candidate["draft"].toBool(false)) {
                continue;
            }

            if (!candidate.contains("tag_name") || !candidate["tag_name"].isString()) {
                continue;
            }

            QVector<int> candidateVersion;
            parseStringToVersionQuad(candidate["tag_name"].toString(), candidateVersion);
            if (candidateVersion.isEmpty()) {
                continue;
            }

            if (bestVersion.isEmpty() || compareVersion(bestVersion, candidateVersion) < 0) {
                bestVersion = candidateVersion;
                releaseObj = candidate;
            }
        }

        if (releaseObj.isEmpty()) {
            qWarning() << "No usable releases found";
            if (manualCheck) {
                emit onCheckFailed(tr("No Umbra releases were found to compare against."));
            }
            return;
        }

        // GitHub Releases API response format:
        // {
        //   "tag_name": "v6.3.0",
        //   "name": "Release 6.3.0",
        //   "html_url": "https://github.com/owner/repo/releases/tag/v6.3.0",
        //   "prerelease": false,
        //   "draft": false,
        //   "assets": [
        //     {
        //       "name": "MoonlightSetup-x64-6.3.0.exe",
        //       "browser_download_url": "https://github.com/..."
        //     }
        //   ]
        // }

        if (!releaseObj.contains("tag_name") || !releaseObj["tag_name"].isString()) {
            qWarning() << "GitHub release missing tag_name";
            if (manualCheck) {
                emit onCheckFailed(tr("The update server sent a response Umbra couldn't read."));
            }
            return;
        }

        QString tagName = releaseObj["tag_name"].toString();
        qDebug() << "Latest GitHub release tag:" << tagName;

        // Parse version from tag (strip 'v' prefix if present)
        QVector<int> latestVersionQuad;
        parseStringToVersionQuad(tagName, latestVersionQuad);

        int res = compareVersion(m_CurrentVersionQuad, latestVersionQuad);
        if (res < 0) {
            // Current version is older than latest release
            qDebug() << "Update available:" << tagName;

            // Try to find a platform-specific download URL from assets
            QString downloadUrl;
            QString assetDigest;
            QString expectedPrefix = getExpectedAssetPrefix();
            QString expectedSuffix = getExpectedAssetSuffix();

            QString preferredSuffix = getPreferredAssetSuffix();

            if (!expectedSuffix.isEmpty() && releaseObj.contains("assets") && releaseObj["assets"].isArray()) {
                QJsonArray assets = releaseObj["assets"].toArray();

                // 后备候选：后缀对得上但不带本机架构后缀的那个（旧 release 的命名）
                QString fallbackUrl;
                QString fallbackName;
                QString fallbackDigest;

                for (const auto& asset : std::as_const(assets)) {
                    if (asset.isObject()) {
                        QJsonObject assetObj = asset.toObject();
                        QString assetName = assetObj["name"].toString();
                        bool prefixMatches = expectedPrefix.isEmpty() ||
                                             assetName.startsWith(expectedPrefix, Qt::CaseInsensitive);
                        bool suffixMatches = assetName.endsWith(expectedSuffix, Qt::CaseInsensitive);

                        if (!prefixMatches || !suffixMatches) {
                            continue;
                        }

                        if (!preferredSuffix.isEmpty() &&
                                assetName.endsWith(preferredSuffix, Qt::CaseInsensitive)) {
                            downloadUrl = assetObj["browser_download_url"].toString();
                            assetDigest = assetObj["digest"].toString();
                            qDebug() << "Found matching asset for this architecture:" << assetName;
                            break;
                        }

                        // 后备只认「没带架构后缀」的旧命名。带了别的架构后缀的资产
                        // 绝对不能当后备 —— 只发了 arm64 包的 release 会把 arm64 的
                        // DMG 喂给 Intel 客户端。这种情况下宁可让 downloadUrl 留空，
                        // 退回打开 release 页面让用户自己看。
                        bool isOtherArchAsset =
                                assetName.endsWith(QStringLiteral("-arm64.dmg"), Qt::CaseInsensitive) ||
                                assetName.endsWith(QStringLiteral("-x86_64.dmg"), Qt::CaseInsensitive);

                        if (fallbackUrl.isEmpty() && !isOtherArchAsset) {
                            fallbackUrl = assetObj["browser_download_url"].toString();
                            fallbackName = assetName;
                            fallbackDigest = assetObj["digest"].toString();
                        }
                    }
                }

                if (downloadUrl.isEmpty() && !fallbackUrl.isEmpty()) {
                    downloadUrl = fallbackUrl;
                    assetDigest = fallbackDigest;
                    qDebug() << "Found matching asset:" << fallbackName;
                }
            }

            // Fall back to the release page URL if no matching asset found
            if (downloadUrl.isEmpty()) {
                downloadUrl = releaseObj["html_url"].toString();
            }

            m_UpdateDownloadUrl = downloadUrl;
            m_UpdateAssetDigest = assetDigest;

            emit onUpdateAvailable(tagName, downloadUrl);
        }
        else if (res > 0) {
            qDebug() << "Current version is newer than latest release";
            if (manualCheck) {
                emit onUpToDate(tagName);
            }
        }
        else {
            qDebug() << "Current version matches latest release";
            if (manualCheck) {
                emit onUpToDate(tagName);
            }
        }
    }
    else {
        qWarning() << "Update checking failed:" << reply->error() << reply->errorString();

        if (manualCheck) {
            // A 404 used to be reported as "up to date". It is not: it means the
            // question could not be answered, and saying otherwise is how an out of
            // date install was told it was current.
            emit onCheckFailed(tr("Umbra couldn't reach the update server: %1")
                               .arg(reply->errorString()));
        }

        reply->deleteLater();
    }
}
