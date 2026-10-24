#pragma once

#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QPair>
#include <QQueue>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdint>
#include <functional>

class QImage;
namespace ClipboardFiles { class Transfer; }
class QMimeData;
class QNetworkAccessManager;
class QNetworkRequest;
class QNetworkReply;
class QSslError;
class QTimer;

struct ClipboardSyncHostContext
{
    QString address;
    quint16 httpsPort = 0;
    QSslCertificate serverCertificate;
    QSslCertificate clientCertificate;
    QSslKey clientPrivateKey;
    // Apollo-derived hosts, Umbra Host among them, have no clipboard frames on the
    // control stream. Their clipboard is read and set over HTTPS instead, with
    // GET and POST /actions/clipboard?type=text, and it is text only. Nothing is
    // pushed from such a host, so with one of them only the requests below work.
    bool actionsApi = false;
};

// ClipboardSync owns the local clipboard state inside the clipboard helper
// process. It implements the v1 wire format (u8 version=1, u8 kind, u32
// token, u32 length, bytes payload, little-endian) used by the Limelight
// control packet 0x5508, but it does not talk to Limelight directly. The
// helper process reports outbound frames to the main process via IPC, and the
// main process forwards them through LiSendClipboardData().
//
// Supports kind=1 (UTF-8 text), kind=2 (PNG image), and kind=3 (REF JSON
// pointing to an out-of-band blob transferred over HTTPS via the Sunshine
// /api/v1/clipboard/blob endpoints) in both directions. Payloads above the
// 65 KB single-packet cap are switched to KIND_REF transparently.
// File / other payloads are accepted on the wire but ignored.
//
// Echo suppression: when we either write a payload to the local clipboard
// (because the host pushed it) or send a payload to the host (because we
// detected a local change), we record a (hash, timestamp_ms) pair into a
// 16-entry deque with a 5 second TTL. Subsequent QClipboard::dataChanged
// notifications for the same content are dropped, mirroring the Sunshine
// GUI agent's logic so the two ends don't ping-pong.
//
// Modes: Automatic shares every clipboard change in both directions as it happens.
// Manual shares nothing by itself; the host's clipboard is only remembered, and
// crosses when copyFromHost() or pasteToHost() asks. That is what lets Ctrl+C and
// Ctrl+V stay on the host while Ctrl+Shift+C and Ctrl+Shift+V cross between machines.
//
// Threading:
//   * Construct on the GUI thread.
//   * start() / stop() must run on the GUI thread.
//   * handleIncomingFrame() may be called from any thread. It marshals the
//     payload onto the helper GUI thread via a queued metacall.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    enum Kind : uint8_t {
        KIND_TEXT = 1,
        KIND_PNG  = 2,
        KIND_REF  = 3,
    };

    enum class Mode {
        Automatic,
        Manual,
    };

    // How long copyFromHost() keeps accepting the host's answer, and how long it
    // waits for one before using what the host last reported. The host stays quiet
    // when the same thing is copied twice within its echo window, so no answer does
    // not mean nothing was copied.
    static constexpr int COPY_ARMED_MS  = 5000;
    static constexpr int COPY_SETTLE_MS = 400;
    // A host reached over the actions API is asked rather than listened to. These
    // are the moments after copyFromHost() at which it is asked: once the keystroke
    // has had time to land and be acted on, then again for an application that was
    // slow about it. A later answer replaces an earlier one only if it differs.
    static constexpr int ACTIONS_COPY_DELAYS_MS[] = { 150, 500, 1200 };
    static constexpr int ACTIONS_TIMEOUT_MS = 5000;
    // Automatic mode with such a host: how often it is asked whether its clipboard
    // changed, since it will not say. A clipboard this large is asked for less often.
    static constexpr int SYNC_TICK_MS = 1000;
    static constexpr qint64 LARGE_TEXT_BYTES = 256 * 1024;
    static constexpr int LARGE_TEXT_TICKS = 5;
    // How long the first question waits for the owner of the local clipboard to say
    // what is on it, so that this machine's clipboard is the one that wins at the start.
    static constexpr int LOCAL_STATE_WAIT_MS = 2500;
    // A stream that is still starting refuses the clipboard for a moment.
    static constexpr int PUSH_RETRIES = 8;
    static constexpr int PROBE_ATTEMPTS = 3;

    static constexpr uint8_t WIRE_VERSION = 1;
    static constexpr int     MAX_PAYLOAD  = 65535 - 10; // wire frame must fit u16 length
    // Payload size at/above which we switch from inline KIND_TEXT/KIND_PNG to
    // out-of-band blob transfer (KIND_REF). Leaves headroom under the wire cap.
    static constexpr int     INLINE_THRESHOLD = 60000;
    // Mirrors the cross-client cap (64 MiB) shared with the Android and
    // HarmonyOS clients. The service-side blob store accepts up to this much.
    static constexpr qint64  MAX_BLOB_BYTES   = 64LL * 1024 * 1024;
    static constexpr int     ECHO_TTL_MS  = 5000;
    static constexpr int     ECHO_MAX     = 16;
    // Mirror the Android client's cap (32 Mpx) so a stray full-screen capture
    // doesn't try to PNG-encode a 100 MB bitmap and stall the GUI thread.
    static constexpr qint64  MAX_IMAGE_PIXELS = 32LL * 1024 * 1024;

    static constexpr bool shouldTransferOutOfBand(qint64 payloadBytes)
    {
        return payloadBytes >= INLINE_THRESHOLD;
    }

    explicit ClipboardSync(const ClipboardSyncHostContext& hostContext = ClipboardSyncHostContext(),
                           QObject* parent = nullptr);
    ~ClipboardSync() override;

    void setHostContext(const ClipboardSyncHostContext& hostContext);

    void setMode(Mode mode);
    Mode mode() const { return m_Mode; }

    // When set, text for the local clipboard is handed to localTextRequested()
    // instead of being written here. A background process on Wayland is not allowed
    // to touch the clipboard; the window with keyboard focus is.
    void setDelegateLocalText(bool delegate);

    // Manual mode: put the host's clipboard on the local one. Applies the next
    // payload the host sends, or failing that the last one it sent.
    void copyFromHost();

    // Manual mode: send the local clipboard to the host, whether or not it changed.
    // `textUtf8` is the local text when the caller already read it; nullptr reads
    // the clipboard here (image first, then text). Answered by pasteFinished().
    void pasteToHost(quint32 requestId, const QByteArray* textUtf8);

    // With setDelegateLocalText(true): the owner of the local clipboard read it, and
    // this is its text (empty for none). In automatic mode a change goes to the host.
    void localTextChanged(const QByteArray& textUtf8);

    // Sends local files to a host with the actions API, which puts them on its
    // clipboard. Answered by pasteFinished(). Only on request: never automatically.
    void pasteFilesToHost(quint32 requestId, const QStringList& paths);

    // Start watching the local clipboard and accepting inbound payloads.
    // Must be called on the GUI thread.
    void start();

    // Disconnect signals and stop processing both directions.
    void stop();

    // Called from the control receive thread when the host pushes a 0x5508
    // payload. Buffer is only valid for the duration of the call; we copy.
    void handleIncomingFrame(const char* data, int length);

signals:
    // Emitted when local clipboard changes need to be sent to the host. The
    // owner decides how to deliver the frame, which lets the clipboard engine
    // run in a helper process without direct access to the Limelight session.
    void outboundFrame(QByteArray frame);

    // Only with setDelegateLocalText(true): put this UTF-8 text on the clipboard.
    void localTextRequested(QByteArray textUtf8);

    // Only with setDelegateLocalText(true): put these downloaded files on the clipboard.
    void localFilesRequested(QStringList paths);

    // Something the user should see: progress of a large copy, or why it did not happen.
    void notice(QString text);

    // The answer to pasteToHost(). `ok` means the payload is on its way; with
    // `outOfBand` the host still has to fetch it, so pasting has to wait longer, and
    // with `confirmed` the host has said it holds it, so pasting need not wait at all.
    void pasteFinished(quint32 requestId, bool ok, bool outOfBand, bool confirmed, QString detail);

private slots:
    void onSyncTick();

#ifdef Q_OS_MACOS
    // QClipboard::dataChanged wrapper that keeps the native pasteboard
    // changeCount baseline in sync before processing the clipboard.
    void onMacClipboardDataChanged();
#endif

    // QClipboard::dataChanged -> emitted on GUI thread.
    void onLocalClipboardChanged();

#ifdef Q_OS_MACOS
    // macOS does not reliably emit QClipboard::dataChanged for external
    // pasteboard changes while the helper is a background accessory process.
    void pollPasteboardChangeCount();
#endif

    // Queued slot used by handleIncomingFrame() to deliver to GUI thread.
    void onIncomingFrame(QByteArray frame);

private:
    // Which of the two clipboard interfaces the host has. Asked of the host itself
    // (see probeHost()) unless the host context already says.
    enum class HostKind {
        Unknown,
        Actions,   // GET and POST /actions/clipboard, text only, never pushes
        Frames,    // clipboard frames on the control stream, pushed both ways
    };

    void applyHostFrame(const QByteArray& frame);
    void armForHostFrame();
    void copyFromHostOverActions();
    void pasteToHostOverActions(quint32 requestId, const QByteArray* textUtf8);
    void probeHost();
    void resolveHostKind(HostKind kind);
    void whenHostKindKnown(const std::function<void()>& resume);
    void pollHost(bool report, bool requested);
    void pushTextToHost(const QByteArray& utf8,
                        const std::function<void(bool ok, int status, const QString& error)>& done);
    void mirrorLocalText(const QByteArray& utf8);
    void pushMirroredText();
    void pollHostFiles(bool requested);
    void applyDownloadedFiles(const QStringList& paths);
    bool fileRequest(const QString& pathAndQuery, QNetworkRequest& out) const;
    bool buildHostUrl(const QString& path, QUrl& outUrl) const;
    QNetworkRequest hostRequest(const QUrl& url) const;
    void sendLocalText(const QByteArray& utf8, bool force,
                       const std::function<void(bool ok, bool outOfBand)>& done = {});

    bool encodeFrame(uint8_t kind, const QByteArray& payload, QByteArray& outFrame) const;
    bool decodeFrame(const QByteArray& frame,
                     uint8_t& outKind,
                     QByteArray& outPayload) const;

    // Hash + TTL bookkeeping; not thread-safe, only touched from GUI thread.
    bool seenRecently(uint64_t hash);
    void recordHash(uint64_t hash);

    static uint64_t hashBytes(const QByteArray& bytes);

    bool encodeImageAsPng(const QImage& image,
                          QByteArray& outPng,
                          const char* sourceDescription) const;
    void sendClipboardPng(const QByteArray& png,
                          const QString& sourceDescription,
                          bool force = false,
                          const std::function<void(bool ok, bool outOfBand)>& done = {});
    bool extractClipboardPng(const QMimeData* mime,
                             QByteArray& outPng,
                             QString* outSourceDescription = nullptr) const;
    bool tryExtractImageBytes(const QMimeData* mime,
                              const QStringList& preferredFormats,
                              QByteArray& outPng,
                              QString* outSourceDescription) const;
    bool tryExtractImageFromUrls(const QMimeData* mime,
                                 QByteArray& outPng,
                                 QString* outSourceDescription) const;
    bool tryExtractImageFromHtml(const QMimeData* mime,
                                 QByteArray& outPng,
                                 QString* outSourceDescription) const;

    // Out-of-band blob helpers. Both run on the GUI thread; the
    // QNetworkAccessManager event-loop callbacks land back on the GUI thread.
    bool buildBlobUrl(const QString& tail, QUrl& outUrl) const;
    QNetworkAccessManager* nam();
    void uploadAndSendRef(const QByteArray& payload, const QString& mime,
                          const std::function<void(bool ok)>& done = {});
    void fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize);
    void applyInboundText(const QByteArray& payload);
    void applyInboundPng(const QByteArray& payload);

    ClipboardSyncHostContext m_HostContext;
    QNetworkAccessManager* m_Nam = nullptr;

    bool m_Active = false;
    Mode m_Mode = Mode::Automatic;
    bool m_DelegateLocalText = false;

    // Manual mode. The last frame the host sent is its clipboard as far as we know.
    QByteArray m_LatestHostFrame;
    quint32 m_HostFrameCount = 0;
    QElapsedTimer m_Clock;
    qint64 m_CopyArmedUntil = -1;
    quint32 m_CopyGeneration = 0;

    // A host with the actions API. m_SharedText is the text both clipboards are known
    // to hold (LF line ends), so that neither side's copy of it is sent back.
    HostKind m_HostKind = HostKind::Unknown;
    QList<std::function<void()>> m_WhenHostKindKnown;
    bool m_ProbeInFlight = false;
    int m_ProbeFailures = 0;
    QTimer* m_SyncTimer = nullptr;
    QByteArray m_SharedText;
    bool m_HaveShared = false;
    quint32 m_SyncGeneration = 0;   // moves whenever a push starts or ends
    int m_PushesInFlight = 0;
    bool m_LocalStateKnown = false;
    qint64 m_StartedAt = 0;
    QByteArray m_RetryText;        // the local text still owed to the host, if any
    int m_RetriesLeft = 0;
    qint64 m_LastHostTextBytes = 0;
    int m_TickCount = 0;
    bool m_WarnedReadDenied = false;
    bool m_WarnedSetDenied = false;

    // Files, with a host that has the actions API. One transfer at a time.
    QPointer<ClipboardFiles::Transfer> m_FileTransfer;
    bool m_HostTakesFiles = true;      // until it says otherwise
    quint32 m_FilesSequenceSeen = 0;   // the host listing last acted on
    QStringList m_LastUploadedFiles;   // what this machine's clipboard last sent
    QQueue<QPair<uint64_t, qint64>> m_EchoCache; // (hash, timestamp_ms)

#ifdef Q_OS_MACOS
    QTimer* m_PasteboardPollTimer = nullptr;
    int m_LastPasteboardChangeCount = -1;
#endif

    // Counter for self-writes pending QClipboard::dataChanged callbacks.
    // Some Windows clipboard hooks (e.g. IME composition managers) cause a
    // single setMimeData() to fire dataChanged twice; a bool latch would
    // absorb the first echo and leak the second back to the host. Counting
    // lets multiple pending writes coexist without ping-pong.
    int m_PendingSelfWrites = 0;
};
