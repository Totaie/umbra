#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

// Files on the clipboard, copied between this machine and a host that has the
// actions API (Umbra Host). The host's half is umbra-host/src/clipboard_files.*,
// and the two keep the same rules for names and limits, so what one side accepts
// the other can write.
//
// Everything travels over the paired HTTPS connection with both certificates
// pinned, in chunks the host's request limit allows. Downloads land in a private
// folder of their own, created new and never through a link; uploads only send
// files the user copied, without following links inside copied folders.
namespace ClipboardFiles {

static constexpr int     MAX_ENTRIES     = 10000;
static constexpr int     MAX_DEPTH       = 32;
static constexpr int     MAX_NAME_BYTES  = 255;
static constexpr int     MAX_PATH_BYTES  = 1024;
static constexpr qint64  MAX_TOTAL_BYTES = 64LL << 30;
// What a copy on the host brings over without being asked: a copy made to paste
// within the host must not start pulling gigabytes across.
static constexpr qint64  AUTO_DOWNLOAD_BYTES = 2LL << 30;
static constexpr int     PROGRESS_INTERVAL_MS = 1000;

struct Entry
{
    QString path;       // relative, '/' separated
    qint64 size = 0;
    bool directory = false;
    QString source;     // uploads: where it is on this machine
};

// The same rules as the host's valid_name(): safe on Windows and POSIX alike.
bool validName(const QString& name, QString* why = nullptr);
bool validRelativePath(const QString& path, QString* why = nullptr);
bool validEntries(const QList<Entry>& entries, QString* why = nullptr);

// What is on the clipboard, as files: copied files show up as local file URLs.
QStringList localFilesFromUrls(const QList<QUrl>& urls);
QStringList localFilesFromUriList(const QByteArray& uriList);
QByteArray uriListFor(const QStringList& paths);

// Lists the copied items and what their folders hold, without following links
// inside them. False, with the reason, if anything cannot be sent.
bool listForUpload(const QStringList& paths, QList<Entry>& out, QString* why);

// Where downloads go: a new private folder under the cache, and an old one swept.
QString newDownloadFolder();
void sweepDownloads();

// One transfer either way, driven by the event loop.
class Transfer : public QObject
{
    Q_OBJECT

public:
    // Builds an authenticated request to the host for a path such as "/actions/clipboard/file?…".
    using RequestFactory = std::function<bool(const QString& pathAndQuery, QNetworkRequest& out)>;

    Transfer(QNetworkAccessManager* nam, RequestFactory factory, QObject* parent = nullptr);
    ~Transfer() override;

    // Sends these local files to the host, which puts them on its clipboard.
    void upload(const QStringList& paths);

    // Fetches the files of a host listing ({"seq","files":[{"p","s","d"}]}).
    void download(quint32 sequence, const QList<Entry>& entries);

    void cancel();

signals:
    void progress(QString message);
    // ok: uploads are on the host's clipboard; downloads are in `topLevelPaths`.
    void finished(bool ok, QString detail, QStringList topLevelPaths);

private:
    void fail(const QString& detail);
    void succeed(const QStringList& paths);
    void sendBegin();
    void sendNextChunk();
    void sendCommit();
    void fetchNextChunk();
    void reportProgress(bool force = false);
    QNetworkReply* post(const QString& pathAndQuery, const QByteArray& body, const char* contentType);
    QNetworkReply* get(const QString& pathAndQuery);

    QNetworkAccessManager* m_Nam;
    RequestFactory m_Factory;
    QPointer<QNetworkReply> m_Reply;
    bool m_Upload = false;
    bool m_Done = false;
    QList<Entry> m_Entries;
    int m_Index = 0;
    qint64 m_Offset = 0;
    qint64 m_TotalBytes = 0;
    qint64 m_DoneBytes = 0;
    int m_ChunkBytes = 768 * 1024;
    QString m_UploadId;
    quint32 m_Sequence = 0;
    QString m_Folder;
    QFile m_File;
    QElapsedTimer m_SinceProgress;
};

}
