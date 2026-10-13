#pragma once

#include <QByteArray>
#include <QString>

namespace ClipboardIpc {

static constexpr int PROTOCOL_VERSION = 1;
static constexpr int MAX_LINE_BYTES = 1024 * 1024;
// Text that crosses the pipe is base64 inside JSON, so it has to stay well under a line.
static constexpr int MAX_TEXT_BYTES = 512 * 1024;

enum class MessageType {
    Unknown,
    Configure,
    HostFrame,
    LocalFrame,
    Ready,
    Error,
    Stop,
    // Shortcut mode (see ClipboardSync::Mode). Main -> helper:
    CopyFromHost,   // put the host's clipboard on the local one
    PasteToHost,    // send the local clipboard to the host; `text` when the main process read it
    // Helper -> main:
    SetLocalText,   // the main process owns the local text clipboard and must set `text`
    PasteResult,    // answer to PasteToHost: `ok`, `outOfBand`, and `text` saying why not
};

struct HostConfig {
    QString address;
    quint16 httpsPort = 0;
    QByteArray serverCertPem;
    QByteArray clientCertPem;
    QByteArray clientKeyPem;
    // False: every clipboard change is shared as it happens. True: only on request.
    bool manual = false;
    // True when the helper cannot reach the local clipboard for text (a background
    // process on Wayland), so the main process reads and writes it instead.
    bool delegateText = false;
    // True for hosts whose clipboard is reached over HTTPS (/actions/clipboard, text
    // only) instead of through clipboard frames on the control stream.
    bool actionsApi = false;
};

struct Message {
    MessageType type = MessageType::Unknown;
    quint32 sequence = 0;
    HostConfig config;
    QByteArray frame;
    QString code;
    QString text;
    QByteArray clipboardText;   // UTF-8; PasteToHost (optional) and SetLocalText
    bool hasClipboardText = false;
    bool ok = false;            // PasteResult
    bool outOfBand = false;     // PasteResult: the host still has to fetch the blob
};

QByteArray encodeConfigure(quint32 sequence, const HostConfig& config);
QByteArray encodeHostFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeLocalFrame(quint32 sequence, const QByteArray& frame);
QByteArray encodeReady(quint32 sequence);
QByteArray encodeError(quint32 sequence, const QString& code, const QString& text);
QByteArray encodeStop(quint32 sequence);
QByteArray encodeCopyFromHost(quint32 sequence);
QByteArray encodePasteToHost(quint32 sequence, const QByteArray* textUtf8);
QByteArray encodeSetLocalText(quint32 sequence, const QByteArray& textUtf8);
QByteArray encodePasteResult(quint32 sequence, bool ok, bool outOfBand, const QString& detail);

bool decodeLine(const QByteArray& line, Message& outMessage, QString& outError);

}
