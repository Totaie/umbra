#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QVector>

class QFile;
class QUrl;

class QNetworkReply;
class PortableUpdateInstaller;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

    // A user-initiated check. Unlike start(), this always reports an outcome —
    // silence is fine for the automatic check at launch, but a button the user
    // pressed has to say something back.
    Q_INVOKABLE void checkNow();
    Q_INVOKABLE bool supportsInAppUpdate() const;
    Q_INVOKABLE void installUpdate(QString url);

signals:
    void onUpdateAvailable(QString newVersion, QString url);
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);

    // Only emitted for checkNow().
    // Carries the newest release tag we found, not the version installed here. If the
    // two ever disagree the message says so, instead of reporting a local string that
    // looks stale and can't be told apart from a bad query.
    void onUpToDate(QString latestVersion);
    void onCheckFailed(QString message);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    // Fetch the installer and run it. Only for installed copies - a portable one is
    // updated by PortableUpdateInstaller, which swaps files in place instead.
    void downloadAndRunSetup(const QString& url, const QString& expectedDigest);
    void finishSetupDownload();
    static bool isTrustedReleaseHost(const QUrl& url);

private:
    void parseStringToVersionQuad(const QString& string, QVector<int>& version);

    int compareVersion(const QVector<int>& version1, const QVector<int>& version2);

    bool isPortableInstall() const;
    QString getExpectedAssetPrefix() const;
    QString getExpectedAssetSuffix() const;
    // 同一个后缀里再优先挑本机架构的那个资产（macOS 的 DMG 带 -arm64 / -x86_64）
    QString getPreferredAssetSuffix() const;
    QString getCurrentBuildArch() const;

    // Set while a user-initiated check is in flight, so the reply handler knows
    // whether to report "you're up to date" or stay quiet.
    bool m_ManualCheck = false;

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
    PortableUpdateInstaller* m_PortableUpdateInstaller;
    QString m_UpdateDownloadUrl;
    QString m_UpdateAssetDigest;

    // In-flight installer download.
    //
    // Deliberately its own manager. m_Nam is torn down by the reply handler after every
    // check and its finished signal is wired to the JSON parser - sharing it means
    // either a null dereference or the installer being parsed as a release list.
    QNetworkAccessManager* m_DownloadNam = nullptr;
    QNetworkReply* m_SetupReply = nullptr;
    QFile* m_SetupFile = nullptr;
    QString m_SetupPath;
    QString m_SetupExpectedDigest;
};
