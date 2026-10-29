#pragma once

#include <QRect>
#include <QString>
#include <QVector>

class QLocalServer;
class QLocalSocket;

// "Stream to all screens": the host's displays laid out across this PC's screens,
// one Umbra window per screen, each pinned to the host display that sits the same
// way round (umbra-host's umbraDisplay launch parameter). moonlight-common-c holds
// one stream per process, so the other windows are other processes; the first
// window starts them, and they end when it does.
namespace SpanScreens {

// A host display as GET /actions/displays describes it. bounds is empty when the
// host doesn't say where the display is.
struct HostDisplay {
    int index = -1;
    QString name;
    QRect bounds;
};

// One window: which screen of ours shows which display of the host's.
struct Placement {
    int clientScreen = -1;
    int hostDisplay = -1;
    QString hostDisplayName;
};

// Pairs screens and displays left to right (then top to bottom), so the display on
// the host's left is on the screen on our left. The window this process opens on,
// mainScreen, is always in the plan and always first; the rest are the windows to
// start. Displays without positions keep the host's order. Empty when there is
// nothing to spread across: one screen, or one display.
QVector<Placement> plan(const QVector<QRect>& clientScreens,
                        const QVector<HostDisplay>& hostDisplays,
                        int mainScreen);

// The first window's end of the tie. Every other window connects to it and closes
// itself when the connection goes, which is when this window's session ends or the
// process does, however it ends.
class Lifeline
{
public:
    Lifeline() = default;
    ~Lifeline();
    Lifeline(const Lifeline&) = delete;
    Lifeline& operator=(const Lifeline&) = delete;

    // The name to hand the other windows, or empty if it couldn't be opened.
    QString open();

    // Takes the windows that have connected since. The stream loop doesn't run Qt's
    // event loop, so nothing else would; on Windows a pipe that nobody accepts on
    // leaves the next window unable to connect.
    void poll();

    void close();

private:
    QLocalServer* m_Server = nullptr;
    QVector<QLocalSocket*> m_Windows;
};

// The other windows' end.
class Tether
{
public:
    Tether() = default;
    ~Tether();
    Tether(const Tether&) = delete;
    Tether& operator=(const Tether&) = delete;

    bool connect(const QString& name);

    // Whether the first window has gone. Cheap enough to ask every loop: it only
    // looks at the connection, without waiting.
    bool firstWindowGone();

private:
    QLocalSocket* m_Socket = nullptr;
};

}
