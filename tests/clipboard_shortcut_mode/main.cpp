// Shortcut ("manual") mode of the clipboard engine: nothing crosses by itself, and
// copyFromHost() / pasteToHost() carry exactly one clipboard each way.
//
// Run with QT_QPA_PLATFORM=offscreen: that platform keeps the clipboard inside the
// process, so the test neither needs a desktop nor touches the one it runs on.

#include "streaming/clipboardipc.h"
#include "streaming/clipboardsync.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
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
    require(ClipboardIpc::decodeLine(ClipboardIpc::encodeConfigure(3, config), m, error) &&
            m.type == ClipboardIpc::MessageType::Configure && m.config.manual && m.config.delegateText,
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
            !m.config.manual && !m.config.delegateText,
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
    testAutomaticModeIsUnchanged();

    if (g_Failures != 0) {
        return 1;
    }

    QTextStream(stdout) << "clipboard_shortcut_mode=passed\n";
    return 0;
}
