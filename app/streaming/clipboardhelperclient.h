#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>

class NvComputer;
class QProcess;

class ClipboardHelperClient : public QObject
{
    Q_OBJECT

public:
    // What the streaming loop has to do on the host once a paste has been handed over.
    enum class PasteAction {
        None,
        SendPasteChord, // the host has the clipboard: press Ctrl+V there
        TypeText,       // the host cannot take a clipboard: type the text instead
    };

    // `shortcutMode` shares the clipboard only when copyFromHost() or pasteToHost()
    // asks. Otherwise every change is shared as it happens.
    explicit ClipboardHelperClient(NvComputer* computer, bool shortcutMode, QObject* parent = nullptr);
    ~ClipboardHelperClient() override;

    void start();
    void stop();
    void updateHostContext();

    // Thread-safe. Called from the Limelight control receive thread.
    void handleIncomingFrame(const char* data, int length);

    // Called from the SDL streaming loop on the session/main thread.
    void processPendingMessages();

    bool isRunning() const;

    bool isShortcutMode() const { return m_ShortcutMode; }

    // Shortcut mode, SDL thread. Ask for the host's clipboard to be put on the local
    // one. Call it before sending the host its copy keystroke, so that the answer
    // cannot arrive first.
    void copyFromHost();

    // Shortcut mode, SDL thread. Send the local clipboard to the host. The result
    // arrives through takePasteAction() once the host has it, or cannot have it.
    void pasteToHost();

    // Automatic mode, SDL thread. The next three only do anything where this process
    // is the one that reads and writes the local text clipboard (Wayland); elsewhere
    // the helper watches the clipboard by itself.
    //
    // The stream window got keyboard focus or was told the clipboard changed: if this
    // machine has new text, it goes to the host.
    void localClipboardMayHaveChanged();

    // Ctrl+V is about to go to the host. True when this machine has text the host
    // has not been sent yet: the key must then be held back, and the paste follows
    // through takePasteAction() as in shortcut mode.
    bool syncBeforePaste();

    // Ctrl+C or Ctrl+X just went to the host: look at its clipboard now rather than
    // at the next regular look.
    void hostMayHaveCopied();

    // Poll after processPendingMessages(). `outText` is only set for TypeText.
    PasteAction takePasteAction(QByteArray& outText);

    // True while an answer or a keystroke is due, so the loop should not sleep long.
    bool hasPendingWork() const;

private:
    static constexpr int MAX_QUEUED_HOST_FRAMES = 32;
    static constexpr int MAX_PENDING_STDIN_BYTES = 4 * 1024 * 1024;
    static constexpr int MAX_RESTART_ATTEMPTS = 3;
    static constexpr quint32 RESTART_DELAY_MS = 1000;
    // The host applies a clipboard payload when its agent gets round to it and never
    // says so. These are how long the paste keystroke waits for that: a payload sent
    // inline, and one the host has to fetch back from its own blob store first.
    static constexpr quint32 PASTE_SETTLE_MS = 250;
    static constexpr quint32 PASTE_SETTLE_OUT_OF_BAND_MS = 600;
    // A host reached over its actions API has set its clipboard by the time it answers.
    static constexpr quint32 PASTE_SETTLE_CONFIRMED_MS = 30;
    // Long enough to upload a large image over a slow link.
    static constexpr quint32 PASTE_ANSWER_TIMEOUT_MS = 15000;

    QString findHelperExecutable() const;
    bool sendCurrentConfig();
    bool startProcess();
    void scheduleRestart(const char* reason);
    void maybeRestart();
    void flushQueuedHostFrames();
    void readHelperOutput();
    void readHelperErrors();
    void logHelperStderrLine(const QByteArray& line);
    void processProtocolLine(const QByteArray& line);
    void handleProtocolError(const QString& error);
    void restartHelper(const char* reason);
    bool flushProcessInput();
    bool writeLine(const QByteArray& line);
    quint32 nextSequence();
    static bool mainProcessOwnsLocalText();
    static QByteArray readLocalText();
    bool hostUsesActionsApi() const;
    bool mirrorsLocalText() const;
    bool takeLocalChange(QByteArray& outText);
    void sendInitialLocalText();
    void setLocalText(const QByteArray& textUtf8);
    void startPaste(const QByteArray& text);
    void finishPaste(bool ok, bool outOfBand, bool confirmed, const QString& detail);
    void checkPasteTimeout();

    NvComputer* m_Computer;
    QProcess* m_Process;
    QString m_HelperPath;
    QMutex m_InboundMutex;
    QQueue<QByteArray> m_InboundFrames;
    QByteArray m_StdoutBuffer;
    QByteArray m_StderrBuffer;
    QByteArray m_StdinBuffer;
    bool m_Enabled;
    bool m_Disabled;
    bool m_StopRequested;
    bool m_HelperReady;
    quint32 m_ConfigSequence;
    quint32 m_NextSequence;
    quint32 m_NextRestartTicks;
    int m_RestartAttempts;
    int m_DroppedInboundFrames;

    bool m_ShortcutMode;
    bool m_HostRejectedClipboard;    // LiSendClipboardData() said the host has no clipboard channel
    bool m_LoggedHostRejection;
    quint32 m_PasteSequence;         // 0 = no paste waiting for the helper
    quint32 m_PasteDeadlineTicks;
    bool m_PasteFrameFailed;
    QByteArray m_PasteText;          // what to type if the clipboard cannot cross
    PasteAction m_PasteAction;       // decided, waiting for m_PasteActionTicks
    quint32 m_PasteActionTicks;

    // Automatic mode where this process owns the local text. m_LastLocalText is the
    // text (LF line ends) the local clipboard held when the two were last the same.
    bool m_InitialLocalSent;
    bool m_HaveLastLocal;
    QByteArray m_LastLocalText;
    bool m_HavePendingHostText;      // arrived while the stream window had no focus
    QByteArray m_PendingHostText;
};
