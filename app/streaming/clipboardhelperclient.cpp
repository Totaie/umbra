#include "clipboardhelperclient.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "clipboardfiles.h"
#include "clipboardipc.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QProcess>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QUrl>

#ifdef Q_OS_LINUX
#include <dlfcn.h>
#endif

#include <SDL.h>

#include <Limelight.h>

ClipboardHelperClient::ClipboardHelperClient(NvComputer* computer, bool shortcutMode, QObject* parent)
    : QObject(parent),
      m_Computer(computer),
      m_Process(nullptr),
      m_Enabled(false),
      m_Disabled(false),
      m_StopRequested(false),
      m_HelperReady(false),
      m_ConfigSequence(0),
      m_NextSequence(1),
      m_NextRestartTicks(0),
      m_RestartAttempts(0),
      m_DroppedInboundFrames(0),
      m_ShortcutMode(shortcutMode),
      m_HostRejectedClipboard(false),
      m_LoggedHostRejection(false),
      m_PasteSequence(0),
      m_PasteDeadlineTicks(0),
      m_PasteFrameFailed(false),
      m_PasteAction(PasteAction::None),
      m_PasteActionTicks(0),
      m_InitialLocalSent(false),
      m_HaveLastLocal(false),
      m_HavePendingHostText(false),
      m_PasteAlwaysPresses(false)
{
}

namespace {
// Line ends as the two clipboards are compared: a Windows host returns CRLF for
// what was sent to it with LF.
QByteArray comparable(QByteArray text)
{
    text.replace("\r\n", "\n");
    return text;
}

#ifdef Q_OS_LINUX
// Files on the Wayland clipboard need SDL3's clipboard-data calls, which the SDL2
// API does not have. Umbra runs on sdl2-compat, which has SDL3 loaded already, so
// the calls are looked up in that same library; without it, files do not cross.
using Sdl3DataCallback = const void* (*)(void* userdata, const char* mimeType, size_t* size);
using Sdl3CleanupCallback = void (*)(void* userdata);
struct Sdl3Clipboard {
    bool (*hasData)(const char* mimeType) = nullptr;
    void* (*getData)(const char* mimeType, size_t* size) = nullptr;
    bool (*setData)(Sdl3DataCallback, Sdl3CleanupCallback, void*, const char* const*, size_t) = nullptr;
    void (*free)(void*) = nullptr;

    bool usable() const { return hasData && getData && setData && free; }
};

const Sdl3Clipboard& sdl3Clipboard()
{
    static const Sdl3Clipboard clipboard = []() {
        Sdl3Clipboard c;
        // RTLD_NOLOAD: only the copy already in the process, never a second one.
        void* sdl3 = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_NOLOAD);
        if (sdl3 != nullptr) {
            c.hasData = reinterpret_cast<decltype(c.hasData)>(dlsym(sdl3, "SDL_HasClipboardData"));
            c.getData = reinterpret_cast<decltype(c.getData)>(dlsym(sdl3, "SDL_GetClipboardData"));
            c.setData = reinterpret_cast<decltype(c.setData)>(dlsym(sdl3, "SDL_SetClipboardData"));
            c.free = reinterpret_cast<decltype(c.free)>(dlsym(sdl3, "SDL_free"));
        }
        return c;
    }();
    return clipboard;
}

struct OfferedFiles {
    QByteArray uriList;
    QByteArray gnomeCopiedFiles;
};

const void* offerFiles(void* userdata, const char* mimeType, size_t* size)
{
    const auto* offer = static_cast<const OfferedFiles*>(userdata);
    const QByteArray* data = qstrcmp(mimeType, "x-special/gnome-copied-files") == 0 ? &offer->gnomeCopiedFiles
                                                                                     : &offer->uriList;
    *size = static_cast<size_t>(data->size());
    return data->constData();
}

void releaseOfferedFiles(void* userdata)
{
    delete static_cast<OfferedFiles*>(userdata);
}
#endif
}

ClipboardHelperClient::~ClipboardHelperClient()
{
    stop();
}

void ClipboardHelperClient::start()
{
    if (m_Enabled) {
        return;
    }

    m_Enabled = true;
    m_Disabled = false;
    m_StopRequested = false;
    m_RestartAttempts = 0;
    m_NextRestartTicks = 0;
    m_HelperPath = findHelperExecutable();
    if (m_HelperPath.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper executable not found; clipboard sync disabled");
        m_Disabled = true;
        return;
    }

    startProcess();
}

bool ClipboardHelperClient::startProcess()
{
    if (!m_Enabled || m_Disabled || m_Process != nullptr) {
        return false;
    }

    m_Process = new QProcess(this);
    m_Process->setProgram(m_HelperPath);
    m_Process->setProcessChannelMode(QProcess::SeparateChannels);

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    // A Wayland client without keyboard focus can neither read nor set the clipboard,
    // and the helper never has a window, let alone focus. Through XWayland it can at
    // least set it, which is what brings images across from the host; text goes
    // through this process instead (see mainProcessOwnsLocalText()). The list is a
    // fallback order, so a session without XWayland still gets the helper.
    if (mainProcessOwnsLocalText() && !qEnvironmentVariableIsEmpty("DISPLAY") &&
            qEnvironmentVariableIsEmpty("UMBRA_CLIPBOARD_HELPER_PLATFORM")) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb;wayland"));
        m_Process->setProcessEnvironment(env);
    }
    else if (!qEnvironmentVariableIsEmpty("UMBRA_CLIPBOARD_HELPER_PLATFORM")) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), qEnvironmentVariable("UMBRA_CLIPBOARD_HELPER_PLATFORM"));
        m_Process->setProcessEnvironment(env);
    }
#endif
    m_Process->start();
    if (!m_Process->waitForStarted(2000)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper failed to start: %s",
                    m_Process->errorString().toUtf8().constData());
        delete m_Process;
        m_Process = nullptr;
        scheduleRestart("start failed");
        return false;
    }

    m_HelperReady = false;
    m_StdoutBuffer.clear();
    m_StderrBuffer.clear();
    m_StdinBuffer.clear();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper started: %s",
                m_HelperPath.toUtf8().constData());

    sendCurrentConfig();
    return true;
}

void ClipboardHelperClient::stop()
{
    m_Enabled = false;
    m_StopRequested = true;
    m_HelperReady = false;
    m_NextRestartTicks = 0;

    if (m_Process == nullptr) {
        return;
    }

    if (m_Process->state() != QProcess::NotRunning) {
        writeLine(ClipboardIpc::encodeStop(nextSequence()));
        flushProcessInput();
        if (m_Process != nullptr) {
            m_Process->closeWriteChannel();
        }
        if (m_Process != nullptr && !m_Process->waitForFinished(1000)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard helper did not exit cleanly; killing it");
            m_Process->kill();
            m_Process->waitForFinished(1000);
        }
    }

    if (m_Process != nullptr) {
        delete m_Process;
        m_Process = nullptr;
    }
}

void ClipboardHelperClient::updateHostContext()
{
    if (!m_Enabled || m_Disabled || m_Process == nullptr ||
            m_Process->state() == QProcess::NotRunning) {
        return;
    }

    sendCurrentConfig();
}

void ClipboardHelperClient::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    QMutexLocker locker(&m_InboundMutex);
    if (m_InboundFrames.size() >= MAX_QUEUED_HOST_FRAMES) {
        m_InboundFrames.dequeue();
        m_DroppedInboundFrames++;
    }
    m_InboundFrames.enqueue(QByteArray(data, length));
}

void ClipboardHelperClient::processPendingMessages()
{
    checkPasteTimeout();

    if (m_Process == nullptr) {
        maybeRestart();
        return;
    }

    if (m_Process->state() == QProcess::NotRunning) {
        readHelperOutput();
        readHelperErrors();
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper exited: exitCode=%d error=%s",
                    m_Process->exitCode(),
                    m_Process->errorString().toUtf8().constData());
        delete m_Process;
        m_Process = nullptr;
        m_HelperReady = false;
        m_StdinBuffer.clear();
        if (m_PasteSequence != 0) {
            finishPaste(false, false, false, QStringLiteral("the clipboard helper stopped"));
        }
        if (!m_StopRequested) {
            scheduleRestart("unexpected exit");
        }
        return;
    }

    if (!flushProcessInput()) {
        return;
    }

    m_Process->waitForReadyRead(0);
    readHelperOutput();
    readHelperErrors();
    if (m_HelperReady) {
        flushQueuedHostFrames();
    }
    if (mirrorsLocalText() && !m_InitialLocalSent && SDL_GetKeyboardFocus() != nullptr) {
        // Only with keyboard focus does this process know what the clipboard holds.
        sendInitialLocalText();
    }
}

bool ClipboardHelperClient::isRunning() const
{
    return m_Process != nullptr && m_Process->state() != QProcess::NotRunning;
}

bool ClipboardHelperClient::mainProcessOwnsLocalText()
{
    // On Wayland only the client with keyboard focus may use the clipboard. While a
    // shortcut is being pressed that is the stream window, which is SDL's and lives
    // in this process, so text is read and written here rather than in the helper.
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr && SDL_strcmp(driver, "wayland") == 0;
}

bool ClipboardHelperClient::hostUsesActionsApi() const
{
    // Umbra Host is built on Apollo, whose clipboard is an HTTPS call rather than the
    // control stream frames a Foundation Sunshine host exchanges. It is the one host
    // that names itself, so that is what there is to go by.
    return m_Computer != nullptr && !m_Computer->hostVersion.isEmpty();
}

QByteArray ClipboardHelperClient::readLocalText()
{
    QByteArray text;
    if (SDL_HasClipboardText()) {
        char* sdlText = SDL_GetClipboardText();
        if (sdlText != nullptr) {
            text = QByteArray(sdlText);
            SDL_free(sdlText);
        }
    }
    return text;
}

bool ClipboardHelperClient::mirrorsLocalText() const
{
    return !m_ShortcutMode && mainProcessOwnsLocalText() && m_HelperReady && isRunning();
}

QStringList ClipboardHelperClient::readLocalFiles()
{
#ifdef Q_OS_LINUX
    const Sdl3Clipboard& sdl3 = sdl3Clipboard();
    if (!sdl3.usable() || !sdl3.hasData("text/uri-list")) {
        return {};
    }
    size_t size = 0;
    void* data = sdl3.getData("text/uri-list", &size);
    if (data == nullptr) {
        return {};
    }
    const QStringList files = ClipboardFiles::localFilesFromUriList(
                QByteArray(static_cast<const char*>(data), static_cast<int>(qMin<size_t>(size, 4 * 1024 * 1024))));
    sdl3.free(data);
    return files;
#else
    return {};
#endif
}

ClipboardHelperClient::LocalChange ClipboardHelperClient::takeLocalChange(QByteArray& outText, QStringList& outFiles)
{
    // Copied files first: the file manager also offers their names as text, and
    // those are not what the user means to paste.
    const QStringList files = readLocalFiles();
    if (!files.isEmpty()) {
        const QString key = files.join(QLatin1Char('\n'));
        if (key == m_LastLocalFiles) {
            return LocalChange::None;
        }
        m_LastLocalFiles = key;
        outFiles = files;
        return LocalChange::Files;
    }

    const QByteArray text = readLocalText();
    if (text.isEmpty() || (m_HaveLastLocal && comparable(text) == m_LastLocalText)) {
        return LocalChange::None;
    }

    m_LastLocalText = comparable(text);
    m_HaveLastLocal = true;
    m_LastLocalFiles.clear();
    outText = text;
    return LocalChange::Text;
}

void ClipboardHelperClient::setLocalFiles(const QStringList& paths)
{
    if (SDL_GetKeyboardFocus() == nullptr) {
        // As with text: only with focus, and only if nothing newer was copied here.
        m_PendingHostFiles = paths;
        return;
    }

#ifdef Q_OS_LINUX
    const Sdl3Clipboard& sdl3 = sdl3Clipboard();
    if (sdl3.usable()) {
        auto* offer = new OfferedFiles;
        offer->uriList = ClipboardFiles::uriListFor(paths);
        offer->gnomeCopiedFiles = "copy";
        for (const QString& p : paths) {
            offer->gnomeCopiedFiles += "\n" + QUrl::fromLocalFile(p).toEncoded();
        }
        // No text type: the names as text would come straight back as a "new copy".
        static const char* const types[] = { "text/uri-list", "x-special/gnome-copied-files" };
        if (sdl3.setData(offerFiles, releaseOfferedFiles, offer, types, 2)) {
            m_LastLocalFiles = paths.join(QLatin1Char('\n'));
            m_UnsentLocalFiles.clear();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard: copied %d items from the host", static_cast<int>(paths.size()));
            return;
        }
        delete offer;
    }
#endif
    m_Notices.append(QStringLiteral("Files from the host are in %1").arg(QFileInfo(paths.first()).absolutePath()));
}

void ClipboardHelperClient::setLocalText(const QByteArray& textUtf8)
{
    if (!m_ShortcutMode && mainProcessOwnsLocalText() && SDL_GetKeyboardFocus() == nullptr) {
        // Without keyboard focus the compositor will not let us set it, and what this
        // machine copies in the meantime outranks it anyway. Decided when focus is back.
        m_PendingHostText = textUtf8;
        m_HavePendingHostText = true;
        return;
    }

    if (SDL_SetClipboardText(textUtf8.constData()) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Could not put the host's text on the clipboard: %s",
                    SDL_GetError());
        return;
    }

    m_LastLocalText = comparable(textUtf8);
    m_HaveLastLocal = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard: copied %d bytes of text from the host",
                static_cast<int>(textUtf8.size()));
}

void ClipboardHelperClient::sendInitialLocalText()
{
    // Said once per stream, text or none: until then the helper holds off asking the
    // host, so that what this machine has is what both end up with.
    m_InitialLocalSent = true;
    m_HavePendingHostText = false;

    QByteArray text = readLocalText();
    if (text.size() > ClipboardIpc::MAX_TEXT_BYTES) {
        text.clear();
    }
    if (!text.isEmpty()) {
        m_LastLocalText = comparable(text);
        m_HaveLastLocal = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard: sharing %d bytes of text with the host",
                    static_cast<int>(text.size()));
    }
    writeLine(ClipboardIpc::encodeLocalText(nextSequence(), text));
}

void ClipboardHelperClient::localClipboardMayHaveChanged()
{
    if (!mirrorsLocalText() || !m_InitialLocalSent) {
        return;
    }

    QByteArray text;
    QStringList files;
    const LocalChange change = takeLocalChange(text, files);
    if (change == LocalChange::None) {
        // Nothing new here, so what the host copied meanwhile stands.
        if (!m_PendingHostFiles.isEmpty()) {
            const QStringList pending = m_PendingHostFiles;
            m_PendingHostFiles.clear();
            setLocalFiles(pending);
        }
        else if (m_HavePendingHostText) {
            m_HavePendingHostText = false;
            setLocalText(m_PendingHostText);
        }
        return;
    }

    // What this machine has goes first.
    m_HavePendingHostText = false;
    m_PendingHostFiles.clear();
    if (change == LocalChange::Files) {
        // Kept for the next paste on the host; a copy alone sends nothing.
        m_UnsentLocalFiles = files;
        return;
    }
    m_UnsentLocalFiles.clear();
    if (text.size() > ClipboardIpc::MAX_TEXT_BYTES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard: %d bytes of text is too much to share with the host",
                    static_cast<int>(text.size()));
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard: sharing %d bytes of text with the host",
                static_cast<int>(text.size()));
    writeLine(ClipboardIpc::encodeLocalText(nextSequence(), text));
}

bool ClipboardHelperClient::syncBeforePaste()
{
    if (m_ShortcutMode || !m_HelperReady || !isRunning()) {
        return false;
    }

    if (!mainProcessOwnsLocalText()) {
        // The helper sees the clipboard itself; it says whether anything has to go
        // first, which for copied files means sending them, and Ctrl+V follows.
        m_PasteAlwaysPresses = true;
        startPaste(QByteArray());
        // The helper answers every request, a stalled upload included, in its own time.
        if (m_PasteSequence != 0) {
            m_PasteDeadlineTicks = SDL_GetTicks() + 24 * 3600 * 1000;
        }
        return true;
    }

    if (!m_InitialLocalSent) {
        return false;
    }

    QByteArray text;
    QStringList files;
    const LocalChange change = takeLocalChange(text, files);
    if (change == LocalChange::Files) {
        m_UnsentLocalFiles = files;
    }
    if (!m_UnsentLocalFiles.isEmpty() && change != LocalChange::Text) {
        m_HavePendingHostText = false;
        m_PendingHostFiles.clear();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard: sending %d copied items to the host before pasting",
                    static_cast<int>(m_UnsentLocalFiles.size()));
        m_PasteAlwaysPresses = true;
        startPasteFiles(m_UnsentLocalFiles);
        return true;
    }
    if (change != LocalChange::Text || text.size() > ClipboardIpc::MAX_TEXT_BYTES) {
        // The host has what this machine has; the key can go straight through.
        return false;
    }

    m_HavePendingHostText = false;
    m_PendingHostFiles.clear();
    m_UnsentLocalFiles.clear();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard: sharing %d bytes of text with the host before pasting",
                static_cast<int>(text.size()));
    m_PasteAlwaysPresses = true;
    startPaste(text);
    return true;
}

void ClipboardHelperClient::startPasteFiles(const QStringList& paths)
{
    m_PasteAction = PasteAction::None;
    m_PasteFrameFailed = false;
    m_PasteText.clear();
    m_PasteSequence = nextSequence();
    // An upload can take a while; its progress shows as notices meanwhile.
    m_PasteDeadlineTicks = SDL_GetTicks() + 24 * 3600 * 1000;
    if (!writeLine(ClipboardIpc::encodePasteFilesToHost(m_PasteSequence, paths))) {
        m_PasteSequence = 0;
        finishPaste(false, false, false, QStringLiteral("the clipboard helper stopped"));
    }
}

QStringList ClipboardHelperClient::takeNotices()
{
    QStringList notices;
    qSwap(notices, m_Notices);
    return notices;
}

void ClipboardHelperClient::hostMayHaveCopied()
{
    if (m_ShortcutMode || !m_HelperReady || !isRunning()) {
        return;
    }
    writeLine(ClipboardIpc::encodeCopyFromHost(nextSequence()));
}

void ClipboardHelperClient::copyFromHost()
{
    if (!m_HelperReady || !isRunning()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard: cannot copy from the host, the clipboard helper is not running");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: copying from the host");
    writeLine(ClipboardIpc::encodeCopyFromHost(nextSequence()));
}

void ClipboardHelperClient::pasteToHost()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: pasting to the host");
    startPaste(readLocalText());
}

void ClipboardHelperClient::startPaste(const QByteArray& text)
{
    // The text is read in this process when this is the one allowed to, and in any
    // case kept to be typed if the host turns out not to take a clipboard at all.
    m_PasteAction = PasteAction::None;
    m_PasteSequence = 0;
    m_PasteFrameFailed = false;
    m_PasteText = text;
    m_PasteDeadlineTicks = SDL_GetTicks() + PASTE_ANSWER_TIMEOUT_MS;

    const bool ownsText = mainProcessOwnsLocalText();
    if (m_HostRejectedClipboard || !m_HelperReady || !isRunning() ||
            (ownsText && text.size() > ClipboardIpc::MAX_TEXT_BYTES)) {
        finishPaste(false, false, false, m_HostRejectedClipboard
                    ? QStringLiteral("the host has no clipboard channel")
                    : QStringLiteral("the clipboard helper cannot take it"));
        return;
    }

    m_PasteSequence = nextSequence();
    m_PasteDeadlineTicks = SDL_GetTicks() + PASTE_ANSWER_TIMEOUT_MS;
    // Without text the helper looks at the clipboard itself, which is where an image
    // comes from on platforms that let a background process read it.
    if (!writeLine(ClipboardIpc::encodePasteToHost(m_PasteSequence,
                                                   ownsText && !text.isEmpty() ? &text : nullptr))) {
        m_PasteSequence = 0;
        finishPaste(false, false, false, QStringLiteral("the clipboard helper stopped"));
    }
}

void ClipboardHelperClient::finishPaste(bool ok, bool outOfBand, bool confirmed, const QString& detail)
{
    m_PasteSequence = 0;
    const bool alwaysPress = m_PasteAlwaysPresses;
    m_PasteAlwaysPresses = false;

    if (ok) {
        m_PasteAction = PasteAction::SendPasteChord;
        m_PasteActionTicks = SDL_GetTicks() + (confirmed ? PASTE_SETTLE_CONFIRMED_MS
                                               : outOfBand ? PASTE_SETTLE_OUT_OF_BAND_MS
                                                           : PASTE_SETTLE_MS);
        return;
    }

    if (!m_PasteText.isEmpty()) {
        // What a paste was before hosts had a clipboard channel, and still the only
        // way into a stock Sunshine or GeForce Experience host.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard: %s; typing the text instead",
                    detail.toUtf8().constData());
        m_PasteAction = PasteAction::TypeText;
        m_PasteActionTicks = SDL_GetTicks();
        return;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard: nothing pasted to the host: %s",
                detail.toUtf8().constData());
    if (alwaysPress) {
        // It was the user's Ctrl+V: the host pastes what it has.
        m_PasteAction = PasteAction::SendPasteChord;
        m_PasteActionTicks = SDL_GetTicks();
    }
}

void ClipboardHelperClient::checkPasteTimeout()
{
    if (m_PasteSequence != 0 && SDL_TICKS_PASSED(SDL_GetTicks(), m_PasteDeadlineTicks)) {
        finishPaste(false, false, false, QStringLiteral("the clipboard helper did not answer"));
    }
}

ClipboardHelperClient::PasteAction ClipboardHelperClient::takePasteAction(QByteArray& outText)
{
    if (m_PasteAction == PasteAction::None || !SDL_TICKS_PASSED(SDL_GetTicks(), m_PasteActionTicks)) {
        return PasteAction::None;
    }

    const PasteAction action = m_PasteAction;
    m_PasteAction = PasteAction::None;
    if (action == PasteAction::TypeText) {
        outText = m_PasteText;
    }
    m_PasteText.clear();
    return action;
}

bool ClipboardHelperClient::hasPendingWork() const
{
    return m_PasteSequence != 0 || m_PasteAction != PasteAction::None;
}

void ClipboardHelperClient::scheduleRestart(const char* reason)
{
    m_HelperReady = false;
    if (!m_Enabled || m_Disabled || m_StopRequested) {
        return;
    }

    if (m_RestartAttempts >= MAX_RESTART_ATTEMPTS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper disabled after %d restart attempts (%s)",
                    m_RestartAttempts,
                    reason);
        m_Disabled = true;
        return;
    }

    m_RestartAttempts++;
    m_NextRestartTicks = SDL_GetTicks() + RESTART_DELAY_MS;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper will restart after %s (attempt %d/%d)",
                reason,
                m_RestartAttempts,
                MAX_RESTART_ATTEMPTS);
}

void ClipboardHelperClient::maybeRestart()
{
    if (!m_Enabled || m_Disabled || m_StopRequested || m_HelperPath.isEmpty()) {
        return;
    }

    if (m_NextRestartTicks == 0 || SDL_TICKS_PASSED(SDL_GetTicks(), m_NextRestartTicks)) {
        m_NextRestartTicks = 0;
        startProcess();
    }
}

QString ClipboardHelperClient::findHelperExecutable() const
{
#ifdef Q_OS_WIN
    const QString helperName = QStringLiteral("moonlight-clipboard-helper.exe");
#else
    const QString helperName = QStringLiteral("moonlight-clipboard-helper");
#endif

    QString envPath = QString::fromLocal8Bit(qgetenv("UMBRA_CLIPBOARD_HELPER"));
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
        return envPath;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/release/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/debug/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../clipboard-helper/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/release/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/debug/") + helperName),
        QDir(appDir).filePath(QStringLiteral("../../clipboard-helper/") + helperName),
#ifndef QT_NO_DEBUG
        // Debug builds only: the helper is handed the client's private key, so a
        // release never runs one from whatever directory it was started in.
        QDir(QDir::currentPath()).filePath(helperName)
#endif
    };

    for (const QString& candidate : candidates) {
        const QString cleanPath = QDir::cleanPath(candidate);
        if (QFileInfo::exists(cleanPath)) {
            return cleanPath;
        }
    }

    return QString();
}

bool ClipboardHelperClient::sendCurrentConfig()
{
    ClipboardIpc::HostConfig config;
    if (m_Computer != nullptr) {
        config.address = m_Computer->activeAddress.address();
        config.httpsPort = m_Computer->activeHttpsPort;
        config.serverCertPem = m_Computer->serverCert.toPem();
    }

    IdentityManager* identity = IdentityManager::get();
    if (identity != nullptr) {
        config.clientCertPem = identity->getCertificate();
        config.clientKeyPem = identity->getPrivateKey();
    }
    config.manual = m_ShortcutMode;
    config.delegateText = mainProcessOwnsLocalText();
    config.actionsApi = hostUsesActionsApi();

    if (config.address.isEmpty() || config.httpsPort == 0 ||
            config.serverCertPem.isEmpty() ||
            config.clientCertPem.isEmpty() ||
            config.clientKeyPem.isEmpty()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "Clipboard helper config is not available yet");
        return false;
    }

    m_HelperReady = false;
    m_ConfigSequence = nextSequence();
    return writeLine(ClipboardIpc::encodeConfigure(m_ConfigSequence, config));
}

void ClipboardHelperClient::flushQueuedHostFrames()
{
    QQueue<QByteArray> frames;
    int droppedFrames = 0;
    {
        QMutexLocker locker(&m_InboundMutex);
        qSwap(frames, m_InboundFrames);
        droppedFrames = m_DroppedInboundFrames;
        m_DroppedInboundFrames = 0;
    }

    if (droppedFrames != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Dropped %d queued clipboard frames while helper was busy",
                    droppedFrames);
    }

    while (!frames.isEmpty()) {
        if (!writeLine(ClipboardIpc::encodeHostFrame(nextSequence(), frames.dequeue()))) {
            return;
        }
    }
}

void ClipboardHelperClient::readHelperOutput()
{
    if (m_Process == nullptr) {
        return;
    }

    m_StdoutBuffer += m_Process->readAllStandardOutput();
    if (m_StdoutBuffer.size() > ClipboardIpc::MAX_LINE_BYTES) {
        handleProtocolError(QStringLiteral("helper stdout line exceeded protocol limit"));
        m_StdoutBuffer.clear();
        return;
    }

    int newlineIndex = -1;
    while ((newlineIndex = m_StdoutBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_StdoutBuffer.left(newlineIndex);
        m_StdoutBuffer.remove(0, newlineIndex + 1);
        while (line.endsWith('\r')) {
            line.chop(1);
        }
        processProtocolLine(line);
    }
}

void ClipboardHelperClient::readHelperErrors()
{
    if (m_Process == nullptr) {
        return;
    }

    m_StderrBuffer += m_Process->readAllStandardError();
    if (m_StderrBuffer.size() > ClipboardIpc::MAX_LINE_BYTES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper stderr line exceeded protocol limit; discarding buffered stderr");
        m_StderrBuffer.clear();
        return;
    }

    int newlineIndex = -1;
    while ((newlineIndex = m_StderrBuffer.indexOf('\n')) >= 0) {
        QByteArray line = m_StderrBuffer.left(newlineIndex);
        m_StderrBuffer.remove(0, newlineIndex + 1);
        while (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.isEmpty()) {
            logHelperStderrLine(line);
        }
    }
}

void ClipboardHelperClient::logHelperStderrLine(const QByteArray& line)
{
    if (line.startsWith("ClipboardSync WARN:")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper: %s",
                    line.constData());
        return;
    }

    if (line.startsWith("ClipboardSync INFO:")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper: %s",
                    line.constData());
        return;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Clipboard helper: %s",
                 line.constData());
}

void ClipboardHelperClient::processProtocolLine(const QByteArray& line)
{
    ClipboardIpc::Message message;
    QString error;
    if (!ClipboardIpc::decodeLine(line, message, error)) {
        handleProtocolError(error);
        return;
    }

    if (message.type == ClipboardIpc::MessageType::Ready) {
        if (message.sequence != m_ConfigSequence) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "Clipboard helper READY for stale config sequence %u",
                         message.sequence);
            return;
        }
        m_HelperReady = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard helper is ready");
        return;
    }

    if (message.type == ClipboardIpc::MessageType::LocalFrame) {
        int rc = LiSendClipboardData(message.frame.constData(), message.frame.size());
        if (rc != 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "LiSendClipboardData(helper, %d bytes) -> %d",
                         static_cast<int>(message.frame.size()),
                         rc);
            // The frame of a paste comes just before its result, so the result
            // handler below knows not to press Ctrl+V on a clipboard that never left.
            m_PasteFrameFailed = true;
            if (rc == -2) { // no clipboard packet type on this control stream generation
                m_HostRejectedClipboard = true;
                if (!m_LoggedHostRejection) {
                    m_LoggedHostRejection = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "This host has no clipboard channel; pasting will type text instead");
                }
            }
        }
        return;
    }

    if (message.type == ClipboardIpc::MessageType::SetLocalText) {
        setLocalText(message.clipboardText);
        return;
    }

    if (message.type == ClipboardIpc::MessageType::SetLocalFiles) {
        setLocalFiles(message.paths);
        return;
    }

    if (message.type == ClipboardIpc::MessageType::Notice) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Clipboard: %s", message.text.toUtf8().constData());
        m_Notices.append(message.text);
        return;
    }

    if (message.type == ClipboardIpc::MessageType::PasteResult) {
        if (message.sequence != m_PasteSequence || m_PasteSequence == 0) {
            // An older paste that a newer one replaced.
            return;
        }
        if (message.ok && m_PasteFrameFailed) {
            finishPaste(false, false, false, QStringLiteral("the host did not take the clipboard"));
        }
        else {
            finishPaste(message.ok, message.outOfBand, message.confirmed, message.text);
        }
        return;
    }

    if (message.type == ClipboardIpc::MessageType::Error) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper error [%s]: %s",
                    message.code.toUtf8().constData(),
                    message.text.toUtf8().constData());
        return;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "Clipboard helper sent unexpected message type");
}

void ClipboardHelperClient::handleProtocolError(const QString& error)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Clipboard helper protocol error: %s",
                error.toUtf8().constData());
    restartHelper("protocol error");
}

void ClipboardHelperClient::restartHelper(const char* reason)
{
    m_HelperReady = false;
    m_StdoutBuffer.clear();
    m_StderrBuffer.clear();
    m_StdinBuffer.clear();

    if (m_Process != nullptr) {
        if (m_Process->state() != QProcess::NotRunning) {
            m_Process->kill();
            m_Process->waitForFinished(1000);
        }
        delete m_Process;
        m_Process = nullptr;
    }

    scheduleRestart(reason);
}

bool ClipboardHelperClient::flushProcessInput()
{
    if (m_Process == nullptr || m_Process->state() == QProcess::NotRunning) {
        return false;
    }

    while (!m_StdinBuffer.isEmpty()) {
        qint64 written = m_Process->write(m_StdinBuffer.constData(), m_StdinBuffer.size());
        if (written < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Clipboard helper pipe write failed: %s",
                        m_Process->errorString().toUtf8().constData());
            restartHelper("pipe write failed");
            return false;
        }

        if (written == 0) {
            break;
        }

        m_StdinBuffer.remove(0, static_cast<int>(written));
    }

    m_Process->waitForBytesWritten(0);
    return true;
}

bool ClipboardHelperClient::writeLine(const QByteArray& line)
{
    if (m_Process == nullptr || m_Process->state() == QProcess::NotRunning) {
        return false;
    }

    QByteArray out = line;
    out.append('\n');
    if (m_StdinBuffer.size() + out.size() > MAX_PENDING_STDIN_BYTES) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Clipboard helper stdin backlog exceeded protocol limit");
        restartHelper("pipe backlog exceeded");
        return false;
    }

    m_StdinBuffer.append(out);
    return flushProcessInput();
}

quint32 ClipboardHelperClient::nextSequence()
{
    if (m_NextSequence == 0) {
        m_NextSequence = 1;
    }
    return m_NextSequence++;
}
