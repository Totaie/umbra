// Files on the clipboard, between this machine and a host with the actions API.
// The fake host below speaks the same calls as umbra-host's nvhttp.cpp, and keeps
// the same rules, so what is checked here is what the client sends and what it
// does with whatever a host sends back - including a host that lies.
//
// Run with QT_QPA_PLATFORM=offscreen; downloads go under a temporary XDG_CACHE_HOME.

#include "streaming/clipboardfiles.h"
#include "streaming/clipboardsync.h"
#include "testhostidentity.h"

#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_Failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        g_Failures++;
    }
}

void spin(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

void put(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return;
    }
    f.write(data);
}

QByteArray get(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return f.readAll();
}

int modeOf(const QString& path)
{
    struct stat st {};
    return lstat(QFile::encodeName(path).constData(), &st) == 0 ? (st.st_mode & 0777) : -1;
}

struct Listed {
    QString path;
    qint64 size;
    bool dir;
    QByteArray content;   // what the host serves for it
};

class FakeHost : public QObject
{
public:
    // What the host's clipboard holds.
    QByteArray text;
    quint32 sequence = 1;
    QList<Listed> files;
    bool filesSupported = true;
    int failFileRequestsWith = 0;
    int clipboardChangesAfterChunks = -1;   // a copy on the host half-way through a fetch
    int chunksServed = 0;
    // What it was sent.
    QJsonArray begun;
    QMap<int, QByteArray> uploaded;
    int begins = 0;
    int commits = 0;
    int aborts = 0;
    bool committedOk = false;
    QString badChunk;

    FakeHost()
    {
        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(QSslCertificate(QByteArray(kTestHostCertPem)));
        config.setPrivateKey(QSslKey(QByteArray(kTestHostKeyPem), QSsl::Rsa));
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_Server.setSslConfiguration(config);
        m_Server.listen(QHostAddress::LocalHost);
        connect(&m_Server, &QTcpServer::pendingConnectionAvailable, this, [this]() {
            while (QTcpSocket* socket = m_Server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { serve(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    ClipboardSyncHostContext context() const
    {
        ClipboardSyncHostContext c;
        c.address = QStringLiteral("127.0.0.1");
        c.httpsPort = m_Server.serverPort();
        c.serverCertificate = QSslCertificate(QByteArray(kTestHostCertPem));
        c.clientCertificate = c.serverCertificate;
        c.clientPrivateKey = QSslKey(QByteArray(kTestHostKeyPem), QSsl::Rsa);
        c.actionsApi = true;
        return c;
    }

private:
    void reply(QTcpSocket* socket, int status, const QByteArray& body = {})
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\n\r\n" + body);
    }

    void serve(QTcpSocket* socket)
    {
        QByteArray buffer = socket->property("buffer").toByteArray() + socket->readAll();
        for (;;) {
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                break;
            }
            const QByteArray head = buffer.left(headerEnd);
            int length = 0;
            for (const QByteArray& line : head.split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    length = line.mid(15).trimmed().toInt();
                }
            }
            if (buffer.size() < headerEnd + 4 + length) {
                break;
            }
            const QByteArray body = buffer.mid(headerEnd + 4, length);
            buffer.remove(0, headerEnd + 4 + length);

            const QList<QByteArray> requestLine = head.left(head.indexOf('\r')).split(' ');
            const QUrl url(QString::fromLatin1(requestLine.value(1)));
            const QUrlQuery q(url);
            const QString path = url.path();
            const bool isGet = requestLine.value(0) == "GET";

            if (path == QLatin1String("/actions/clipboard") && isGet && q.queryItemValue("type") == QLatin1String("text")) {
                reply(socket, 200, text);
            }
            else if (path == QLatin1String("/actions/clipboard") && isGet && q.queryItemValue("type") == QLatin1String("files")) {
                if (!filesSupported) {
                    reply(socket, 400);
                    continue;
                }
                QJsonArray list;
                for (const Listed& f : files) {
                    list.append(QJsonObject{ { "p", f.path }, { "s", double(f.size) }, { "d", f.dir } });
                }
                reply(socket, 200, QJsonDocument(QJsonObject{ { "seq", double(sequence) }, { "truncated", false }, { "files", list } }).toJson());
            }
            else if (path == QLatin1String("/actions/clipboard/file") && isGet) {
                const int i = q.queryItemValue("i").toInt();
                const qint64 offset = q.queryItemValue("offset").toLongLong();
                if (clipboardChangesAfterChunks >= 0 && chunksServed++ >= clipboardChangesAfterChunks) {
                    sequence++;
                }
                if (failFileRequestsWith != 0 || q.queryItemValue("seq").toUInt() != sequence || i < 0 || i >= files.size()) {
                    reply(socket, failFileRequestsWith != 0 ? failFileRequestsWith : 409);
                    continue;
                }
                reply(socket, 200, files[i].content.mid(offset, 768 * 1024));
            }
            else if (path == QLatin1String("/actions/clipboard/files/begin")) {
                begins++;
                begun = QJsonDocument::fromJson(body).object().value("files").toArray();
                uploaded.clear();
                reply(socket, 200, QJsonDocument(QJsonObject{ { "id", QString(32, QLatin1Char('a')) }, { "chunk", 768 * 1024 } }).toJson());
            }
            else if (path == QLatin1String("/actions/clipboard/files/chunk")) {
                const int i = q.queryItemValue("i").toInt();
                if (q.queryItemValue("offset").toLongLong() != uploaded.value(i).size() || body.size() > 768 * 1024) {
                    badChunk = QString::fromLatin1(url.toEncoded());
                    reply(socket, 400);
                    continue;
                }
                uploaded[i] += body;
                reply(socket, 200);
            }
            else if (path == QLatin1String("/actions/clipboard/files/commit")) {
                commits++;
                committedOk = true;
                for (int i = 0; i < begun.size(); i++) {
                    const QJsonObject f = begun[i].toObject();
                    if (!f.value("d").toBool() && uploaded.value(i).size() != qint64(f.value("s").toDouble())) {
                        committedOk = false;
                    }
                }
                reply(socket, committedOk ? 200 : 400);
            }
            else if (path == QLatin1String("/actions/clipboard/files/abort")) {
                aborts++;
                reply(socket, 200);
            }
            else {
                reply(socket, 404);
            }
        }
        socket->setProperty("buffer", buffer);
    }

    QSslServer m_Server;
};

struct Seen {
    QList<QStringList> localFiles;
    QStringList notices;
    struct Result { quint32 id; bool ok; QString detail; };
    QList<Result> results;

    explicit Seen(ClipboardSync& sync)
    {
        QObject::connect(&sync, &ClipboardSync::localFilesRequested, [this](QStringList p) { localFiles.append(p); });
        QObject::connect(&sync, &ClipboardSync::notice, [this](QString n) { notices.append(n); });
        QObject::connect(&sync, &ClipboardSync::pasteFinished,
                         [this](quint32 id, bool ok, bool, bool, QString detail) { results.append({ id, ok, detail }); });
    }
};

QString downloadRoot()
{
    return qEnvironmentVariable("XDG_CACHE_HOME") + QStringLiteral("/Umbra/Pasted files");
}

QStringList everythingUnder(const QString& root)
{
    QStringList out;
    QDir dir(root);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        out.append(fi.absoluteFilePath());
        if (fi.isDir() && !fi.isSymLink()) {
            out += everythingUnder(fi.absoluteFilePath());
        }
    }
    return out;
}

void testNameRules()
{
    using ClipboardFiles::validName;
    for (const char* ok : { "file.txt", ".hidden", "console.txt", "comX", "com10", "a b", "d\xC3\xA9j\xC3\xA0 \xE6\x96\x87.txt" }) {
        require(validName(QString::fromUtf8(ok)), "names: an ordinary name was refused");
    }
    for (const char* bad : { "", ".", "..", "a/b", "a\\b", "a:b", "a*", "a?", "a\"b", "a<b", "a|b", "tab\there", "trailing.", "trailing ",
                             "con", "CON.txt", "nul .log", "com1", "LPT9.x", "conin$", "com\xC2\xB9" }) {
        require(!validName(QString::fromUtf8(bad)), "names: a name Windows or POSIX cannot hold safely was accepted");
    }
    require(!validName(QString(256, QLatin1Char('a'))), "names: an over-long name was accepted");
    require(!ClipboardFiles::validRelativePath(QStringLiteral("a/../b")), "paths: .. was accepted");
    require(!ClipboardFiles::validRelativePath(QStringLiteral("/abs")), "paths: an absolute path was accepted");
    require(!ClipboardFiles::validRelativePath(QStringLiteral("a//b")), "paths: an empty component was accepted");
}

void testUriLists()
{
    const QStringList paths { QStringLiteral("/tmp/a b/c.txt"), QStringLiteral("/tmp/déjà.txt") };
    require(ClipboardFiles::localFilesFromUriList(ClipboardFiles::uriListFor(paths)) == paths, "uri-list: paths did not survive a round trip");
    require(ClipboardFiles::localFilesFromUriList("# comment\r\nhttps://example.com/x\r\nfile:///etc/hostname\r\n") ==
            QStringList{ QStringLiteral("/etc/hostname") }, "uri-list: non-local or comment lines were taken as files");
}

void testListingForUpload(const QString& scratch)
{
    const QString src = scratch + "/src";
    QDir().mkpath(src + "/folder/inner");
    put(src + "/folder/a.txt", "aaaa");
    put(src + "/folder/inner/b.txt", "bb");
    put(src + "/top.txt", "top");
    put(scratch + "/secret.txt", "secret");
    QFile::link(scratch + "/secret.txt", src + "/folder/link-to-secret");
    QFile::link(scratch, src + "/folder/dir-link");
    mkfifo(QFile::encodeName(src + "/folder/fifo").constData(), 0600);
    QFile::link(src + "/top.txt", src + "/copied-link");

    QList<ClipboardFiles::Entry> entries;
    QString why;
    require(ClipboardFiles::listForUpload({ src + "/folder", src + "/copied-link" }, entries, &why), "listing: a plain folder could not be listed");
    QStringList listed;
    for (const auto& e : entries) {
        listed.append(e.path);
    }
    require(listed == QStringList({ "folder", "folder/a.txt", "folder/inner", "folder/inner/b.txt", "top.txt" }),
            "listing: links, a fifo, or the wrong names were included");

    put(src + "/bad:name.txt", "x");
    require(!ClipboardFiles::listForUpload({ src + "/bad:name.txt" }, entries, &why) && why.contains("bad:name.txt"),
            "listing: a name Windows cannot hold was not refused by name");
}

void testUpload(const QString& scratch)
{
    FakeHost host;
    ClipboardSync sync(host.context());
    Seen seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    const QString src = scratch + "/up";
    QDir().mkpath(src + "/dir");
    QByteArray big(1800 * 1024, 0);
    for (int i = 0; i < big.size(); i++) {
        big[i] = char(i * 7 + 3);
    }
    put(src + "/dir/big.bin", big);
    put(src + "/dir/empty", "");
    put(src + "/small.txt", "small");

    sync.pasteFilesToHost(5, { src + "/dir", src + "/small.txt" });
    require(waitFor([&]() { return !seen.results.isEmpty(); }, 10000), "upload: never answered");
    require(!seen.results.isEmpty() && seen.results.first().ok, "upload: did not succeed");
    require(host.begins == 1 && host.commits == 1 && host.committedOk && host.badChunk.isEmpty(), "upload: chunks were out of order or incomplete");
    QStringList declared;
    for (const auto& v : host.begun) {
        declared.append(v.toObject().value("p").toString());
    }
    require(declared == QStringList({ "dir", "dir/big.bin", "dir/empty", "small.txt" }), "upload: the listing sent was not the copied items");
    require(host.uploaded.value(1) == big && host.uploaded.value(3) == "small", "upload: the host did not get the bytes");

    // Pasting the same copy again does not send it again.
    sync.pasteFilesToHost(6, { src + "/dir", src + "/small.txt" });
    require(waitFor([&]() { return seen.results.size() == 2; }, 3000) && seen.results.last().ok && host.begins == 1,
            "upload: the same copy was sent twice");

    // A file that changes size between listing and sending is not sent half-done.
    FakeHost host2;
    ClipboardSync sync2(host2.context());
    Seen seen2(sync2);
    sync2.setMode(ClipboardSync::Mode::Manual);
    sync2.start();
    put(src + "/grows.bin", QByteArray(2 * 1024 * 1024, 'g'));
    sync2.pasteFilesToHost(7, { src + "/grows.bin" });
    QTimer::singleShot(0, [&]() { put(src + "/grows.bin", QByteArray(10, 'g')); });
    require(waitFor([&]() { return !seen2.results.isEmpty(); }, 10000), "upload: a changing file was never answered");
    require(!seen2.results.isEmpty() && (!seen2.results.first().ok || host2.uploaded.value(0).size() == 2 * 1024 * 1024),
            "upload: a file that shrank was reported as sent");
}

void testDownload(bool delegate)
{
    FakeHost host;
    host.sequence = 7;
    QByteArray big(1000 * 1024, 0);
    for (int i = 0; i < big.size(); i++) {
        big[i] = char(i * 13 + 1);
    }
    host.files = { { "Folder", 0, true, {} }, { "Folder/big.bin", big.size(), false, big },
                   { "Folder/empty.txt", 0, false, {} }, { "note.txt", 4, false, "note" } };

    ClipboardSync sync(host.context());
    Seen seen(sync);
    sync.setDelegateLocalText(delegate);
    sync.start();
    if (delegate) {
        sync.localTextChanged(QByteArray());
    }

    QStringList got;
    require(waitFor([&]() {
        if (delegate) {
            if (!seen.localFiles.isEmpty()) { got = seen.localFiles.first(); }
        }
        else {
            const QMimeData* m = QGuiApplication::clipboard()->mimeData();
            if (m != nullptr && m->hasUrls()) { got = ClipboardFiles::localFilesFromUrls(m->urls()); }
        }
        return !got.isEmpty();
    }, 8000), "download: the host's files never reached the clipboard");
    require(got.size() == 2, "download: wrong number of top-level items");
    if (got.size() == 2) {
        const QString folder = QFileInfo(got.first()).absolutePath();
        require(folder.startsWith(downloadRoot()), "download: written outside the download folder");
        require(get(folder + "/Folder/big.bin") == big && get(folder + "/note.txt") == "note" &&
                QFile::exists(folder + "/Folder/empty.txt"), "download: the bytes are wrong");
        require(modeOf(folder) == 0700 && modeOf(folder + "/note.txt") == 0600, "download: not private to this user");
    }
    // Asking again with the same listing does nothing.
    const int before = seen.localFiles.size();
    spin(1300);
    require(seen.localFiles.size() == before, "download: the same listing was fetched twice");
}

void testHostileListings()
{
    const QString root = downloadRoot();
    const QStringList before = everythingUnder(QDir(root).absolutePath() + "/..");

    for (const QList<Listed>& listing : QList<QList<Listed>>{
             { { "../escape.txt", 4, false, "evil" } },
             { { "dir", 0, true, {} }, { "dir/../../escape.txt", 4, false, "evil" } },
             { { "/abs.txt", 4, false, "evil" } },
             { { "dir/x.txt", 4, false, "evil" } },                       // parent never listed
             { { "con.txt", 4, false, "evil" } },
             { { "a.txt", 4, false, "evil" }, { "A.TXT", 4, false, "evil" } },
         }) {
        FakeHost host;
        host.sequence = 3;
        host.files = listing;
        ClipboardSync sync(host.context());
        Seen seen(sync);
        sync.setDelegateLocalText(true);
        sync.start();
        sync.localTextChanged(QByteArray());
        waitFor([&]() { return !seen.notices.isEmpty(); }, 4000);
        require(seen.localFiles.isEmpty(), "hostile: a bad listing ended up on the clipboard");
        require(!seen.notices.isEmpty(), "hostile: a bad listing was dropped without a word");
    }

    // More bytes than declared.
    {
        FakeHost host;
        host.sequence = 4;
        host.files = { { "liar.bin", 4, false, "more than four bytes" } };
        ClipboardSync sync(host.context());
        Seen seen(sync);
        sync.setDelegateLocalText(true);
        sync.start();
        sync.localTextChanged(QByteArray());
        waitFor([&]() { return !seen.notices.isEmpty(); }, 4000);
        require(seen.localFiles.isEmpty(), "hostile: a file longer than declared was accepted");
    }

    // The clipboard changes half-way: what arrived so far is thrown away.
    {
        FakeHost host;
        host.sequence = 5;
        host.files = { { "a.bin", 2 * 1024 * 1024, false, QByteArray(2 * 1024 * 1024, 'a') } };
        ClipboardSync sync(host.context());
        Seen seen(sync);
        sync.setDelegateLocalText(true);
        sync.start();
        host.clipboardChangesAfterChunks = 1;
        sync.localTextChanged(QByteArray());
        waitFor([&]() { return !seen.notices.isEmpty(); }, 4000);
        require(seen.localFiles.isEmpty(), "hostile: a half-fetched copy reached the clipboard");
    }

    // Too big to fetch unasked.
    {
        FakeHost host;
        host.sequence = 6;
        host.files = { { "huge.bin", ClipboardFiles::AUTO_DOWNLOAD_BYTES + 1, false, {} } };
        ClipboardSync sync(host.context());
        Seen seen(sync);
        sync.setDelegateLocalText(true);
        sync.start();
        sync.localTextChanged(QByteArray());
        waitFor([&]() { return !seen.notices.isEmpty(); }, 4000);
        require(seen.localFiles.isEmpty() && !seen.notices.isEmpty() && seen.notices.first().contains("GB"),
                "hostile: a huge copy was fetched without being asked, or without a word");
    }

    spin(200);
    // Nothing anywhere but fresh private folders was written, and failed ones are gone.
    for (const QString& path : everythingUnder(QDir(root).absolutePath() + "/..")) {
        if (!before.contains(path)) {
            require(path.startsWith(root), "hostile: something was written outside the download folder");
        }
    }
    for (const QString& path : everythingUnder(root)) {
        require(!path.contains("escape") && !path.contains("liar") && !path.contains("a.bin"),
                "hostile: a refused or broken download was left behind");
    }
}

void testHostWithoutFiles()
{
    FakeHost host;
    host.filesSupported = false;
    ClipboardSync sync(host.context());
    Seen seen(sync);
    sync.setDelegateLocalText(true);
    sync.start();
    sync.localTextChanged(QByteArray());
    spin(2500);
    require(seen.localFiles.isEmpty() && seen.notices.isEmpty(), "old host: something happened with a host that has no files");
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache.path()));
    QTemporaryDir scratch;
    QGuiApplication app(argc, argv);

    testNameRules();
    testUriLists();
    testListingForUpload(scratch.path());
    testUpload(scratch.path());
    testDownload(false);
    testDownload(true);
    testHostileListings();
    testHostWithoutFiles();

    if (g_Failures != 0) {
        return 1;
    }
    QTextStream(stdout) << "clipboard_files=passed\n";
    return 0;
}
