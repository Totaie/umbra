#include "clipboardfiles.h"
#include "clipboardlogging.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ClipboardFiles {

namespace {

void say(QString* why, const QString& text)
{
    if (why != nullptr) {
        *why = text;
    }
}

bool reservedOnWindows(const QString& name)
{
    // Device names stay devices with any extension, and with spaces before it.
    QString base = name.section(QLatin1Char('.'), 0, 0).toLower();
    while (base.endsWith(QLatin1Char(' '))) {
        base.chop(1);
    }
    static const QStringList words = { QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
                                       QStringLiteral("nul"), QStringLiteral("conin$"), QStringLiteral("conout$") };
    if (words.contains(base)) {
        return true;
    }
    if (base.size() == 4 && (base.startsWith(QLatin1String("com")) || base.startsWith(QLatin1String("lpt")))) {
        const QChar last = base.at(3);
        return (last >= QLatin1Char('0') && last <= QLatin1Char('9')) ||
               last == QChar(0x00B9) || last == QChar(0x00B2) || last == QChar(0x00B3);
    }
    return false;
}

QString downloadRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/Umbra/Pasted files");
}

QString humanSize(qint64 bytes)
{
    if (bytes >= (1LL << 30)) {
        return QStringLiteral("%1 GB").arg(bytes / double(1LL << 30), 0, 'f', 1);
    }
    if (bytes >= (1LL << 20)) {
        return QStringLiteral("%1 MB").arg(bytes / double(1LL << 20), 0, 'f', 1);
    }
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024));
}

bool isIdName(const QString& name)
{
    if (name.size() != 32) {
        return false;
    }
    for (const QChar c : name) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

// A plain file or folder, reached without following a link.
enum class Kind { Other, File, Folder };

Kind kindOf(const QString& path, qint64* size = nullptr)
{
#ifdef Q_OS_UNIX
    struct stat st {};
    if (lstat(QFile::encodeName(path).constData(), &st) != 0) {
        return Kind::Other;
    }
    if (size != nullptr) {
        *size = st.st_size;
    }
    return S_ISREG(st.st_mode) ? Kind::File : S_ISDIR(st.st_mode) ? Kind::Folder : Kind::Other;
#else
    const QFileInfo fi(path);
    if (fi.isSymLink() || fi.isJunction() || !fi.exists()) {
        return Kind::Other;
    }
    if (size != nullptr) {
        *size = fi.size();
    }
    return fi.isDir() ? Kind::Folder : fi.isFile() ? Kind::File : Kind::Other;
#endif
}

}

bool validName(const QString& name, QString* why)
{
    const QByteArray utf8 = name.toUtf8();
    if (utf8.isEmpty() || utf8.size() > MAX_NAME_BYTES) {
        say(why, QStringLiteral("a name is empty or too long"));
        return false;
    }
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        say(why, QStringLiteral("a name is . or .."));
        return false;
    }
    if (!name.isValidUtf16()) {
        say(why, QStringLiteral("a name is not valid Unicode"));
        return false;
    }
    for (const QChar c : name) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7F || QStringLiteral("<>:\"/\\|?*").contains(c)) {
            say(why, QStringLiteral("\"%1\" has a character Windows does not allow in a name").arg(name));
            return false;
        }
    }
    if (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))) {
        say(why, QStringLiteral("\"%1\" ends in a space or a dot, which Windows does not allow").arg(name));
        return false;
    }
    if (reservedOnWindows(name)) {
        say(why, QStringLiteral("\"%1\" is a Windows device name").arg(name));
        return false;
    }
    return true;
}

bool validRelativePath(const QString& path, QString* why)
{
    if (path.isEmpty() || path.toUtf8().size() > MAX_PATH_BYTES) {
        say(why, QStringLiteral("a path is empty or too long"));
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    if (parts.size() > MAX_DEPTH) {
        say(why, QStringLiteral("a folder is nested too deeply"));
        return false;
    }
    for (const QString& part : parts) {
        if (!validName(part, why)) {
            return false;
        }
    }
    return true;
}

bool validEntries(const QList<Entry>& entries, QString* why)
{
    if (entries.isEmpty() || entries.size() > MAX_ENTRIES) {
        say(why, QStringLiteral("too many files, or none"));
        return false;
    }
    QSet<QString> seen;
    QSet<QString> folders;
    qint64 total = 0;
    for (const Entry& e : entries) {
        if (!validRelativePath(e.path, why)) {
            return false;
        }
        // Windows compares names without case.
        const QString key = e.path.toLower();
        if (seen.contains(key)) {
            say(why, QStringLiteral("two entries have the same name"));
            return false;
        }
        seen.insert(key);
        const int slash = key.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0 && !folders.contains(key.left(slash))) {
            say(why, QStringLiteral("an entry comes before the folder it is in"));
            return false;
        }
        if (e.directory) {
            if (e.size != 0) {
                say(why, QStringLiteral("a folder has a size"));
                return false;
            }
            folders.insert(key);
        }
        if (e.size < 0 || e.size > MAX_TOTAL_BYTES || total + e.size > MAX_TOTAL_BYTES) {
            say(why, QStringLiteral("the files are too large in total"));
            return false;
        }
        total += e.size;
    }
    return true;
}

QStringList localFilesFromUrls(const QList<QUrl>& urls)
{
    QStringList out;
    for (const QUrl& url : urls) {
        if (url.isLocalFile() && !url.toLocalFile().isEmpty()) {
            out.append(url.toLocalFile());
        }
    }
    return out;
}

QStringList localFilesFromUriList(const QByteArray& uriList)
{
    QList<QUrl> urls;
    for (QByteArray line : uriList.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        urls.append(QUrl::fromEncoded(line, QUrl::StrictMode));
    }
    return localFilesFromUrls(urls);
}

QByteArray uriListFor(const QStringList& paths)
{
    QByteArray out;
    for (const QString& p : paths) {
        out += QUrl::fromLocalFile(p).toEncoded() + "\r\n";
    }
    return out;
}

bool listForUpload(const QStringList& paths, QList<Entry>& out, QString* why)
{
    out.clear();
    QSet<QString> topNames;
    qint64 total = 0;

    std::function<bool(const QString&, const QString&, int)> walk =
        [&](const QString& folder, const QString& rel, int depth) -> bool {
        if (depth >= MAX_DEPTH) {
            say(why, QStringLiteral("a folder is nested too deeply"));
            return false;
        }
        const QStringList names = QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                         QDir::Name);
        for (const QString& name : names) {
            const QString path = folder + QLatin1Char('/') + name;
            const QString childRel = rel + QLatin1Char('/') + name;
            qint64 size = 0;
            const Kind kind = kindOf(path, &size);
            if (kind == Kind::Other) {
                continue;   // links, sockets, devices
            }
            if (!validRelativePath(childRel, why)) {
                return false;
            }
            if (out.size() >= MAX_ENTRIES || total + size > MAX_TOTAL_BYTES) {
                say(why, QStringLiteral("too many files, or too large in total"));
                return false;
            }
            total += size;
            if (kind == Kind::Folder) {
                out.append({ childRel, 0, true, path });
                if (!walk(path, childRel, depth + 1)) {
                    return false;
                }
            }
            else {
                out.append({ childRel, size, false, path });
            }
        }
        return true;
    };

    for (const QString& top : paths) {
        // A link that was copied is followed this once, to what it names.
        const QString real = QFileInfo(top).canonicalFilePath();
        if (real.isEmpty()) {
            say(why, QStringLiteral("\"%1\" is not there any more").arg(top));
            return false;
        }
        const QString name = QFileInfo(real).fileName();
        if (!validName(name, why)) {
            return false;
        }
        if (topNames.contains(name.toLower())) {
            continue;
        }
        topNames.insert(name.toLower());
        qint64 size = 0;
        const Kind kind = kindOf(real, &size);
        if (kind == Kind::Folder) {
            out.append({ name, 0, true, real });
            if (!walk(real, name, 1)) {
                return false;
            }
        }
        else if (kind == Kind::File) {
            total += size;
            out.append({ name, size, false, real });
        }
    }
    if (out.isEmpty()) {
        say(why, QStringLiteral("nothing that can be copied"));
        return false;
    }
    return validEntries(out, why);
}

QString newDownloadFolder()
{
    const QString root = downloadRoot();
    if (!QDir().mkpath(root)) {
        return QString();
    }
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    QString id;
    for (quint32 w : words) {
        id += QStringLiteral("%1").arg(w, 8, 16, QLatin1Char('0'));
    }
    const QString folder = root + QLatin1Char('/') + id;
    // Created new or not at all, private to this user.
    if (!QDir().mkdir(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return QString();
    }
    return folder;
}

void sweepDownloads()
{
    QDir root(downloadRoot());
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-24 * 3600);
    for (const QFileInfo& fi : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (isIdName(fi.fileName()) && fi.lastModified().toUTC() < cutoff) {
            QDir(fi.absoluteFilePath()).removeRecursively();
        }
    }
}

Transfer::Transfer(QNetworkAccessManager* nam, RequestFactory factory, QObject* parent)
    : QObject(parent),
      m_Nam(nam),
      m_Factory(std::move(factory))
{
}

Transfer::~Transfer()
{
    cancel();
}

QNetworkReply* Transfer::post(const QString& pathAndQuery, const QByteArray& body, const char* contentType)
{
    QNetworkRequest req;
    if (!m_Factory(pathAndQuery, req)) {
        return nullptr;
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(contentType));
    return m_Nam->post(req, body);
}

QNetworkReply* Transfer::get(const QString& pathAndQuery)
{
    QNetworkRequest req;
    if (!m_Factory(pathAndQuery, req)) {
        return nullptr;
    }
    return m_Nam->get(req);
}

void Transfer::cancel()
{
    if (m_Reply != nullptr) {
        m_Reply->abort();
    }
    if (!m_Done) {
        m_Done = true;
        if (m_Upload && !m_UploadId.isEmpty()) {
            if (QNetworkReply* reply = post(QStringLiteral("/actions/clipboard/files/abort?id=") + m_UploadId, {}, "text/plain")) {
                connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
            }
        }
        if (!m_Upload && !m_Folder.isEmpty()) {
            m_File.close();
            QDir(m_Folder).removeRecursively();
        }
    }
}

void Transfer::fail(const QString& detail)
{
    if (m_Done) {
        return;
    }
    m_File.close();
    cancel();
    ClipboardLog::warn("ClipboardSync: file %s failed: %s", m_Upload ? "upload" : "download", detail.toUtf8().constData());
    emit finished(false, detail, {});
}

void Transfer::succeed(const QStringList& paths)
{
    m_Done = true;
    m_File.close();
    reportProgress(true);
    emit finished(true, QString(), paths);
}

void Transfer::reportProgress(bool force)
{
    if (!force && m_SinceProgress.isValid() && m_SinceProgress.elapsed() < PROGRESS_INTERVAL_MS) {
        return;
    }
    // Small copies finish before anyone could read a progress message.
    if (!force && m_TotalBytes < (8LL << 20)) {
        return;
    }
    m_SinceProgress.start();
    int files = 0;
    for (const Entry& e : m_Entries) {
        files += e.directory ? 0 : 1;
    }
    const int percent = m_TotalBytes > 0 ? int(m_DoneBytes * 100 / m_TotalBytes) : 100;
    emit progress(m_Upload
                  ? QStringLiteral("Copying %1 file(s), %2, to the host… %3%").arg(files).arg(humanSize(m_TotalBytes)).arg(percent)
                  : QStringLiteral("Copying %1 file(s), %2, from the host… %3%").arg(files).arg(humanSize(m_TotalBytes)).arg(percent));
}

// ---- upload ----

void Transfer::upload(const QStringList& paths)
{
    m_Upload = true;
    QString why;
    if (!listForUpload(paths, m_Entries, &why)) {
        fail(why);
        return;
    }
    for (const Entry& e : m_Entries) {
        m_TotalBytes += e.size;
    }
    sendBegin();
}

void Transfer::sendBegin()
{
    QJsonArray files;
    for (const Entry& e : m_Entries) {
        files.append(QJsonObject{ { QStringLiteral("p"), e.path },
                                  { QStringLiteral("s"), double(e.size) },
                                  { QStringLiteral("d"), e.directory } });
    }
    const QByteArray body = QJsonDocument(QJsonObject{ { QStringLiteral("files"), files } }).toJson(QJsonDocument::Compact);
    m_Reply = post(QStringLiteral("/actions/clipboard/files/begin"), body, "application/json");
    if (m_Reply == nullptr) {
        fail(QStringLiteral("no host to send the files to"));
        return;
    }
    connect(m_Reply, &QNetworkReply::finished, this, [this, reply = m_Reply.data()]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray answer = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            fail(status == 401 ? QStringLiteral("the host does not let this device paste files (device permissions in the host's web UI)")
                 : status == 501 ? QStringLiteral("the host cannot take files")
                 : status == 400 && !answer.isEmpty() ? QStringLiteral("the host refused the files: %1").arg(QString::fromUtf8(answer.left(200)))
                 : QStringLiteral("the host did not take the files: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(answer).object();
        m_UploadId = obj.value(QStringLiteral("id")).toString();
        const int chunk = obj.value(QStringLiteral("chunk")).toInt(0);
        if (m_UploadId.size() != 32 || chunk <= 0 || chunk > (1 << 20)) {
            fail(QStringLiteral("the host's answer made no sense"));
            return;
        }
        m_ChunkBytes = chunk;
        m_Index = 0;
        m_Offset = 0;
        sendNextChunk();
    });
}

void Transfer::sendNextChunk()
{
    // The next file with bytes still to send.
    while (m_Index < m_Entries.size() &&
           (m_Entries[m_Index].directory || m_Offset >= m_Entries[m_Index].size)) {
        m_File.close();
        m_Index++;
        m_Offset = 0;
    }
    if (m_Index >= m_Entries.size()) {
        sendCommit();
        return;
    }

    const Entry& entry = m_Entries[m_Index];
    if (!m_File.isOpen()) {
#ifdef Q_OS_UNIX
        const int fd = ::open(QFile::encodeName(entry.source).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != entry.size ||
                !m_File.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            if (fd >= 0 && !m_File.isOpen()) {
                ::close(fd);
            }
            fail(QStringLiteral("\"%1\" changed or went away while it was being sent").arg(entry.path));
            return;
        }
#else
        m_File.setFileName(entry.source);
        if (kindOf(entry.source) != Kind::File || !m_File.open(QIODevice::ReadOnly) || m_File.size() != entry.size) {
            fail(QStringLiteral("\"%1\" changed or went away while it was being sent").arg(entry.path));
            return;
        }
#endif
    }

    const qint64 want = qMin<qint64>(m_ChunkBytes, entry.size - m_Offset);
    const QByteArray data = m_File.read(want);
    if (data.size() != want) {
        fail(QStringLiteral("\"%1\" got shorter while it was being sent").arg(entry.path));
        return;
    }

    m_Reply = post(QStringLiteral("/actions/clipboard/files/chunk?id=%1&i=%2&offset=%3").arg(m_UploadId).arg(m_Index).arg(m_Offset),
                   data, "application/octet-stream");
    if (m_Reply == nullptr) {
        fail(QStringLiteral("no host to send the files to"));
        return;
    }
    connect(m_Reply, &QNetworkReply::finished, this, [this, reply = m_Reply.data(), size = data.size()]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("the host stopped taking the files: %1").arg(QString::fromUtf8(reply->readAll().left(200))));
            return;
        }
        m_Offset += size;
        m_DoneBytes += size;
        reportProgress();
        sendNextChunk();
    });
}

void Transfer::sendCommit()
{
    m_Reply = post(QStringLiteral("/actions/clipboard/files/commit?id=") + m_UploadId, {}, "text/plain");
    if (m_Reply == nullptr) {
        fail(QStringLiteral("no host to send the files to"));
        return;
    }
    connect(m_Reply, &QNetworkReply::finished, this, [this, reply = m_Reply.data()]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("the host could not put the files on its clipboard: %1").arg(QString::fromUtf8(reply->readAll().left(200))));
            return;
        }
        m_UploadId.clear();   // committed: nothing to abort any more
        succeed({});
    });
}

// ---- download ----

void Transfer::download(quint32 sequence, const QList<Entry>& entries)
{
    m_Upload = false;
    m_Sequence = sequence;
    m_Entries = entries;
    QString why;
    if (!validEntries(m_Entries, &why)) {
        fail(QStringLiteral("the host's listing is not safe to write: %1").arg(why));
        return;
    }
    for (const Entry& e : m_Entries) {
        m_TotalBytes += e.size;
    }

    m_Folder = newDownloadFolder();
    if (m_Folder.isEmpty()) {
        fail(QStringLiteral("could not make a folder for the files"));
        return;
    }
    const auto privateDir = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    for (const Entry& e : m_Entries) {
        const QString path = m_Folder + QLatin1Char('/') + e.path;
        if (e.directory) {
            if (!QDir().mkdir(path, privateDir)) {
                fail(QStringLiteral("could not make a folder for the files"));
                return;
            }
        }
        else if (e.size == 0) {
            QFile empty(path);
            if (!empty.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                fail(QStringLiteral("could not create \"%1\"").arg(e.path));
                return;
            }
        }
    }
    m_Index = 0;
    m_Offset = 0;
    fetchNextChunk();
}

void Transfer::fetchNextChunk()
{
    while (m_Index < m_Entries.size() &&
           (m_Entries[m_Index].directory || m_Offset >= m_Entries[m_Index].size)) {
        m_File.close();
        m_Index++;
        m_Offset = 0;
    }
    if (m_Index >= m_Entries.size()) {
        QStringList tops;
        for (const Entry& e : m_Entries) {
            if (!e.path.contains(QLatin1Char('/'))) {
                tops.append(m_Folder + QLatin1Char('/') + e.path);
            }
        }
        succeed(tops);
        return;
    }

    const Entry& entry = m_Entries[m_Index];
    if (!m_File.isOpen()) {
        // New, never through something already there, readable by this user only.
        m_File.setFileName(m_Folder + QLatin1Char('/') + entry.path);
        if (!m_File.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            fail(QStringLiteral("could not create \"%1\"").arg(entry.path));
            return;
        }
    }

    m_Reply = get(QStringLiteral("/actions/clipboard/file?seq=%1&i=%2&offset=%3").arg(m_Sequence).arg(m_Index).arg(m_Offset));
    if (m_Reply == nullptr) {
        fail(QStringLiteral("no host to fetch the files from"));
        return;
    }
    connect(m_Reply, &QNetworkReply::finished, this, [this, reply = m_Reply.data()]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            fail(status == 409 ? QStringLiteral("the host's clipboard changed before all its files arrived")
                               : QStringLiteral("the host stopped sending the files: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray data = reply->readAll();
        const Entry& entry = m_Entries[m_Index];
        if (data.isEmpty() || data.size() > m_ChunkBytes || m_Offset + data.size() > entry.size) {
            fail(QStringLiteral("\"%1\" is not the size the host said it was").arg(entry.path));
            return;
        }
        if (m_File.write(data) != data.size()) {
            fail(QStringLiteral("could not write \"%1\" (disk full?)").arg(entry.path));
            return;
        }
        m_Offset += data.size();
        m_DoneBytes += data.size();
        reportProgress();
        fetchNextChunk();
    });
}

}
