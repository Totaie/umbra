import QtQuick 2.0
import QtQuick.Controls
import QtQuick.Window 2.2

import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import Session 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

import "theme"

// The screen between clicking a PC and being on it.
//
// This used to be a game launcher: box art behind a fourteen-second zoom, one line of
// text that said the same thing for the whole wait, and a tip about a gamepad. None of
// that is what someone connecting to a machine wants to know. They want to know which
// machine, and what is taking so long - so this shows the host being reached and the
// stages the client already reports, each keeping the time it took. A slow connect then
// points at whoever caused it instead of just feeling broken.
Item {
    property Session session
    property string appName

    // Still passed by AppView. Nothing draws it any more.
    property string boxArtUrl: ""

    // 自带背景，main.qml 的全局壁纸层不用再垫一层
    readonly property bool usesOwnBackground: true
    property bool isResume : false
    property bool quitAfter : false

    // 0 reaching the host, 1 negotiating, 2 opening streams, 3 waiting for a frame.
    // Everything below this index has finished.
    property int currentStage: 0
    property bool failed: false

    // How long each finished stage took. Measured from the arrival of the signals
    // rather than by a running timer: Session::exec() owns the main loop from here on
    // and pumps Qt by hand, so queued signals get through where a Timer ticking every
    // 100 ms would not.
    property var stageMs: [0, 0, 0, 0]
    property double stageStartedAt: 0

    readonly property var stageNames: [
        qsTr("Reached the host"),
        qsTr("Session negotiated"),
        qsTr("Opening video, audio and input"),
        qsTr("Waiting for the first frame")
    ]

    // A quarter of a second of flourish is worth it when you're being walked into a
    // game. It isn't when you clicked a machine and want to be on it - there the
    // animation is just time spent not working. Session::exec() holds the main loop
    // open for whichever length this is.
    readonly property int exitDurationMs: StreamingPreferences.directConnectDesktop ? 120 : 340

    function advanceTo(stage)
    {
        if (stage <= currentStage || failed) {
            return
        }

        var now = Date.now()
        var elapsed = stageMs.slice()
        for (var i = currentStage; i < stage && i < 4; i++) {
            elapsed[i] = now - stageStartedAt
            stageStartedAt = now
        }

        stageMs = elapsed
        currentStage = stage
    }

    function stageStarting(stage)
    {
        // These names come from the connection library, so match on what they contain
        // rather than on an exact string. Anything unrecognised is early setup, which
        // is stage 0 and already counted as done by the time it arrives - the host
        // answered the launch request to get us here at all.
        var name = stage.toLowerCase()

        if (name.indexOf("rtsp") !== -1) {
            advanceTo(1)
        }
        else if (name.indexOf("control") !== -1 || name.indexOf("video") !== -1 ||
                 name.indexOf("input") !== -1 || name.indexOf("audio stream") !== -1) {
            advanceTo(2)
        }
    }

    function stageFailed(stage, errorCode, failingPorts)
    {
        failed = true
        errorTitle.text = qsTr("%1 failed.").arg(stage)
        errorDetail.text = failingPorts
                ? qsTr("Nothing reached port(s) %1. Check your firewall and port forwarding rules.").arg(failingPorts)
                : qsTr("The host stopped answering during setup.")
        // Also queue the dialog Session::exec() shows once it returns
        streamSegueErrorDialog.text = qsTr("Starting %1 failed: Error %2").arg(stage).arg(errorCode)

        if (failingPorts) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts)
        }
    }

    function hideForStreaming()
    {
        // Hide the UI contents so the user doesn't
        // see them briefly when we pop off the StackView
        contentRoot.visible = false

        // 窗口本身不在这里藏，由 Session::exec() 在串流窗口进入全屏之后隐藏。
        // 提前藏的话，macOS 切进新 Space 的整个动画期间旧 Space 露出来的是桌面，
        // 而不是这层已经全黑的幕。
    }

    function connectionStarted()
    {
        advanceTo(4)

        // 淡出到全黑。Session::exec() 会等这条动画跑完再创建串流窗口，
        // 所以交接是在一块纯黑上完成的，中间不会闪。
        exitAnimation.start()
    }

    function displayLaunchError(text)
    {
        failed = true
        errorTitle.text = text
        errorDetail.text = ""

        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = text
        console.error(text)
    }

    function quitStarting()
    {
        // Avoid the push transition animation
        var component = Qt.createComponent("QuitSegue.qml")
        stackView.replace(stackView.currentItem, component.createObject(stackView, {"appName": appName}), StackView.Immediate)

        // Show the Qt window again to show quit segue
        window.visible = true
    }

    function sessionFinished(portTestResult)
    {
        if (portTestResult !== 0 && portTestResult !== -1 && streamSegueErrorDialog.text) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Umbra. Streaming over the Internet may not work while connected to this network.")
        }

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()

        // Pop the StreamSegue off the stack if this is a GUI-based app launch
        if (!quitAfter) {
            stackView.pop()
        }

        if (quitAfter && !streamSegueErrorDialog.text) {
            // If this was a CLI launch without errors, exit now
            Qt.quit()
        }
        else {
            // Show the Qt window again after streaming
            window.visible = true

            // Display any launch errors. We do this after
            // the Qt UI is visible again to prevent losing
            // focus on the dialog which would impact gamepad
            // users.
            if (streamSegueErrorDialog.text) {
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
        }
    }

    function sessionReadyForDeletion()
    {
        // Garbage collect the Session object since it's pretty heavyweight
        // and keeps other libraries (like SDL_TTF) around until it is deleted.
        session = null
        gc()
    }

    StackView.onDeactivating: {
        // Show the toolbar again when popped off the stack
        toolBar.shown = true

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.shown = false

        // Stop polling now rather than when the window hides, which doesn't happen
        // until the stream is already up. Every poll is a serverinfo request the
        // host answers instead of getting on with starting capture, and they were
        // landing right through the handshake. It restarts by itself when the
        // window comes back afterwards.
        if (window.pollingActive) {
            ComputerManager.stopPollingAsync()
            window.pollingActive = false
        }

        // Hook up our signals
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.displayLaunchError.connect(displayLaunchError)
        session.quitStarting.connect(quitStarting)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)

        // Ensure the SystemProperties async thread is finished,
        // since it may currently be using the SDL video subsystem
        SystemProperties.waitForAsyncLoad()

        stageStartedAt = Date.now()
        enterAnimation.start()

        // Kick off the stream
        streamLoader.active = true
    }

    // Flat, and the app's own surface rather than a picture of a game. Something that
    // moves for fourteen seconds behind a progress bar is a promise about how long the
    // wait is going to be.
    Rectangle {
        anchors.fill: parent
        color: Theme.ink
        z: -2
    }

    // 进入串流时盖上来的幕。
    //
    // 这一层刻意用纯黑而不是 Theme.ink：接手它的是 SDL 串流窗口，而 SDL 窗口在拿到
    // 第一帧之前就是纯黑的（实测 macOS 上是 0,0,0）。两边同色，交接那一刻才没有色阶跳变。
    Rectangle {
        id: exitVeil
        anchors.fill: parent
        color: "black"
        opacity: 0
        visible: opacity > 0
        z: 10
    }

    ParallelAnimation {
        id: enterAnimation
        NumberAnimation {
            target: contentRoot; property: "opacity"
            from: 0; to: 1; duration: 260; easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: contentShift; property: "y"
            from: 10; to: 0; duration: 300; easing.type: Easing.OutCubic
        }
    }

    ParallelAnimation {
        id: exitAnimation

        NumberAnimation {
            target: contentRoot; property: "opacity"
            to: 0; duration: Math.round(exitDurationMs * 0.76); easing.type: Easing.InCubic
        }
        SequentialAnimation {
            NumberAnimation {
                target: exitVeil; property: "opacity"
                to: 1; duration: exitDurationMs; easing.type: Easing.InOutQuad
            }
            ScriptAction {
                script: hideForStreaming()
            }
        }
    }

    Timer {
        id: startSessionTimer
        onTriggered: {
            // Garbage collect QML stuff before we start streaming,
            // since we'll probably be streaming for a while and we
            // won't be able to GC during the stream.
            gc()

            // Run the streaming session to completion
            session.start()
        }
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true

        onLoaded: {
            // Stop GUI gamepad usage now
            SdlGamepadKeyNavigation.disable()

            // Initialize the session and probe for host/client capabilities
            if (!session.initialize(window)) {
                sessionFinished(0);
                sessionReadyForDeletion();
                return;
            }

            // Don't wait unless we have toasts to display
            startSessionTimer.interval = 0

            // Display the toasts together in a vertical centered arrangement
            var yOffset = 0
            for (var i = 0; i < session.launchWarnings.length; i++) {
                var text = session.launchWarnings[i]
                console.warn(text)

                // Show the tooltip for 3 seconds
                var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
                toast.timeout = 3000
                toast.text = text
                toast.y += yOffset
                toast.visible = true

                // Offset the next toast below the previous one
                yOffset = toast.y + toast.padding + toast.height

                // Allow an extra 500 ms for the tooltip's fade-out animation to finish
                startSessionTimer.interval = toast.timeout + 500;
            }

            // Umbra is used as a remote desktop, where a connection that takes three
            // and a half seconds to appear feels broken. The warnings are still shown
            // and still logged, we just don't hold the session back waiting for them
            // to fade. Only when the app grid has been skipped, so anyone deliberately
            // browsing apps still gets the full-length warning.
            if (StreamingPreferences.directConnectDesktop) {
                startSessionTimer.interval = 0
            }

            // Start the timer to wait for toasts (or start the session immediately)
            startSessionTimer.start()
        }

        sourceComponent: Item {}
    }

    Item {
        id: contentRoot

        anchors.fill: parent
        opacity: 0

        // 淡入的同时轻微上浮
        transform: Translate {
            id: contentShift
            y: 10
        }

        Column {
            id: panel

            anchors.centerIn: parent
            width: Math.min(parent.width - Theme.spaceXl * 2, 620)
            spacing: Theme.spaceXl

            // ---- which machine ----
            Item {
                width: parent.width
                height: Math.max(hostBlock.height, statusChip.height)

                Row {
                    id: hostBlock

                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - statusChip.width - Theme.spaceLg
                    spacing: Theme.spaceLg

                    Rectangle {
                        width: 52
                        height: 52
                        color: Theme.surface2
                        border.width: 1
                        border.color: Theme.lineStrong
                        anchors.verticalCenter: parent.verticalCenter

                        // The same monitor glyph the PC tiles use, for the same reason:
                        // it reads as a machine where a letter in a circle reads as a person.
                        Canvas {
                            anchors.centerIn: parent
                            width: 26
                            height: 26
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                ctx.strokeStyle = Theme.accent
                                ctx.lineWidth = 1.6
                                ctx.strokeRect(2.5, 4, 21, 13)
                                ctx.beginPath()
                                ctx.moveTo(8, 22); ctx.lineTo(18, 22)
                                ctx.moveTo(13, 17); ctx.lineTo(13, 22)
                                ctx.stroke()
                            }
                        }
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 52 - Theme.spaceLg
                        spacing: 3

                        Text {
                            width: parent.width
                            text: session && session.hostName ? session.hostName : appName
                            color: Theme.text
                            font.family: Theme.fontSans
                            font.pointSize: 20
                            font.weight: Font.ExtraBold
                            font.letterSpacing: Theme.trackingTight(20)
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            text: {
                                var parts = []
                                if (session && session.hostAddress) parts.push(session.hostAddress)
                                parts.push(appName)
                                // Which host build is answering. Every host reports the same
                                // GameStream version, so without this there is nothing on
                                // screen that distinguishes one from another.
                                if (session && session.hostVersion) parts.push("Host " + session.hostVersion)
                                return parts.join("  ·  ")
                            }
                            color: Theme.textDim
                            font.family: Theme.fontMono
                            font.pointSize: Theme.fontBody
                            elide: Text.ElideRight
                        }
                    }
                }

                Rectangle {
                    id: statusChip

                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: chipText.implicitWidth + Theme.spaceLg
                    height: chipText.implicitHeight + Theme.spaceSm + 2
                    color: "transparent"
                    border.width: 1
                    border.color: failed ? Theme.danger : Theme.lineStrong

                    Text {
                        id: chipText

                        anchors.centerIn: parent
                        text: failed ? qsTr("Failed")
                                     : (isResume ? qsTr("Resuming") : qsTr("Connecting"))
                        color: failed ? Theme.danger : Theme.textDim
                        font.family: Theme.fontMono
                        font.pointSize: Theme.fontCaption
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: Theme.trackingCaption
                    }
                }
            }

            // ---- how far along ----
            Column {
                width: parent.width
                spacing: 0

                Repeater {
                    model: 4

                    Item {
                        width: panel.width
                        height: 40

                        readonly property bool isDone: index < currentStage
                        readonly property bool isNow: index === currentStage && !failed
                        readonly property bool isFail: index === currentStage && failed

                        Rectangle {
                            id: tick

                            width: 14
                            height: 14
                            anchors.left: parent.left
                            anchors.leftMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: isFail ? Theme.danger
                                          : (isDone ? Theme.accentDim
                                                    : (isNow ? Theme.accent : "transparent"))
                            border.width: 1
                            border.color: isFail ? Theme.danger
                                                 : (isDone ? Theme.accentDim
                                                           : (isNow ? Theme.accent : Theme.lineStrong))

                            SequentialAnimation on opacity {
                                running: isNow
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.35; duration: 550; easing.type: Easing.InOutSine }
                                NumberAnimation { to: 1.0;  duration: 550; easing.type: Easing.InOutSine }
                            }
                        }

                        Text {
                            anchors.left: tick.right
                            anchors.leftMargin: Theme.spaceLg
                            anchors.right: elapsed.left
                            anchors.rightMargin: Theme.spaceMd
                            anchors.verticalCenter: parent.verticalCenter
                            text: stageNames[index]
                            color: isFail ? Theme.danger
                                          : (isNow ? Theme.text
                                                   : (isDone ? Theme.textDim : Theme.textFaint))
                            font.family: Theme.fontSans
                            font.pointSize: 12
                            font.weight: (isNow || isFail) ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                        }

                        Text {
                            id: elapsed

                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: stageMs[index] > 0 ? (stageMs[index] / 1000).toFixed(1) + "s" : ""
                            color: Theme.textFaint
                            font.family: Theme.fontMono
                            font.pointSize: Theme.fontCaption
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.line
                            visible: index < 3
                        }
                    }
                }
            }

            HardProgress {
                id: stageSpinner

                width: parent.width
                visible: !failed
            }

            // ---- what went wrong, in place ----
            Item {
                width: parent.width
                height: failed ? errorColumn.height + Theme.spaceLg * 2 : 0
                visible: failed
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: Theme.surface
                }

                Rectangle {
                    anchors.left: parent.left
                    width: Theme.accentBarStrong
                    height: parent.height
                    color: Theme.danger
                }

                Column {
                    id: errorColumn

                    anchors.left: parent.left
                    anchors.leftMargin: Theme.accentBarStrong + Theme.spaceLg
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceLg
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceXs

                    Text {
                        id: errorTitle

                        width: parent.width
                        color: Theme.text
                        font.family: Theme.fontSans
                        font.pointSize: 12
                        font.weight: Font.ExtraBold
                        wrapMode: Text.Wrap
                    }

                    Text {
                        id: errorDetail

                        width: parent.width
                        color: Theme.textDim
                        font.family: Theme.fontSans
                        font.pointSize: Theme.fontBody
                        wrapMode: Text.Wrap
                        visible: text !== ""
                    }
                }
            }
        }

        // ---- the shortcuts that matter once you're in ----
        Text {
            id: hintText

            anchors.bottom: parent.bottom
            anchors.bottomMargin: 44
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.textFaint
            font.family: Theme.fontMono
            font.pointSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap

            // The gamepad chord is only worth mentioning when a gamepad is attached.
            text: {
                var disconnect = SdlGamepadKeyNavigation.getConnectedGamepads() > 0
                        ? qsTr("Start+Select+L1+R1") : qsTr("Ctrl+Alt+Shift+Q")
                return qsTr("%1 disconnects").arg(disconnect) + "   ·   " +
                       qsTr("Ctrl+Shift+D switches display")
            }
        }
    }
}
