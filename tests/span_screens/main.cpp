// Stream to all screens: which screen gets which host display, and the tie that closes
// the other windows with the first. The tie is tested across real processes, the way
// Umbra uses it: this program runs itself again as the "other window".

#include "streaming/spanscreens.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTextStream>
#include <QThread>

namespace {

int g_Failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        g_Failures++;
    }
}

using SpanScreens::HostDisplay;
using SpanScreens::Placement;

bool same(const QVector<Placement>& got, const QVector<Placement>& want)
{
    if (got.size() != want.size()) {
        return false;
    }
    for (int i = 0; i < got.size(); i++) {
        if (got[i].clientScreen != want[i].clientScreen || got[i].hostDisplay != want[i].hostDisplay) {
            return false;
        }
    }
    return true;
}

void testPlan()
{
    const QRect left(0, 0, 1920, 1080), right(1920, 0, 2560, 1440);

    // Host lists its right-hand display first; ours are listed left first.
    const QVector<HostDisplay> hostRightFirst = {
        {0, "\\\\.\\DISPLAY1", QRect(1920, 0, 1920, 1080)},
        {1, "\\\\.\\DISPLAY2", QRect(0, 0, 1920, 1080)},
    };
    require(same(SpanScreens::plan({left, right}, hostRightFirst, 0), {{0, 1}, {1, 0}}),
            "left screen shows the host's left display, from the left screen");
    require(same(SpanScreens::plan({left, right}, hostRightFirst, 1), {{1, 0}, {0, 1}}),
            "the window's own screen comes first in the plan");
    {
        const auto named = SpanScreens::plan({left, right}, hostRightFirst, 0);
        require(named.size() == 2 && named[0].hostDisplayName == QLatin1String("\\\\.\\DISPLAY2") &&
                    named[1].hostDisplayName == QLatin1String("\\\\.\\DISPLAY1"),
                "each placement carries its display's name, for pinning by name");
    }

    // Our screens listed right first.
    require(same(SpanScreens::plan({right, left}, hostRightFirst, 1), {{1, 1}, {0, 0}}),
            "client screens are ordered by position, not by index");

    // A host that doesn't say where its displays are keeps its own order.
    const QVector<HostDisplay> unplaced = {{0, "A", QRect()}, {1, "B", QRect()}};
    require(same(SpanScreens::plan({left, right}, unplaced, 0), {{0, 0}, {1, 1}}),
            "displays without positions keep the host's order");

    // Nothing to spread.
    require(SpanScreens::plan({left}, hostRightFirst, 0).isEmpty(), "one screen: no plan");
    require(SpanScreens::plan({left, right}, {hostRightFirst[0]}, 0).isEmpty(), "one display: no plan");
    require(SpanScreens::plan({left, right}, hostRightFirst, 5).isEmpty(), "unknown screen: no plan");

    // Three screens, two displays: the leftmost two, unless the window is on the third.
    const QRect third(4480, 0, 1920, 1080);
    require(same(SpanScreens::plan({left, right, third}, hostRightFirst, 0), {{0, 1}, {1, 0}}),
            "more screens than displays: the leftmost screens");
    require(same(SpanScreens::plan({left, right, third}, hostRightFirst, 2), {{2, 0}, {0, 1}}),
            "the window's screen is never left out");

    // Two screens, three displays: the leftmost displays.
    const QVector<HostDisplay> three = {
        {0, "A", QRect(3840, 0, 1920, 1080)},
        {1, "B", QRect(0, 0, 1920, 1080)},
        {2, "C", QRect(1920, 0, 1920, 1080)},
    };
    require(same(SpanScreens::plan({left, right}, three, 0), {{0, 1}, {1, 2}}),
            "more displays than screens: the leftmost displays");
}

// The other window: connect, then report when the first window has gone.
int runOtherWindow(const QString& name)
{
    SpanScreens::Tether tether;
    if (!tether.connect(name)) {
        QTextStream(stdout) << "no-connect\n";
        return 2;
    }
    QTextStream(stdout) << "connected\n" << Qt::flush;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        if (tether.firstWindowGone()) {
            QTextStream(stdout) << "gone\n";
            return 0;
        }
        QThread::msleep(20);
    }
    QTextStream(stdout) << "timeout\n";
    return 1;
}

QProcess* startOtherWindow(const QString& name)
{
    auto* process = new QProcess();
    process->setProgram(QCoreApplication::applicationFilePath());
    process->setArguments({QStringLiteral("--other-window"), name});
    process->start();
    process->waitForStarted();
    return process;
}

bool waitFor(QProcess* process, const QByteArray& line, int ms)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray out;
    while (timer.elapsed() < ms) {
        process->waitForReadyRead(50);
        out += process->readAllStandardOutput();
        if (out.contains(line)) {
            return true;
        }
    }
    return false;
}

void testTie(bool polled)
{
    SpanScreens::Lifeline lifeline;
    const QString name = lifeline.open();
    require(!name.isEmpty(), "the first window opens its end");

    // Two other windows, as with three screens; the stream loop polls, or (between
    // polls) doesn't.
    QProcess* a = startOtherWindow(name);
    QProcess* b = startOtherWindow(name);
    QElapsedTimer timer;
    timer.start();
    while (polled && timer.elapsed() < 1500) {
        lifeline.poll();
        QThread::msleep(20);
    }
    require(waitFor(a, "connected", 3000), "first other window connects");
    require(waitFor(b, "connected", 3000), "second other window connects");

    // Still open: neither may close yet.
    QThread::msleep(300);
    require(a->state() == QProcess::Running && b->state() == QProcess::Running,
            "other windows stay while the first is open");

    lifeline.close();
    require(waitFor(a, "gone", 3000), "first other window sees the first window go");
    require(waitFor(b, "gone", 3000), "second other window sees the first window go");
    a->waitForFinished(2000);
    b->waitForFinished(2000);
    delete a;
    delete b;
}

void testNoFirstWindow()
{
    SpanScreens::Tether tether;
    require(!tether.connect(QStringLiteral("umbra-screens-test-nobody-%1").arg(QCoreApplication::applicationPid())),
            "connecting to a first window that is gone fails");
}

}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == QLatin1String("--other-window")) {
        return runOtherWindow(args[2]);
    }

    testPlan();
    testTie(true);
    testTie(false);
    testNoFirstWindow();

    if (g_Failures == 0) {
        QTextStream(stdout) << "span_screens: all passed\n";
        return 0;
    }
    return 1;
}
