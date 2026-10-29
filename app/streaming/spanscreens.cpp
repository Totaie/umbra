#include "spanscreens.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QUuid>

#include <algorithm>

namespace SpanScreens {

namespace {

bool leftToRight(const QRect& a, const QRect& b)
{
    if (a.x() != b.x()) {
        return a.x() < b.x();
    }
    return a.y() < b.y();
}

}

QVector<Placement> plan(const QVector<QRect>& clientScreens,
                        const QVector<HostDisplay>& hostDisplays,
                        int mainScreen)
{
    if (clientScreens.size() < 2 || hostDisplays.size() < 2 ||
            mainScreen < 0 || mainScreen >= clientScreens.size()) {
        return {};
    }

    QVector<int> screens;
    for (int i = 0; i < clientScreens.size(); i++) {
        screens.append(i);
    }
    std::stable_sort(screens.begin(), screens.end(), [&](int a, int b) {
        return leftToRight(clientScreens[a], clientScreens[b]);
    });

    QVector<HostDisplay> displays = hostDisplays;
    const bool allPlaced = std::all_of(displays.begin(), displays.end(), [](const HostDisplay& d) {
        return !d.bounds.isEmpty();
    });
    if (allPlaced) {
        std::stable_sort(displays.begin(), displays.end(), [](const HostDisplay& a, const HostDisplay& b) {
            return leftToRight(a.bounds, b.bounds);
        });
    }

    const int count = std::min(screens.size(), displays.size());

    // More screens than displays: the leftmost screens get them, unless that leaves
    // out the screen this window is on, which then takes the last display instead.
    int mainSlot = screens.indexOf(mainScreen);
    if (mainSlot >= count) {
        screens[count - 1] = mainScreen;
        mainSlot = count - 1;
    }

    QVector<Placement> placements;
    placements.append({mainScreen, displays[mainSlot].index, displays[mainSlot].name});
    for (int slot = 0; slot < count; slot++) {
        if (slot != mainSlot) {
            placements.append({screens[slot], displays[slot].index, displays[slot].name});
        }
    }
    return placements;
}

Lifeline::~Lifeline()
{
    close();
}

QString Lifeline::open()
{
    close();

    m_Server = new QLocalServer();
    // Only this user's processes may connect: the name is all a window needs to
    // keep itself open, nothing more.
    m_Server->setSocketOptions(QLocalServer::UserAccessOption);
    const QString name = QStringLiteral("umbra-screens-%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QUuid::createUuid().toString(QUuid::Id128));
    if (!m_Server->listen(name)) {
        close();
        return QString();
    }
    return name;
}

void Lifeline::poll()
{
    if (m_Server == nullptr) {
        return;
    }
    while (m_Server->waitForNewConnection(0)) {
        while (QLocalSocket* window = m_Server->nextPendingConnection()) {
            m_Windows.append(window);
        }
    }
}

void Lifeline::close()
{
    // Closing the server leaves the connections it accepted open; they have to go
    // too for the other windows to notice.
    for (QLocalSocket* window : std::as_const(m_Windows)) {
        window->abort();
        delete window;
    }
    m_Windows.clear();

    if (m_Server != nullptr) {
        m_Server->close();
        delete m_Server;
        m_Server = nullptr;
    }
}

Tether::~Tether()
{
    delete m_Socket;
}

bool Tether::connect(const QString& name)
{
    delete m_Socket;
    m_Socket = new QLocalSocket();
    m_Socket->connectToServer(name);
    if (!m_Socket->waitForConnected(3000)) {
        delete m_Socket;
        m_Socket = nullptr;
        return false;
    }
    return true;
}

bool Tether::firstWindowGone()
{
    if (m_Socket == nullptr) {
        return false;
    }
    // Nothing is ever sent down it, so this returns at once; what it does is let the
    // socket see that the other end has closed.
    m_Socket->waitForReadyRead(0);
    return m_Socket->state() != QLocalSocket::ConnectedState;
}

}
