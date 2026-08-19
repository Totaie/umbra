import QtQuick 2.9
import "."

// A dashed horizontal rule.
//
// Qt Quick has no border-style, so the dashes are laid out as a row of small
// rectangles clipped to the available width. It is used where a solid hairline
// would read as a boundary and this only needs to read as a fold - the divisions
// on the connect screen, between sections of the in-session menu.
Item {
    id: root

    property color color: Theme.line
    property int dashLength: 4
    property int gapLength: 4
    property int thickness: 1

    implicitHeight: thickness

    // Rounded up so the run always reaches the right edge; the last dash is
    // clipped rather than leaving a gap that looks like a mistake.
    readonly property int dashCount: Math.ceil(width / (dashLength + gapLength))

    clip: true

    Row {
        spacing: root.gapLength

        Repeater {
            model: root.dashCount

            Rectangle {
                width: root.dashLength
                height: root.thickness
                color: root.color
            }
        }
    }
}
