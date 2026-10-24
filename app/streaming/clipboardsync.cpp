#include "clipboardsync.h"
#include "clipboardfiles.h"
#include "clipboardlogging.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSharedPointer>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstring>
#include <iterator>

#ifdef Q_OS_MACOS
extern "C" int ClipboardHelperPasteboardChangeCount();
#endif

namespace {
bool isImageLikeMimeFormat(const QString& format)
{
    if (format.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive)
            || format.compare(QStringLiteral("application/x-qt-image"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    // Chromium/Edge/Firefox register Windows clipboard formats like
    // "PNG" / "image/png" / "DeviceIndependentBitmap" directly; Qt
    // surfaces those mangled as application/x-qt-windows-mime;value="...".
    // Recognize the common image-bearing variants so we still treat them
    // as candidates for extractClipboardPng().
    if (format.startsWith(QStringLiteral("application/x-qt-windows-mime;value=\""),
                          Qt::CaseInsensitive)) {
        // Extract inner value name.
        const int prefixLen = QStringLiteral("application/x-qt-windows-mime;value=\"").size();
        const int endQuote = format.indexOf(QLatin1Char('"'), prefixLen);
        if (endQuote > prefixLen) {
            const QString inner = format.mid(prefixLen, endQuote - prefixLen);
            if (inner.compare(QStringLiteral("PNG"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/jpeg"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/bmp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/webp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmap"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmapV5"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("JFIF"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("GIF"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    return false;
}

// PNG ::= 89 50 4E 47 0D 0A 1A 0A (RFC 2083 §3.1). Some Windows apps
// (older Office, IM clients) advertise "image/png" on the clipboard but
// stuff a DIB/BMP payload inside; loadFromData still decodes those by
// sniffing the actual magic, so we cannot trust the mime name alone.
// Only treat the bytes as wire-ready PNG when the magic matches.
// Line ends as they are compared and, off Windows, as they are put on the clipboard.
QByteArray withLocalLineEnds(QByteArray text)
{
    text.replace("\r\n", "\n");
    return text;
}

bool looksLikePngBytes(const QByteArray& bytes)
{
    static const unsigned char kMagic[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return bytes.size() >= 8
            && memcmp(bytes.constData(), kMagic, sizeof(kMagic)) == 0;
}
}

ClipboardSync::ClipboardSync(const ClipboardSyncHostContext& hostContext, QObject* parent)
    : QObject(parent),
      m_HostContext(hostContext)
{
    qRegisterMetaType<QByteArray>("QByteArray");
    m_Clock.start();
}

ClipboardSync::~ClipboardSync()
{
    stop();
}

void ClipboardSync::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    m_HostContext = hostContext;
    if (hostContext.actionsApi && m_Active) {
        resolveHostKind(HostKind::Actions);
    }
}

void ClipboardSync::setMode(Mode mode)
{
    if (m_Mode == mode) {
        return;
    }

    m_Mode = mode;
    m_CopyArmedUntil = -1;
    ++m_CopyGeneration;
    ClipboardLog::info("ClipboardSync: mode is now %s",
                       mode == Mode::Manual ? "manual (on request)" : "automatic");
}

void ClipboardSync::setDelegateLocalText(bool delegate)
{
    m_DelegateLocalText = delegate;
}

void ClipboardSync::copyFromHost()
{
    if (!m_Active) {
        return;
    }

    if (m_Mode == Mode::Manual) {
        armForHostFrame();
    }

    // A host with the actions API is asked; one that sends frames answers by itself.
    whenHostKindKnown([this]() {
        if (m_HostKind == HostKind::Actions) {
            copyFromHostOverActions();
        }
    });
}

void ClipboardSync::armForHostFrame()
{
    m_CopyArmedUntil = m_Clock.elapsed() + COPY_ARMED_MS;
    const quint32 generation = ++m_CopyGeneration;
    const quint32 framesBefore = m_HostFrameCount;

    QTimer::singleShot(COPY_SETTLE_MS, this, [this, generation, framesBefore]() {
        if (!m_Active || generation != m_CopyGeneration || m_CopyArmedUntil < 0) {
            // Stopped, asked again, or the host already answered.
            return;
        }
        if (framesBefore != m_HostFrameCount || m_HostKind == HostKind::Actions) {
            return;
        }
        if (m_LatestHostFrame.isEmpty()) {
            ClipboardLog::info("ClipboardSync: copy from host: the host has not sent its clipboard yet");
            return;
        }

        // Stay armed: a slow copy on the host may still answer, and should win.
        ClipboardLog::info("ClipboardSync: copy from host: using the clipboard the host last sent");
        applyHostFrame(m_LatestHostFrame);
    });
}

void ClipboardSync::copyFromHostOverActions()
{
    const quint32 generation = ++m_CopyGeneration;
    if (m_Mode == Mode::Manual) {
        // An explicit request brings the host's clipboard over even when it is what
        // was seen last; only its later attempts are compared against the first.
        m_HaveShared = false;
    }

    const int lastDelay = ACTIONS_COPY_DELAYS_MS[std::size(ACTIONS_COPY_DELAYS_MS) - 1];
    for (int delay : ACTIONS_COPY_DELAYS_MS) {
        QTimer::singleShot(delay, this, [this, generation, delay, lastDelay]() {
            if (m_Active && generation == m_CopyGeneration) {
                // Asked for with the shortcut, files come over whatever their size.
                pollHost(delay == lastDelay, m_Mode == Mode::Manual);
            }
        });
    }
}

void ClipboardSync::pollHost(bool report, bool requested)
{
    QUrl url;
    if (!buildHostUrl(QStringLiteral("/actions/clipboard?type=text"), url)) {
        return;
    }

    const quint32 generation = m_SyncGeneration;
    QNetworkReply* reply = nam()->get(hostRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, report, requested]() {
        reply->deleteLater();
        if (!m_Active) {
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 401) {
                if (!m_WarnedReadDenied) {
                    m_WarnedReadDenied = true;
                    ClipboardLog::warn("ClipboardSync: the host does not let this device read its clipboard (device permissions in the host's web UI)");
                }
            }
            else if (report && status != 403) {
                // 403 is the host not counting this device as streaming yet, or any more.
                ClipboardLog::warn("ClipboardSync: could not read the host's clipboard: %s",
                                   reply->errorString().toUtf8().constData());
            }
            return;
        }

        if (generation != m_SyncGeneration) {
            // The local clipboard went to the host while this was on its way, so
            // what it carries is what the host had before, not news.
            return;
        }

        const QByteArray raw = reply->readAll();
        m_LastHostTextBytes = raw.size();
        const QByteArray text = withLocalLineEnds(raw);
        if (text.isEmpty()) {
            // No text: perhaps files, which is the other thing the host can hand over.
            if (m_Mode == Mode::Automatic || requested) {
                pollHostFiles(requested);
            }
            else if (report && !m_HaveShared) {
                ClipboardLog::info("ClipboardSync: the host's clipboard has no text");
            }
            return;
        }
        if (m_HaveShared && text == m_SharedText) {
            return;
        }

        m_SharedText = text;
        m_HaveShared = true;
        ClipboardLog::info("ClipboardSync: received %d bytes of text from the host",
                           static_cast<int>(text.size()));
#ifdef Q_OS_WIN
        applyInboundText(raw);
#else
        // A Windows host hands over CRLF line ends, which nothing here wants.
        applyInboundText(text);
#endif
    });
}

void ClipboardSync::pushTextToHost(const QByteArray& utf8,
                                   const std::function<void(bool ok, int status, const QString& error)>& done)
{
    QUrl url;
    if (!buildHostUrl(QStringLiteral("/actions/clipboard?type=text"), url)) {
        if (done) {
            done(false, 0, QStringLiteral("no host to send the clipboard to"));
        }
        return;
    }

    // Shared from this moment, not from the host's answer: the answer to a poll
    // that crosses this on the wire must not bring the old text back, and a second
    // notice of the same copy must not send it twice.
    ++m_SyncGeneration;
    ++m_PushesInFlight;
    m_SharedText = withLocalLineEnds(utf8);
    m_HaveShared = true;

    QNetworkRequest req = hostRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/plain; charset=utf-8"));

    QNetworkReply* reply = nam()->post(req, utf8);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done, size = utf8.size()]() {
        reply->deleteLater();
        ++m_SyncGeneration;
        --m_PushesInFlight;

        const bool ok = reply->error() == QNetworkReply::NoError;
        if (ok) {
            // The host sets its clipboard before it answers, so this is on it now.
            ClipboardLog::info("ClipboardSync: sent %d bytes of text to the host",
                               static_cast<int>(size));
        }
        else if (m_PushesInFlight == 0) {
            // The host does not have it after all; let the next poll say what it has.
            m_HaveShared = false;
        }
        if (done) {
            done(ok, reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->errorString());
        }
    });
}

void ClipboardSync::mirrorLocalText(const QByteArray& utf8)
{
    if (utf8.isEmpty()) {
        return;
    }
    if (m_HaveShared && withLocalLineEnds(utf8) == m_SharedText) {
        // Already what the host has: its own text coming back, or the same copy again.
        return;
    }

    m_RetryText = utf8;
    m_RetriesLeft = PUSH_RETRIES;
    pushMirroredText();
}

void ClipboardSync::pushMirroredText()
{
    const QByteArray utf8 = m_RetryText;
    pushTextToHost(utf8, [this, utf8](bool ok, int status, const QString& error) {
        if (m_RetryText != utf8) {
            // A newer copy took its place.
            return;
        }
        if (ok) {
            m_RetryText.clear();
            return;
        }
        if (status == 401) {
            if (!m_WarnedSetDenied) {
                m_WarnedSetDenied = true;
                ClipboardLog::warn("ClipboardSync: the host does not let this device set its clipboard (device permissions in the host's web UI)");
            }
            m_RetryText.clear();
            return;
        }
        // Most often the stream is still starting and the host does not count this
        // device as streaming yet. The tick tries again.
        if (--m_RetriesLeft <= 0) {
            ClipboardLog::warn("ClipboardSync: could not send the clipboard to the host: %s",
                               error.toUtf8().constData());
            m_RetryText.clear();
        }
    });
}

bool ClipboardSync::fileRequest(const QString& pathAndQuery, QNetworkRequest& out) const
{
    QUrl url;
    if (!buildHostUrl(pathAndQuery, url)) {
        return false;
    }
    out = hostRequest(url);
    return true;
}

void ClipboardSync::pollHostFiles(bool requested)
{
    if (!m_HostTakesFiles || m_HostKind != HostKind::Actions || m_FileTransfer != nullptr) {
        return;
    }

    QNetworkRequest req;
    if (!fileRequest(QStringLiteral("/actions/clipboard?type=files"), req)) {
        return;
    }
    QNetworkReply* reply = nam()->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, requested]() {
        reply->deleteLater();
        if (!m_Active || m_FileTransfer != nullptr) {
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (status == 400 || status == 404 || status == 501) {
                // A host from before files, or one that cannot do them: stop asking.
                m_HostTakesFiles = false;
            }
            return;
        }

        const QJsonObject listing = QJsonDocument::fromJson(reply->read(4 * 1024 * 1024)).object();
        const quint32 sequence = static_cast<quint32>(listing.value(QStringLiteral("seq")).toDouble(0));
        const QJsonArray files = listing.value(QStringLiteral("files")).toArray();
        const bool truncated = listing.value(QStringLiteral("truncated")).toBool(false);
        if ((files.isEmpty() && !truncated) || (sequence == m_FilesSequenceSeen && !requested)) {
            return;
        }
        m_FilesSequenceSeen = sequence;

        if (truncated) {
            emit notice(QStringLiteral("The host copied more files than can come across (over %1 files or %2 GB)")
                        .arg(ClipboardFiles::MAX_ENTRIES).arg(ClipboardFiles::MAX_TOTAL_BYTES >> 30));
            return;
        }

        QList<ClipboardFiles::Entry> entries;
        qint64 total = 0;
        for (const QJsonValue& v : files) {
            const QJsonObject f = v.toObject();
            ClipboardFiles::Entry e;
            e.path = f.value(QStringLiteral("p")).toString();
            e.size = static_cast<qint64>(f.value(QStringLiteral("s")).toDouble(-1));
            e.directory = f.value(QStringLiteral("d")).toBool(false);
            total += qMax<qint64>(0, e.size);
            entries.append(e);
        }
        if (!requested && total > ClipboardFiles::AUTO_DOWNLOAD_BYTES) {
            emit notice(QStringLiteral("The host copied %1 GB of files, more than is brought over by itself (%2 GB)")
                        .arg(total / double(1LL << 30), 0, 'f', 1).arg(ClipboardFiles::AUTO_DOWNLOAD_BYTES >> 30));
            return;
        }

        ClipboardLog::info("ClipboardSync: fetching %d files and folders from the host", static_cast<int>(entries.size()));
        m_FileTransfer = new ClipboardFiles::Transfer(nam(), [this](const QString& path, QNetworkRequest& out) {
            return fileRequest(path, out);
        }, this);
        connect(m_FileTransfer, &ClipboardFiles::Transfer::progress, this, &ClipboardSync::notice);
        connect(m_FileTransfer, &ClipboardFiles::Transfer::finished, this,
                [this, transfer = m_FileTransfer.data()](bool ok, QString detail, QStringList paths) {
            transfer->deleteLater();
            if (!ok) {
                emit notice(QStringLiteral("Files from the host did not come across: %1").arg(detail));
                return;
            }
            applyDownloadedFiles(paths);
        });
        m_FileTransfer->download(sequence, entries);
    });
}

void ClipboardSync::applyDownloadedFiles(const QStringList& paths)
{
    // Pasting these straight back to the host needs no trip: it has them already.
    m_LastUploadedFiles = paths;

    if (m_DelegateLocalText) {
        emit localFilesRequested(paths);
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }
    QList<QUrl> urls;
    for (const QString& p : paths) {
        urls.append(QUrl::fromLocalFile(p));
    }
    QMimeData* mime = new QMimeData();
    mime->setUrls(urls);
    ++m_PendingSelfWrites;
    cb->setMimeData(mime);
    ClipboardLog::info("ClipboardSync: %d copied items from the host are on the clipboard", static_cast<int>(paths.size()));
}

void ClipboardSync::pasteFilesToHost(quint32 requestId, const QStringList& paths)
{
    auto finish = [this, requestId](bool ok, const QString& detail) {
        emit pasteFinished(requestId, ok, false, ok, detail);
    };

    if (!m_Active || m_HostKind != HostKind::Actions || !m_HostTakesFiles) {
        finish(false, QStringLiteral("this host cannot take files"));
        return;
    }
    if (paths == m_LastUploadedFiles) {
        // Already on the host's clipboard (or came from it), unless it copied something since.
        finish(true, QString());
        return;
    }
    if (m_FileTransfer != nullptr) {
        // What this machine pastes now matters more than what it was fetching.
        m_FileTransfer->cancel();
        m_FileTransfer->deleteLater();
        m_FileTransfer = nullptr;
    }

    m_FileTransfer = new ClipboardFiles::Transfer(nam(), [this](const QString& path, QNetworkRequest& out) {
        return fileRequest(path, out);
    }, this);
    connect(m_FileTransfer, &ClipboardFiles::Transfer::progress, this, &ClipboardSync::notice);
    connect(m_FileTransfer, &ClipboardFiles::Transfer::finished, this,
            [this, transfer = m_FileTransfer.data(), paths, finish](bool ok, QString detail, QStringList) {
        transfer->deleteLater();
        if (ok) {
            m_LastUploadedFiles = paths;
            ClipboardLog::info("ClipboardSync: %d copied items are on the host's clipboard", static_cast<int>(paths.size()));
        }
        else {
            emit notice(QStringLiteral("The files did not go to the host: %1").arg(detail));
        }
        finish(ok, detail);
    });
    m_FileTransfer->upload(paths);
}

void ClipboardSync::localTextChanged(const QByteArray& utf8)
{
    m_LocalStateKnown = true;
    if (!m_Active || m_Mode != Mode::Automatic) {
        return;
    }

    whenHostKindKnown([this, utf8]() {
        if (m_HostKind == HostKind::Actions) {
            mirrorLocalText(utf8);
        }
        else {
            sendLocalText(utf8, false);
        }
    });
}

void ClipboardSync::onSyncTick()
{
    if (!m_Active) {
        return;
    }
    if (m_HostKind == HostKind::Unknown) {
        probeHost();
        return;
    }
    if (m_HostKind != HostKind::Actions) {
        // A host that sends frames tells us; there is nothing to ask.
        m_SyncTimer->stop();
        return;
    }
    if (m_Mode != Mode::Automatic || m_PushesInFlight != 0) {
        return;
    }

    if (!m_LocalStateKnown) {
        if (m_DelegateLocalText) {
            // The owner of the local clipboard has not said what is on it yet. Asking
            // the host first would let the host's clipboard win at the start.
            if (m_Clock.elapsed() - m_StartedAt < LOCAL_STATE_WAIT_MS) {
                return;
            }
            m_LocalStateKnown = true;
        }
        else {
            m_LocalStateKnown = true;
            QClipboard* cb = QGuiApplication::clipboard();
            const QByteArray local = cb != nullptr ? cb->text().toUtf8() : QByteArray();
            if (!local.isEmpty()) {
                // What this machine has goes first.
                mirrorLocalText(local);
                return;
            }
        }
    }

    if (!m_RetryText.isEmpty()) {
        pushMirroredText();
        return;
    }

    // Reading megabytes back every second to learn that nothing changed is waste.
    if (m_LastHostTextBytes > LARGE_TEXT_BYTES && (++m_TickCount % LARGE_TEXT_TICKS) != 0) {
        return;
    }
    pollHost(false, false);
}

void ClipboardSync::probeHost()
{
    if (m_ProbeInFlight) {
        return;
    }

    QUrl url;
    if (!buildHostUrl(QStringLiteral("/actions/clipboard?type=text"), url)) {
        // Nothing to ask over HTTPS at all, so frames it is.
        resolveHostKind(HostKind::Frames);
        return;
    }

    m_ProbeInFlight = true;
    QNetworkReply* reply = nam()->get(hostRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_ProbeInFlight = false;
        if (m_HostKind != HostKind::Unknown) {
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) {
            resolveHostKind(HostKind::Frames);
        }
        else if (status != 0) {
            // Whatever it said - here is the text, not allowed, not streaming - it
            // knows the question.
            resolveHostKind(HostKind::Actions);
        }
        else if (++m_ProbeFailures >= PROBE_ATTEMPTS) {
            ClipboardLog::info("ClipboardSync: the host did not answer over HTTPS (%s)",
                               reply->errorString().toUtf8().constData());
            resolveHostKind(HostKind::Frames);
        }
    });
}

void ClipboardSync::resolveHostKind(HostKind kind)
{
    if (m_HostKind == kind) {
        return;
    }

    m_HostKind = kind;
    ClipboardLog::info("ClipboardSync: the host's clipboard is reached %s",
                       kind == HostKind::Actions ? "over its actions API (text only)"
                                                 : "through clipboard frames");

    const auto waiting = std::move(m_WhenHostKindKnown);
    m_WhenHostKindKnown.clear();
    for (const auto& resume : waiting) {
        resume();
    }
}

void ClipboardSync::whenHostKindKnown(const std::function<void()>& resume)
{
    if (m_HostKind != HostKind::Unknown) {
        resume();
        return;
    }

    m_WhenHostKindKnown.append(resume);
    probeHost();
}

void ClipboardSync::pasteToHostOverActions(quint32 requestId, const QByteArray* textUtf8)
{
    auto finish = [this, requestId](bool ok, const QString& detail) {
        emit pasteFinished(requestId, ok, false, ok, detail);
    };

    QByteArray utf8;
    if (textUtf8 != nullptr) {
        utf8 = *textUtf8;
    }
    else {
        QClipboard* cb = QGuiApplication::clipboard();
        const QMimeData* mime = cb != nullptr ? cb->mimeData() : nullptr;
        // Copied files go as files; their names as text would be no use to anyone.
        const QStringList files = mime != nullptr && mime->hasUrls() ? ClipboardFiles::localFilesFromUrls(mime->urls()) : QStringList();
        if (!files.isEmpty()) {
            pasteFilesToHost(requestId, files);
            return;
        }
        utf8 = cb != nullptr ? cb->text().toUtf8() : QByteArray();
    }
    if (utf8.isEmpty()) {
        // Whatever else is on the clipboard, an image say, this host cannot take.
        finish(false, QStringLiteral("the local clipboard has no text, and this host takes text only"));
        return;
    }

    if (m_Mode == Mode::Automatic && m_HaveShared && m_PushesInFlight == 0 &&
            withLocalLineEnds(utf8) == m_SharedText) {
        // The host has had this since the clipboard last changed.
        finish(true, QString());
        return;
    }

    m_RetryText.clear();
    pushTextToHost(utf8, [finish](bool ok, int status, const QString& error) {
        if (ok) {
            finish(true, QString());
        }
        else if (status == 401) {
            finish(false, QStringLiteral("the host does not let this device set its clipboard (device permissions in the host's web UI)"));
        }
        else if (status == 403) {
            finish(false, QStringLiteral("the host does not see this device as streaming"));
        }
        else {
            finish(false, QStringLiteral("the host did not take the clipboard: %1").arg(error));
        }
    });
}

void ClipboardSync::pasteToHost(quint32 requestId, const QByteArray* textUtf8)
{
    auto finish = [this, requestId](bool ok, bool outOfBand, const QString& detail) {
        emit pasteFinished(requestId, ok, outOfBand, false, detail);
    };

    if (!m_Active) {
        finish(false, false, QStringLiteral("clipboard sync is not running"));
        return;
    }

    if (m_HostKind == HostKind::Unknown) {
        // Ask again once it is known which way this host takes a clipboard.
        const bool hasText = textUtf8 != nullptr;
        const QByteArray text = hasText ? *textUtf8 : QByteArray();
        whenHostKindKnown([this, requestId, hasText, text]() {
            pasteToHost(requestId, hasText ? &text : nullptr);
        });
        return;
    }
    if (m_HostKind == HostKind::Actions) {
        pasteToHostOverActions(requestId, textUtf8);
        return;
    }

    auto done = [finish](bool ok, bool outOfBand) {
        finish(ok, outOfBand, ok ? QString() : QStringLiteral("the clipboard could not be sent to the host"));
    };

    if (textUtf8 != nullptr) {
        if (textUtf8->isEmpty()) {
            finish(false, false, QStringLiteral("the local clipboard is empty"));
            return;
        }
        sendLocalText(*textUtf8, true, done);
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    const QMimeData* mime = cb != nullptr ? cb->mimeData() : nullptr;

    // Same order as an automatic change: the picture before any label beside it.
    QByteArray png;
    QString imageSourceDescription;
    if (extractClipboardPng(mime, png, &imageSourceDescription)) {
        sendClipboardPng(png, imageSourceDescription, true, done);
        return;
    }

    const QByteArray utf8 = cb != nullptr ? cb->text().toUtf8() : QByteArray();
    if (utf8.isEmpty()) {
        finish(false, false, QStringLiteral("the local clipboard has no text or image"));
        return;
    }
    sendLocalText(utf8, true, done);
}

void ClipboardSync::start()
{
    if (m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        ClipboardLog::warn("ClipboardSync: no QClipboard available; sync disabled");
        return;
    }

#ifdef Q_OS_MACOS
    m_LastPasteboardChangeCount = ClipboardHelperPasteboardChangeCount();
    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onMacClipboardDataChanged,
            Qt::UniqueConnection);
    if (m_PasteboardPollTimer == nullptr) {
        m_PasteboardPollTimer = new QTimer(this);
        connect(m_PasteboardPollTimer, &QTimer::timeout,
                this, &ClipboardSync::pollPasteboardChangeCount);
    }
    m_PasteboardPollTimer->start(500);
#else
    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onLocalClipboardChanged,
            Qt::UniqueConnection);
#endif

    m_Active = true;
    ClipboardLog::info("ClipboardSync: started (text + PNG, bidirectional)");

    ClipboardFiles::sweepDownloads();
    m_HostKind = HostKind::Unknown;
    m_ProbeFailures = 0;
    m_LocalStateKnown = false;
    m_HaveShared = false;
    m_StartedAt = m_Clock.elapsed();
    if (m_SyncTimer == nullptr) {
        m_SyncTimer = new QTimer(this);
        connect(m_SyncTimer, &QTimer::timeout, this, &ClipboardSync::onSyncTick);
    }
    m_SyncTimer->start(SYNC_TICK_MS);
    if (m_HostContext.actionsApi) {
        resolveHostKind(HostKind::Actions);
    }
    else {
        probeHost();
    }
}

void ClipboardSync::stop()
{
    if (!m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb != nullptr) {
#ifdef Q_OS_MACOS
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onMacClipboardDataChanged);
#else
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onLocalClipboardChanged);
#endif
    }

    m_EchoCache.clear();
    m_PendingSelfWrites = 0;
    if (m_SyncTimer != nullptr) {
        m_SyncTimer->stop();
    }
    if (m_FileTransfer != nullptr) {
        m_FileTransfer->cancel();
        m_FileTransfer->deleteLater();
        m_FileTransfer = nullptr;
    }
    m_WhenHostKindKnown.clear();
    m_RetryText.clear();
#ifdef Q_OS_MACOS
    if (m_PasteboardPollTimer != nullptr) {
        m_PasteboardPollTimer->stop();
    }
    m_LastPasteboardChangeCount = -1;
#endif
    m_Active = false;

    ClipboardLog::info("ClipboardSync: stopped");
}

void ClipboardSync::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    // Copy out of the recv-thread buffer and marshal to the GUI thread.
    QByteArray frame(data, length);
    QMetaObject::invokeMethod(this, "onIncomingFrame", Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame));
}

#ifdef Q_OS_MACOS
void ClipboardSync::onMacClipboardDataChanged()
{
    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount >= 0) {
        m_LastPasteboardChangeCount = changeCount;
    }

    onLocalClipboardChanged();
}

void ClipboardSync::pollPasteboardChangeCount()
{
    if (!m_Active) {
        return;
    }

    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount < 0) {
        return;
    }

    if (m_LastPasteboardChangeCount < 0) {
        m_LastPasteboardChangeCount = changeCount;
        return;
    }

    if (changeCount == m_LastPasteboardChangeCount) {
        return;
    }

    m_LastPasteboardChangeCount = changeCount;
    onLocalClipboardChanged();
}
#endif

void ClipboardSync::onIncomingFrame(QByteArray frame)
{
    if (!m_Active) {
        return;
    }

    if (m_Mode == Mode::Manual) {
        // Remember it either way, so a later request has something to use.
        m_LatestHostFrame = frame;
        ++m_HostFrameCount;

        if (m_CopyArmedUntil < 0 || m_Clock.elapsed() > m_CopyArmedUntil) {
            m_CopyArmedUntil = -1;
            return;
        }
        m_CopyArmedUntil = -1;
        ClipboardLog::info("ClipboardSync: copy from host: received the host's clipboard");
    }

    applyHostFrame(frame);
}

void ClipboardSync::applyHostFrame(const QByteArray& frame)
{
    uint8_t kind = 0;
    QByteArray payload;
    if (!decodeFrame(frame, kind, payload)) {
        ClipboardLog::warn("ClipboardSync: discarding malformed inbound frame (%d bytes)",
                    static_cast<int>(frame.size()));
        return;
    }

    if (kind == KIND_TEXT) {
        applyInboundText(payload);
        return;
    }

    if (kind == KIND_PNG) {
        applyInboundPng(payload);
        return;
    }

    if (kind == KIND_REF) {
        // Payload is a small UTF-8 JSON descriptor: {"id":"...","mime":"...","size":N}.
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with bad JSON (%s)",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QJsonObject obj = doc.object();
        QString id = obj.value(QStringLiteral("id")).toString();
        QString mime = obj.value(QStringLiteral("mime")).toString();
        qint64 size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble(0));
        if (id.isEmpty() || id.size() > 128) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with bad id length");
            return;
        }
        if (size > MAX_BLOB_BYTES) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF, declared size %lld exceeds %lld cap",
                        static_cast<long long>(size), static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        fetchRefAndApply(id, mime, size);
        return;
    }

    // Unknown kind — ignore.
}

void ClipboardSync::applyInboundText(const QByteArray& payload)
{
    if (payload.contains('\0')) {
        ClipboardLog::warn("ClipboardSync: dropping inbound text payload with embedded NUL");
        return;
    }

    if (m_DelegateLocalText) {
        recordHash(hashBytes(payload));
        emit localTextRequested(payload);
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    // Record hash *before* writing so the dataChanged echo we're about to
    // trigger is suppressed.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    ++m_PendingSelfWrites;
    cb->setText(QString::fromUtf8(payload));
}

void ClipboardSync::applyInboundPng(const QByteArray& payload)
{
    QImage image;
    if (!image.loadFromData(payload, "PNG") || image.isNull()) {
        ClipboardLog::warn("ClipboardSync: dropping inbound PNG payload (decode failed, %d bytes)",
                    static_cast<int>(payload.size()));
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    // Hash the wire bytes (not the decoded pixels) so the echo we suppress
    // matches what we'd re-encode if QClipboard hands the image straight back.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    ++m_PendingSelfWrites;

    QMimeData* mime = new QMimeData();
    mime->setImageData(image);
    // Provide raw PNG bytes too so apps that prefer image/png over CF_DIB
    // (browsers, modern image editors) get the lossless copy verbatim.
    // Intentionally NOT setting HTML: pasting a giant base64 data: URI into
    // CF_HTML adds nothing useful (Office takes the bitmap path anyway) and
    // some Windows clipboard hooks reject oversize CF_HTML, dropping the
    // entire mime data set on the floor.
    mime->setData(QStringLiteral("image/png"), payload);
    cb->setMimeData(mime);
}

bool ClipboardSync::encodeImageAsPng(const QImage& image,
                                     QByteArray& outPng,
                                     const char* sourceDescription) const
{
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
        ClipboardLog::info("ClipboardSync: image too large from %s (%dx%d), dropping",
                    sourceDescription,
                    image.width(),
                    image.height());
        return false;
    }

    outPng.clear();
    outPng.reserve(64 * 1024);
    QBuffer buf(&outPng);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG") || outPng.isEmpty()) {
        ClipboardLog::warn("ClipboardSync: PNG encode failed for %s (%dx%d)",
                    sourceDescription,
                    image.width(),
                    image.height());
        outPng.clear();
        return false;
    }

    return true;
}

void ClipboardSync::sendClipboardPng(const QByteArray& png,
                                     const QString& sourceDescription,
                                     bool force,
                                     const std::function<void(bool ok, bool outOfBand)>& done)
{
    auto report = [&done](bool ok, bool outOfBand) {
        if (done) {
            done(ok, outOfBand);
        }
    };

    const QByteArray sourceUtf8 = sourceDescription.isEmpty()
            ? QByteArrayLiteral("unknown source")
            : sourceDescription.toUtf8();

    if (png.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: PNG payload %lld B from %s exceeds %lld B blob cap, dropping",
                    static_cast<long long>(png.size()),
                    sourceUtf8.constData(),
                    static_cast<long long>(MAX_BLOB_BYTES));
        report(false, false);
        return;
    }

    // An explicit paste always goes: the echo cache only knows what crossed
    // recently, not what the host's clipboard holds now.
    uint64_t hash = hashBytes(png);
    if (!force && seenRecently(hash)) {
        return;
    }
    recordHash(hash);

    if (shouldTransferOutOfBand(png.size())) {
        // Out-of-band path. The hash is recorded before the upload so the
        // echo we'll see when the host loops the REF back (and we fetch the
        // same bytes) is suppressed.
        uploadAndSendRef(png, QStringLiteral("image/png"), [done](bool ok) {
            if (done) {
                done(ok, true);
            }
        });
        return;
    }

    QByteArray frame;
    if (!encodeFrame(KIND_PNG, png, frame)) {
        report(false, false);
        return;
    }

    ClipboardLog::debug("ClipboardSync: outbound PNG frame queued (%d bytes from %s)",
                 static_cast<int>(frame.size()),
                 sourceUtf8.constData());
    emit outboundFrame(frame);
    report(true, false);
}

void ClipboardSync::sendLocalText(const QByteArray& utf8, bool force,
                                  const std::function<void(bool ok, bool outOfBand)>& done)
{
    auto report = [&done](bool ok, bool outOfBand) {
        if (done) {
            done(ok, outOfBand);
        }
    };

    if (utf8.isEmpty()) {
        report(false, false);
        return;
    }

    if (utf8.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: text payload %lld B exceeds %lld B blob cap, dropping",
                    static_cast<long long>(utf8.size()),
                    static_cast<long long>(MAX_BLOB_BYTES));
        report(false, false);
        return;
    }

    uint64_t hash = hashBytes(utf8);
    if (!force && seenRecently(hash)) {
        // This change was the echo of a payload the host just pushed to us.
        return;
    }
    recordHash(hash);

    if (shouldTransferOutOfBand(utf8.size())) {
        ClipboardLog::debug("ClipboardSync: uploading outbound text blob (%lld bytes)",
                     static_cast<long long>(utf8.size()));
        // Sunshine's blob endpoint intentionally accepts only a bare
        // RFC 6838 type/subtype without parameters. KIND_TEXT already
        // defines the blob bytes as UTF-8.
        uploadAndSendRef(utf8, QStringLiteral("text/plain"), [done](bool ok) {
            if (done) {
                done(ok, true);
            }
        });
        return;
    }

    QByteArray frame;
    if (!encodeFrame(KIND_TEXT, utf8, frame)) {
        report(false, false);
        return;
    }

    ClipboardLog::debug("ClipboardSync: outbound text frame queued (%d bytes)",
                 static_cast<int>(frame.size()));
    emit outboundFrame(frame);
    report(true, false);
}

bool ClipboardSync::tryExtractImageBytes(const QMimeData* mime,
                                         const QStringList& preferredFormats,
                                         QByteArray& outPng,
                                         QString* outSourceDescription) const
{
    if (mime == nullptr) {
        return false;
    }

    QStringList candidateFormats = preferredFormats;
    for (const QString& format : mime->formats()) {
        if (!isImageLikeMimeFormat(format) || candidateFormats.contains(format, Qt::CaseInsensitive)) {
            continue;
        }
        candidateFormats.append(format);
    }

    for (const QString& format : candidateFormats) {
        QByteArray bytes = mime->data(format);
        if (bytes.isEmpty()) {
            continue;
        }

        QImage image;
        if (!image.loadFromData(bytes)) {
            continue;
        }

        if ((format.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""), Qt::CaseInsensitive) == 0)
                && looksLikePngBytes(bytes)) {
            const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
            if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                ClipboardLog::info("ClipboardSync: image too large from mime %s (%dx%d), dropping",
                            format.toUtf8().constData(),
                            image.width(),
                            image.height());
                return false;
            }
            outPng = bytes;
        }
        else if (!encodeImageAsPng(image, outPng, format.toUtf8().constData())) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = format;
        }
        return true;
    }

    return false;
}

bool ClipboardSync::tryExtractImageFromUrls(const QMimeData* mime,
                                            QByteArray& outPng,
                                            QString* outSourceDescription) const
{
    if (mime == nullptr || !mime->hasUrls()) {
        return false;
    }

    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        QImage image(url.toLocalFile());
        if (image.isNull()) {
            continue;
        }

        if (!encodeImageAsPng(image, outPng, "local file url")) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = QStringLiteral("url:%1").arg(url.toLocalFile());
        }
        return true;
    }

    return false;
}

bool ClipboardSync::tryExtractImageFromHtml(const QMimeData* mime,
                                            QByteArray& outPng,
                                            QString* outSourceDescription) const
{
    if (mime == nullptr || !mime->hasHtml()) {
        return false;
    }

    const QString html = mime->html();
    if (html.isEmpty()) {
        return false;
    }

    static const QRegularExpression imgSrcRegex(
        QStringLiteral("<img[^>]+src\\s*=\\s*['\"]([^'\"]+)['\"]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dataUrlRegex(
        QStringLiteral("^data:(image/[^;,]+)?;base64,(.+)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    QRegularExpressionMatchIterator it = imgSrcRegex.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString src = match.captured(1).trimmed();
        if (src.isEmpty()) {
            continue;
        }

        const QRegularExpressionMatch dataUrlMatch = dataUrlRegex.match(src);
        if (dataUrlMatch.hasMatch()) {
            QByteArray bytes = QByteArray::fromBase64(dataUrlMatch.captured(2).toUtf8());
            if (bytes.isEmpty()) {
                continue;
            }

            QImage image;
            if (!image.loadFromData(bytes)) {
                continue;
            }

            if (dataUrlMatch.captured(1).compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0) {
                const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
                if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                    ClipboardLog::info("ClipboardSync: image too large from html data URL (%dx%d), dropping",
                                image.width(),
                                image.height());
                    return false;
                }
                outPng = bytes;
            }
            else if (!encodeImageAsPng(image, outPng, "html data url")) {
                continue;
            }

            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("html:data-url");
            }
            return true;
        }

        const QUrl url(src);
        if (!url.isLocalFile()) {
            continue;
        }

        QImage image(url.toLocalFile());
        if (image.isNull()) {
            continue;
        }

        if (!encodeImageAsPng(image, outPng, "html local file")) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = QStringLiteral("html:%1").arg(url.toLocalFile());
        }
        return true;
    }

    return false;
}

bool ClipboardSync::extractClipboardPng(const QMimeData* mime,
                                        QByteArray& outPng,
                                        QString* outSourceDescription) const
{
    outPng.clear();
    if (outSourceDescription != nullptr) {
        outSourceDescription->clear();
    }

    if (mime == nullptr) {
        return false;
    }

    // Prefer raw image bytes attached by the producer (including our own
    // applyInboundPng which sets "image/png" verbatim) BEFORE falling back
    // to re-encoding mime->imageData(). This keeps echoed inbound PNGs
    // byte-identical so the FNV-1a hash matches the echo cache and we
    // don't ping-pong the same image back to the host.
    const QStringList preferredFormats {
        QStringLiteral("image/png"),
        QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""),
        QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""),
        QStringLiteral("image/tiff"),
        QStringLiteral("image/jpeg"),
        QStringLiteral("image/jpg"),
        QStringLiteral("image/bmp"),
        QStringLiteral("image/webp"),
        QStringLiteral("application/x-qt-image")
    };

    if (tryExtractImageBytes(mime, preferredFormats, outPng, outSourceDescription)) {
        return true;
    }

    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull() && encodeImageAsPng(image, outPng, "mime imageData")) {
            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("imageData");
            }
            return true;
        }
    }

    if (tryExtractImageFromUrls(mime, outPng, outSourceDescription)) {
        return true;
    }

    if (tryExtractImageFromHtml(mime, outPng, outSourceDescription)) {
        return true;
    }

    return false;
}

void ClipboardSync::onLocalClipboardChanged()
{
    if (!m_Active) {
        return;
    }

    if (m_PendingSelfWrites > 0) {
        --m_PendingSelfWrites;
        return;
    }

    if (m_Mode == Mode::Manual) {
        // Nothing leaves this machine until pasteToHost() asks.
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (m_HostKind == HostKind::Actions) {
        // Where the owner of the local clipboard reports its text itself
        // (localTextChanged()), what Qt sees here is stale. Copied files wait for
        // a paste on the host, which is when they are worth sending.
        if (!m_DelegateLocalText) {
            m_LocalStateKnown = true;
            const QMimeData* local = cb->mimeData();
            if (local != nullptr && local->hasUrls() && !ClipboardFiles::localFilesFromUrls(local->urls()).isEmpty()) {
                return;
            }
            mirrorLocalText(cb->text().toUtf8());
        }
        return;
    }

    const QMimeData* mime = cb->mimeData();

    // Image takes precedence — some applications attach a fallback text label
    // (file path, alt text) alongside the bitmap; we want the picture, not the
    // path. To match HarmonyOS' record iteration behavior more closely, try
    // multiple extraction paths rather than only mime->imageData().
    QByteArray png;
    QString imageSourceDescription;
    if (extractClipboardPng(mime, png, &imageSourceDescription)) {
        sendClipboardPng(png, imageSourceDescription);
        return;
    }

    if (mime != nullptr) {
        bool hasImageLikeHints = mime->hasImage() || mime->hasUrls() || mime->hasHtml();
        if (!hasImageLikeHints) {
            for (const QString& format : mime->formats()) {
                if (isImageLikeMimeFormat(format)) {
                    hasImageLikeHints = true;
                    break;
                }
            }
        }

        if (hasImageLikeHints) {
            const QString formatsSummary = mime->formats().join(QStringLiteral(", "));
            ClipboardLog::debug("ClipboardSync: no transferable image found in clipboard formats [%s]",
                         formatsSummary.toUtf8().constData());
        }
    }

    QString text = cb->text();
    if (text.isEmpty()) {
        return;
    }

    sendLocalText(text.toUtf8(), false);
}

bool ClipboardSync::encodeFrame(uint8_t kind, const QByteArray& payload, QByteArray& outFrame) const
{
    if (payload.size() > MAX_PAYLOAD) {
        return false;
    }

    outFrame.clear();
    outFrame.reserve(10 + payload.size());

    // u8 version
    outFrame.append(static_cast<char>(WIRE_VERSION));
    // u8 kind
    outFrame.append(static_cast<char>(kind));
    // u32 token (LE) - we don't generate tokens client-side; 0 is accepted.
    quint32 tokenLE = qToLittleEndian<quint32>(0);
    outFrame.append(reinterpret_cast<const char*>(&tokenLE), sizeof(tokenLE));
    // u32 length (LE)
    quint32 lenLE = qToLittleEndian<quint32>(static_cast<quint32>(payload.size()));
    outFrame.append(reinterpret_cast<const char*>(&lenLE), sizeof(lenLE));
    // bytes payload
    outFrame.append(payload);
    return true;
}

bool ClipboardSync::decodeFrame(const QByteArray& frame,
                                uint8_t& outKind,
                                QByteArray& outPayload) const
{
    // 1 + 1 + 4 + 4 = 10 byte header minimum.
    if (frame.size() < 10) {
        return false;
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.constData());
    uint8_t version = p[0];
    if (version != WIRE_VERSION) {
        return false;
    }

    outKind = p[1];

    quint32 lenLE = 0;
    memcpy(&lenLE, p + 6, sizeof(lenLE));
    quint32 length = qFromLittleEndian<quint32>(lenLE);

    if (length > static_cast<quint32>(frame.size() - 10)) {
        return false;
    }

    outPayload = QByteArray(reinterpret_cast<const char*>(p + 10),
                            static_cast<int>(length));
    return true;
}

bool ClipboardSync::seenRecently(uint64_t hash)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Drop stale entries off the front first.
    while (!m_EchoCache.isEmpty() && (now - m_EchoCache.front().second) > ECHO_TTL_MS) {
        m_EchoCache.removeFirst();
    }

    for (const auto& e : m_EchoCache) {
        if (e.first == hash) {
            return true;
        }
    }
    return false;
}

void ClipboardSync::recordHash(uint64_t hash)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_EchoCache.enqueue(qMakePair(hash, now));
    while (m_EchoCache.size() > ECHO_MAX) {
        m_EchoCache.removeFirst();
    }
}

uint64_t ClipboardSync::hashBytes(const QByteArray& bytes)
{
    // FNV-1a 64-bit. Cheap and good enough for echo suppression.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < bytes.size(); ++i) {
        h ^= static_cast<uint8_t>(bytes[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

QNetworkAccessManager* ClipboardSync::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        // Pin the host's self-signed cert exactly like NvHTTP does: ignore
        // SSL errors only when the offending cert matches the pinned one.
        connect(m_Nam, &QNetworkAccessManager::sslErrors, this,
                [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                    if (m_HostContext.serverCertificate.isNull()) {
                        return;
                    }
                    for (const QSslError& e : errors) {
                        if (m_HostContext.serverCertificate != e.certificate()) {
                            return;
                        }
                    }
                    reply->ignoreSslErrors(errors);
                });
    }
    return m_Nam;
}

bool ClipboardSync::buildHostUrl(const QString& path, QUrl& outUrl) const
{
    if (m_HostContext.address.isEmpty() ||
            m_HostContext.httpsPort == 0 ||
            m_HostContext.serverCertificate.isNull()) {
        return false;
    }
    QString host = m_HostContext.address;
    if (host.contains(':')) {
        host = QString("[%1]").arg(host); // bracketed IPv6
    }
    outUrl = QUrl(QString("https://%1:%2%3").arg(host).arg(m_HostContext.httpsPort).arg(path));
    return outUrl.isValid();
}

bool ClipboardSync::buildBlobUrl(const QString& tail, QUrl& outUrl) const
{
    return buildHostUrl(QStringLiteral("/api/v1/clipboard") + tail, outUrl);
}

QNetworkRequest ClipboardSync::hostRequest(const QUrl& url) const
{
    // The host only answers a paired device, which it knows by the client
    // certificate; nam() in turn only accepts the host's pinned one.
    QNetworkRequest req(url);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);
    req.setTransferTimeout(ACTIONS_TIMEOUT_MS);
    return req;
}

void ClipboardSync::uploadAndSendRef(const QByteArray& payload, const QString& mime,
                                     const std::function<void(bool ok)>& done)
{
    QUrl url;
    if (!buildBlobUrl(QStringLiteral("/blob"), url)) {
        ClipboardLog::warn("ClipboardSync: cannot upload blob (no host context)");
        if (done) {
            done(false);
        }
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    req.setRawHeader("X-Clipboard-Mime", mime.toUtf8());
    // Sunshine pins clipboard blob endpoint to its mTLS server (cert
    // required); reuse the same paired client identity nvhttp does, or
    // every fetch/upload fails with a TLS 'certificate required' alert.
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    QNetworkReply* reply = nam()->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mime, done, payloadSize = payload.size()]() {
        reply->deleteLater();
        // Every way out below but the last is a failure.
        bool sent = false;
        auto report = qScopeGuard([&done, &sent]() {
            if (done) {
                done(sent);
            }
        });
        if (reply->error() != QNetworkReply::NoError) {
            ClipboardLog::warn("ClipboardSync: blob upload failed: %s",
                        reply->errorString().toUtf8().constData());
            return;
        }
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            ClipboardLog::warn("ClipboardSync: blob upload response not JSON: %s",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QString id = doc.object().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            ClipboardLog::warn("ClipboardSync: blob upload response missing id");
            return;
        }

        QJsonObject meta;
        meta.insert(QStringLiteral("id"), id);
        meta.insert(QStringLiteral("mime"), mime);
        meta.insert(QStringLiteral("size"), static_cast<double>(payloadSize));
        QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);

        QByteArray frame;
        if (!encodeFrame(KIND_REF, json, frame)) {
            return;
        }
        ClipboardLog::debug("ClipboardSync: outbound REF frame queued (%d bytes)",
                     static_cast<int>(frame.size()));
        emit outboundFrame(frame);
        sent = true;
    });
}

void ClipboardSync::fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize)
{
    QUrl url;
    // The id comes from the host; it names a blob, it does not get to shape the URL.
    if (!buildBlobUrl(QStringLiteral("/blob/") + QString::fromLatin1(QUrl::toPercentEncoding(id)), url)) {
        ClipboardLog::warn("ClipboardSync: cannot fetch blob (no host context)");
        return;
    }

    QNetworkRequest req(url);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    QNetworkReply* reply = nam()->get(req);
    // Checked as it arrives, not after: readAll() at the end would already have held
    // whatever the host chose to send.
    connect(reply, &QNetworkReply::downloadProgress, this, [reply, advertisedSize](qint64 received, qint64) {
        if (received > MAX_BLOB_BYTES || (advertisedSize > 0 && received > advertisedSize)) {
            ClipboardLog::warn("ClipboardSync: blob from the host is larger than it said, stopping");
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, mime, advertisedSize]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            ClipboardLog::warn("ClipboardSync: blob fetch failed: %s",
                        reply->errorString().toUtf8().constData());
            return;
        }
        QByteArray bytes = reply->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        // Defense: cap actual download size in case the server lied about
        // advertised size or QNAM streamed past the declared length.
        if (bytes.size() > MAX_BLOB_BYTES) {
            ClipboardLog::warn("ClipboardSync: dropping fetched blob, actual size %lld exceeds %lld cap",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        // Hard-fail on advertised/actual mismatch when REF declared a size.
        if (advertisedSize > 0 && bytes.size() != advertisedSize) {
            ClipboardLog::warn("ClipboardSync: dropping fetched blob, size mismatch (got %lld, advertised %lld)",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(advertisedSize));
            return;
        }
        // Trust the REF descriptor's mime over Content-Type.
        if (mime == QStringLiteral("image/png")) {
            applyInboundPng(bytes);
        } else if (mime.startsWith(QStringLiteral("text/"))) {
            applyInboundText(bytes);
        } else {
            ClipboardLog::info("ClipboardSync: dropping fetched blob with unsupported mime '%s'",
                        mime.toUtf8().constData());
        }
    });
}
