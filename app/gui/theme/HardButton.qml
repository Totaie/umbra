import QtQuick 2.9
import QtQuick.Controls
import "."

// 方角硬投影按钮。只覆盖 background（FluentWinUI3 的圆角是从它自己的
// __config 背景来的），contentItem 交给基类，图标和文字的排布不受影响。
Button {
    id: control

    font.family: Theme.fontUi
    font.bold: true

    // Set briefly by the click animation below so the button reads as pressed even
    // when nothing held it down.
    property bool struck: false

    background: Panel {
        id: panel

        implicitWidth: 96
        implicitHeight: 34

        fill: control.down || control.struck
                  ? Theme.accentDim
                  : (control.hovered ? Theme.surface2 : Theme.surface)
        borderColor: control.down || control.struck || control.hovered || control.visualFocus
                     ? Theme.accent : Theme.lineStrong
        // 按钮比卡片小，投影跟着收一档，否则一堆小按钮会糊成黑块
        shadowDepth: control.down || control.struck
                         ? 2
                         : (control.hovered || control.visualFocus ? 6 : 4)
        liftShift: control.down || control.struck ? 1 : 0
        opacity: control.enabled ? 1.0 : 0.45
    }

    // Holding the mouse down already animates, through the Panel's own behaviors on
    // shadow and offset. Activating from the keyboard or a gamepad does not: down goes
    // true and false inside one frame, the bindings never settle anywhere, and the
    // button answers by doing nothing at all. This replays the same press so every
    // route to a click looks the same.
    SequentialAnimation {
        id: strike

        PropertyAction { target: control; property: "struck"; value: true }
        PauseAnimation { duration: Theme.durFast }
        PropertyAction { target: control; property: "struck"; value: false }
    }

    onClicked: strike.restart()
}
