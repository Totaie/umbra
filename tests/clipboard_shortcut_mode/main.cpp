// Shortcut ("manual") mode of the clipboard engine: nothing crosses by itself, and
// copyFromHost() / pasteToHost() carry exactly one clipboard each way.
//
// Run with QT_QPA_PLATFORM=offscreen: that platform keeps the clipboard inside the
// process, so the test neither needs a desktop nor touches the one it runs on.

#include "streaming/clipboardipc.h"
#include "streaming/clipboardsync.h"
#include "testhostidentity.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QTextStream>
#include <QtEndian>

#include <cstring>

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

QByteArray frame(uint8_t kind, const QByteArray& payload)
{
    QByteArray out;
    out.append(static_cast<char>(ClipboardSync::WIRE_VERSION));
    out.append(static_cast<char>(kind));
    quint32 token = 0;
    out.append(reinterpret_cast<const char*>(&token), sizeof(token));
    quint32 length = qToLittleEndian<quint32>(static_cast<quint32>(payload.size()));
    out.append(reinterpret_cast<const char*>(&length), sizeof(length));
    out.append(payload);
    return out;
}

void hostSends(ClipboardSync& sync, const QByteArray& text)
{
    const QByteArray f = frame(ClipboardSync::KIND_TEXT, text);
    sync.handleIncomingFrame(f.constData(), f.size());
    spin(30);
}

struct Capture {
    QList<QByteArray> frames;
    QList<QByteArray> delegatedText;
    struct Result { quint32 id; bool ok; bool outOfBand; QString detail; };
    QList<Result> results;

    explicit Capture(ClipboardSync& sync)
    {
        QObject::connect(&sync, &ClipboardSync::outboundFrame,
                         [this](QByteArray f) { frames.append(f); });
        QObject::connect(&sync, &ClipboardSync::localTextRequested,
                         [this](QByteArray t) { delegatedText.append(t); });
        QObject::connect(&sync, &ClipboardSync::pasteFinished,
                         [this](quint32 id, bool ok, bool outOfBand, QString detail) {
                             results.append({id, ok, outOfBand, detail});
                         });
    }
};

QString localText()
{
    return QGuiApplication::clipboard()->text();
}

void setLocalText(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
    spin(30);
}

void testManualModeKeepsClipboardsApart()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    setLocalText(QStringLiteral("local"));
    require(seen.frames.isEmpty(), "manual: a local copy was sent to the host unasked");

    hostSends(sync, "host one");
    spin(ClipboardSync::COPY_SETTLE_MS + 100);
    require(localText() == QStringLiteral("local"), "manual: a host copy reached the local clipboard unasked");
}

void testCopyTakesTheHostsAnswer()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();
    setLocalText(QStringLiteral("local"));

    sync.copyFromHost();
    hostSends(sync, "fresh from host");
    require(localText() == QStringLiteral("fresh from host"), "copy: the host's answer was not applied");

    // One request, one clipboard: the next host copy stays on the host.
    hostSends(sync, "later host copy");
    spin(ClipboardSync::COPY_SETTLE_MS + 100);
    require(localText() == QStringLiteral("fresh from host"), "copy: a later host copy was applied without a request");
    require(seen.frames.isEmpty(), "copy: applying the host's clipboard echoed it back");
}

void testCopyFallsBackToWhatTheHostLastSent()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();
    setLocalText(QStringLiteral("local"));

    // Ctrl+C on the host a moment ago, then Ctrl+Shift+C on the same selection: the
    // host does not send the same thing twice.
    hostSends(sync, "copied earlier");
    sync.copyFromHost();
    spin(ClipboardSync::COPY_SETTLE_MS / 2);
    require(localText() == QStringLiteral("local"), "copy fallback: applied before giving the host time to answer");
    spin(ClipboardSync::COPY_SETTLE_MS);
    require(localText() == QStringLiteral("copied earlier"), "copy fallback: the host's last clipboard was not used");

    // A slow answer still wins over the fallback.
    hostSends(sync, "slow answer");
    require(localText() == QStringLiteral("slow answer"), "copy fallback: a late answer was ignored");
}

void testCopyWithNothingFromTheHost()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();
    setLocalText(QStringLiteral("local"));

    sync.copyFromHost();
    spin(ClipboardSync::COPY_SETTLE_MS + 100);
    require(localText() == QStringLiteral("local"), "copy: the local clipboard changed with nothing from the host");
}

void testDelegatedTextIsHandedOver()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.setDelegateLocalText(true);
    sync.start();
    setLocalText(QStringLiteral("local"));

    sync.copyFromHost();
    hostSends(sync, "for the main process");
    require(seen.delegatedText.size() == 1 && seen.delegatedText.first() == "for the main process",
            "delegate: the text was not handed to the owner of the clipboard");
    require(localText() == QStringLiteral("local"), "delegate: the helper wrote the clipboard itself");
}

void testPasteSendsGivenText()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    const QByteArray text = "pasted \xc3\xa9 text";
    sync.pasteToHost(7, &text);
    require(seen.frames.size() == 1 && seen.frames.first() == frame(ClipboardSync::KIND_TEXT, text),
            "paste: the text frame is missing or wrong");
    require(seen.results.size() == 1 && seen.results.first().id == 7 &&
            seen.results.first().ok && !seen.results.first().outOfBand,
            "paste: no success answer for the request");

    // The same text again must go again: the host's clipboard may have moved on.
    sync.pasteToHost(8, &text);
    require(seen.frames.size() == 2, "paste: a repeated paste was swallowed as an echo");
    require(seen.results.size() == 2 && seen.results.last().id == 8 && seen.results.last().ok,
            "paste: no answer for the repeated paste");
}

void testPasteReadsTheClipboard()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    setLocalText(QStringLiteral("from the clipboard"));
    sync.pasteToHost(1, nullptr);
    require(seen.frames.size() == 1 &&
            seen.frames.first() == frame(ClipboardSync::KIND_TEXT, "from the clipboard"),
            "paste: the clipboard's text was not sent");

    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(image);
    spin(30);
    sync.pasteToHost(2, nullptr);
    require(seen.frames.size() == 2 && seen.frames.last().size() > 10 &&
            static_cast<uint8_t>(seen.frames.last().at(1)) == ClipboardSync::KIND_PNG &&
            seen.frames.last().mid(10, 4) == QByteArray("\x89PNG", 4),
            "paste: the clipboard's image was not sent as PNG");
    require(seen.results.size() == 2 && seen.results.last().ok, "paste: no answer for the image");

    QGuiApplication::clipboard()->clear();
    spin(30);
    sync.pasteToHost(3, nullptr);
    require(seen.frames.size() == 2, "paste: something was sent from an empty clipboard");
    require(seen.results.size() == 3 && !seen.results.last().ok, "paste: an empty clipboard was reported as sent");
}

void testPasteOfLargeTextWithoutHostFails()
{
    // Above the inline limit the payload goes through the host's blob store. With no
    // host to upload to, the request has to be answered, and answered as failed.
    ClipboardSync sync;
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    const QByteArray text(ClipboardSync::INLINE_THRESHOLD + 1, 'x');
    sync.pasteToHost(4, &text);
    spin(50);
    require(seen.frames.isEmpty(), "large paste: a frame was sent without an upload");
    require(seen.results.size() == 1 && seen.results.first().id == 4 && !seen.results.first().ok,
            "large paste: the failed upload was not reported");
}

// A host whose clipboard is an HTTPS call: GET and POST /actions/clipboard?type=text,
// as on Apollo and Umbra Host. Enough of HTTP for QNetworkAccessManager and no more.
class FakeActionsHost : public QObject
{
public:
    QByteArray clipboard;
    int answerWith = 200;
    int gets = 0;
    QList<QByteArray> posts;

    FakeActionsHost()
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
        ClipboardSyncHostContext context;
        context.address = QStringLiteral("127.0.0.1");
        context.httpsPort = m_Server.serverPort();
        context.serverCertificate = QSslCertificate(QByteArray(kTestHostCertPem));
        context.clientCertificate = context.serverCertificate;
        context.clientPrivateKey = QSslKey(QByteArray(kTestHostKeyPem), QSsl::Rsa);
        context.actionsApi = true;
        return context;
    }

private:
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

            QByteArray answer;
            const bool rightPath = head.contains(" /actions/clipboard?type=text ");
            int status = rightPath ? answerWith : 404;
            if (status == 200 && head.startsWith("GET ")) {
                gets++;
                answer = clipboard;
            }
            else if (status == 200 && head.startsWith("POST ")) {
                posts.append(body);
                clipboard = body;
            }
            socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Length: " +
                          QByteArray::number(answer.size()) + "\r\n\r\n" + answer);
        }
        socket->setProperty("buffer", buffer);
    }

    QSslServer m_Server;
};

void testActionsHostCopy()
{
    FakeActionsHost host;
    ClipboardSync sync(host.context());
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();
    setLocalText(QStringLiteral("local"));

    host.clipboard = "from the host\r\nsecond line";
    sync.copyFromHost();
    spin(60);
    require(host.gets == 0, "actions copy: asked before the keystroke could have landed");
    spin(300);
#ifdef Q_OS_WIN
    require(localText() == QStringLiteral("from the host\r\nsecond line"), "actions copy: the host's text was not applied");
#else
    require(localText() == QStringLiteral("from the host\nsecond line"), "actions copy: the host's text was not applied with local line ends");
#endif

    // An application that only now finishes copying still gets through.
    host.clipboard = "slow application";
    spin(1300);
    require(localText() == QStringLiteral("slow application"), "actions copy: a slow copy on the host was missed");
    require(host.gets == 3, "actions copy: the host was not asked three times");
    require(seen.frames.isEmpty(), "actions copy: clipboard frames were sent to a host that has none");

    // Afterwards the host is left alone again.
    host.clipboard = "later, on the host only";
    spin(600);
    require(localText() == QStringLiteral("slow application"), "actions copy: the host's clipboard arrived without a request");
}

void testActionsHostCopyDelegated()
{
    FakeActionsHost host;
    ClipboardSync sync(host.context());
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.setDelegateLocalText(true);
    sync.start();
    setLocalText(QStringLiteral("local"));

    host.clipboard = "for the main process";
    sync.copyFromHost();
    spin(1500);
    require(seen.delegatedText.size() == 1 && seen.delegatedText.first() == "for the main process",
            "actions copy: unchanged text was handed over more than once, or not at all");
    require(localText() == QStringLiteral("local"), "actions copy: the helper wrote the clipboard itself");
}

void testActionsHostCopyRefused()
{
    FakeActionsHost host;
    host.answerWith = 401;
    host.clipboard = "not for this device";
    ClipboardSync sync(host.context());
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();
    setLocalText(QStringLiteral("local"));

    sync.copyFromHost();
    spin(1500);
    require(localText() == QStringLiteral("local"), "actions copy: a refusal changed the local clipboard");
}

void testActionsHostPaste()
{
    FakeActionsHost host;
    ClipboardSync sync(host.context());
    Capture seen(sync);
    sync.setMode(ClipboardSync::Mode::Manual);
    sync.start();

    const QByteArray text = "pasted \xc3\xa9 text";
    sync.pasteToHost(11, &text);
    spin(300);
    require(host.posts.size() == 1 && host.posts.first() == text, "actions paste: the host did not get the text");
    require(seen.results.size() == 1 && seen.results.first().id == 11 && seen.results.first().ok,
            "actions paste: no success answer");
    require(seen.frames.isEmpty(), "actions paste: a clipboard frame was sent as well");

    setLocalText(QStringLiteral("read from the clipboard"));
    sync.pasteToHost(12, nullptr);
    spin(300);
    require(host.posts.size() == 2 && host.posts.last() == "read from the clipboard",
            "actions paste: the clipboard's own text was not sent");

    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QGuiApplication::clipboard()->setImage(image);
    spin(30);
    sync.pasteToHost(13, nullptr);
    spin(300);
    require(host.posts.size() == 2, "actions paste: something was sent for an image the host cannot take");
    require(seen.results.size() == 3 && !seen.results.last().ok, "actions paste: an image was reported as pasted");

    host.answerWith = 401;
    sync.pasteToHost(14, &text);
    spin(300);
    require(seen.results.size() == 4 && seen.results.last().id == 14 && !seen.results.last().ok &&
            seen.results.last().detail.contains(QStringLiteral("permission")),
            "actions paste: a refusal was not reported as one");
}

void testAutomaticModeIsUnchanged()
{
    ClipboardSync sync;
    Capture seen(sync);
    sync.start();

    hostSends(sync, "host pushes");
    require(localText() == QStringLiteral("host pushes"), "automatic: the host's clipboard was not applied");
    require(seen.frames.isEmpty(), "automatic: the host's clipboard was echoed back");

    setLocalText(QStringLiteral("local pushes"));
    require(seen.frames.size() == 1 &&
            seen.frames.first() == frame(ClipboardSync::KIND_TEXT, "local pushes"),
            "automatic: a local copy was not sent to the host");
}

void testIpcRoundTrip()
{
    ClipboardIpc::Message m;
    QString error;

    ClipboardIpc::HostConfig config;
    config.address = QStringLiteral("192.0.2.1");
    config.httpsPort = 47984;
    config.serverCertPem = "s";
    config.clientCertPem = "c";
    config.clientKeyPem = "k";
    config.manual = true;
    config.delegateText = true;
    config.actionsApi = true;
    require(ClipboardIpc::decodeLine(ClipboardIpc::encodeConfigure(3, config), m, error) &&
            m.type == ClipboardIpc::MessageType::Configure && m.config.manual && m.config.delegateText &&
            m.config.actionsApi,
            "ipc: configure lost the mode flags");

    require(ClipboardIpc::decodeLine(ClipboardIpc::encodeCopyFromHost(4), m, error) &&
            m.type == ClipboardIpc::MessageType::CopyFromHost && m.sequence == 4,
            "ipc: copyFromHost");

    const QByteArray text = "caf\xc3\xa9\nline two";
    require(ClipboardIpc::decodeLine(ClipboardIpc::encodePasteToHost(5, &text), m, error) &&
            m.type == ClipboardIpc::MessageType::PasteToHost && m.hasClipboardText && m.clipboardText == text,
            "ipc: pasteToHost with text");
    require(ClipboardIpc::decodeLine(ClipboardIpc::encodePasteToHost(6, nullptr), m, error) &&
            m.type == ClipboardIpc::MessageType::PasteToHost && !m.hasClipboardText,
            "ipc: pasteToHost without text");

    require(ClipboardIpc::decodeLine(ClipboardIpc::encodeSetLocalText(7, text), m, error) &&
            m.type == ClipboardIpc::MessageType::SetLocalText && m.clipboardText == text,
            "ipc: setLocalText");

    require(ClipboardIpc::decodeLine(ClipboardIpc::encodePasteResult(8, true, true, QStringLiteral("why")), m, error) &&
            m.type == ClipboardIpc::MessageType::PasteResult && m.sequence == 8 && m.ok && m.outOfBand &&
            m.text == QStringLiteral("why"),
            "ipc: pasteResult");

    // A configure line from before the flags existed still means automatic.
    require(ClipboardIpc::decodeLine(R"({"type":"configure","sequence":1,"version":1,"host":{}})", m, error) &&
            !m.config.manual && !m.config.delegateText && !m.config.actionsApi,
            "ipc: a configure line without flags did not default to automatic");
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    testIpcRoundTrip();
    testManualModeKeepsClipboardsApart();
    testCopyTakesTheHostsAnswer();
    testCopyFallsBackToWhatTheHostLastSent();
    testCopyWithNothingFromTheHost();
    testDelegatedTextIsHandedOver();
    testPasteSendsGivenText();
    testPasteReadsTheClipboard();
    testPasteOfLargeTextWithoutHostFails();
    testActionsHostCopy();
    testActionsHostCopyDelegated();
    testActionsHostCopyRefused();
    testActionsHostPaste();
    testAutomaticModeIsUnchanged();

    if (g_Failures != 0) {
        return 1;
    }

    QTextStream(stdout) << "clipboard_shortcut_mode=passed\n";
    return 0;
}
