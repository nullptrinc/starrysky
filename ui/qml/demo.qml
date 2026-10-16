// SPDX-License-Identifier: MIT
// The render core demo in QML. Tapping the card slides the puck for 1.5 s
// and then blocks the QML thread for a second: the slide keeps going,
// because animation runs in the render core.
import Starry.UI 1.0

Window {
    id: window
    title: "Starry UI QML"
    appId: "starry-qml-demo"
    layout: "dock"
    background: "#101425"

    property int taps: 0
    property bool right: false

    function busy(ms) {
        var end = Date.now() + ms
        while (Date.now() < end) {}
    }

    Container {
        padding: 24
        spacing: 16

        Label {
            text: window.taps ? "Tapped " + window.taps : "Starry UI"
            fontSize: 30
            bold: true
        }
        Label {
            text: "QML on the Starry render core"
            fontSize: 14
            textColor: "#b3ffffff"
        }
        Container {
            id: card
            background: window.right ? "#27ae60" : "#2f80ed"
            radius: 16
            padding: 18
            scale: card.pressed ? 0.96 : 1
            onClicked: {
                window.taps++
                window.right = !window.right
                window.busy(1000)
            }
            Label { text: "Tap me"; fontSize: 18; bold: true }
        }
        Container {
            background: "#33ffffff"
            radius: 14
            preferredHeight: 28
            layout: "dock"
            Container {
                background: "#ffffff"
                radius: 14
                preferredWidth: 28
                preferredHeight: 28
                horizontalAlignment: "start"
                duration: 1500
                translationX: window.right ? 260 : 0
            }
        }
    }
}
