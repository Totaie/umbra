#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

    // Runs a check on demand, reporting the outcome either way so the user gets
    // feedback rather than silence when they're already up to date.
    Q_INVOKABLE void checkNow();

signals:
    void onUpdateAvailable(QString newVersion, QString url);
    void onUpToDate(QString currentVersion);
    void onCheckFailed(QString error);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    void parseStringToVersionQuad(QString& string, QVector<int>& version);

    int compareVersion(QVector<int>& version1, QVector<int>& version2);

    QString getPlatform();

    static QString getGitHubToken();

    QVector<int> m_CurrentVersionQuad;
    bool m_CheckedWithoutToken = false;
    bool m_ManualCheck = false;

    void beginRequest();
    QNetworkAccessManager* m_Nam;
};
