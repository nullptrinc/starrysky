// SPDX-License-Identifier: MIT
// A dialer without a telephony backend: dial a number, "call" it, hang up.
import Stardust 1.0

Window {
    id: app
    title: "Телефон"
    appId: "starry-dialer"
    layout: "dock"
    background: "#0b0d17"

    property string number: ""
    property bool calling: false
    property bool connected: false
    property int seconds: 0

    // Pretend the other side answers after two seconds, then count the call.
    property Timer answer: Timer {
        interval: 2000
        running: app.calling && !app.connected
        onTriggered: app.connected = true
    }
    property Timer clock: Timer {
        interval: 1000
        repeat: true
        running: app.connected
        onTriggered: app.seconds++
    }

    function press(key) {
        if (number.length < 15)
            number += key
    }
    function erase() { number = number.slice(0, -1) }
    function call() {
        if (number.length === 0)
            return
        seconds = 0
        connected = false
        calling = true
    }
    function hangUp() {
        calling = false
        connected = false
    }
    function twoDigits(n) { return n < 10 ? "0" + n : "" + n }

    component Key: Container {
        id: key
        property string digit
        property string letters
        preferredWidth: 76
        preferredHeight: 76
        radius: 38
        layout: "dock"
        background: key.pressed ? "#3a4166" : "#1f2336"
        duration: 120
        onClicked: app.press(key.digit)

        Container {
            horizontalAlignment: "center"
            verticalAlignment: "center"
            Label { text: key.digit; fontSize: 30; horizontalAlignment: "center" }
            Label {
                text: key.letters
                fontSize: 10
                bold: true
                textColor: "#99ffffff"
                horizontalAlignment: "center"
            }
        }
    }

    component Row: Container {
        orientation: "horizontal"
        spacing: 24
        horizontalAlignment: "center"
    }

    component RoundButton: Container {
        id: button
        property string label
        property string colour
        property string pressedColour
        preferredWidth: 76
        preferredHeight: 76
        radius: 38
        layout: "dock"
        background: button.pressed ? button.pressedColour : button.colour
        duration: 120
        Label {
            text: button.label
            fontSize: 14
            bold: true
            horizontalAlignment: "center"
            verticalAlignment: "center"
        }
    }

    // ---- dialling ----
    Container {
        opacity: app.calling ? 0 : 1
        duration: 200
        padding: 24
        spacing: 18

        Container { preferredHeight: 110 }
        Label {
            text: app.number
            fontSize: app.number.length > 11 ? 26 : 34
            horizontalAlignment: "center"
            preferredHeight: 48
        }
        Container { preferredHeight: 34 }

        Row { Key { digit: "1" } Key { digit: "2"; letters: "ABC" } Key { digit: "3"; letters: "DEF" } }
        Row { Key { digit: "4"; letters: "GHI" } Key { digit: "5"; letters: "JKL" } Key { digit: "6"; letters: "MNO" } }
        Row { Key { digit: "7"; letters: "PQRS" } Key { digit: "8"; letters: "TUV" } Key { digit: "9"; letters: "WXYZ" } }
        Row { Key { digit: "*" } Key { digit: "0"; letters: "+" } Key { digit: "#" } }

        Row {
            Container { preferredWidth: 76; preferredHeight: 76 }
            RoundButton {
                label: "Вызов"
                colour: "#27ae60"
                pressedColour: "#3ddc78"
                onClicked: app.call()
            }
            Container {
                preferredWidth: 76
                preferredHeight: 76
                layout: "dock"
                opacity: app.number.length ? 1 : 0
                duration: 150
                onClicked: app.erase()
                Label {
                    text: "←"
                    fontSize: 30
                    textColor: "#ccffffff"
                    horizontalAlignment: "center"
                    verticalAlignment: "center"
                }
            }
        }
    }

    // ---- in a call ----
    Container {
        opacity: app.calling ? 1 : 0
        translationY: app.calling ? 0 : 40
        duration: 250
        padding: 24
        spacing: 12

        Container { preferredHeight: 150 }
        Label {
            text: app.number
            fontSize: 34
            horizontalAlignment: "center"
        }
        Label {
            text: app.connected
                ? app.twoDigits(Math.floor(app.seconds / 60)) + ":" + app.twoDigits(app.seconds % 60)
                : "Вызов…"
            fontSize: 18
            textColor: "#b3ffffff"
            horizontalAlignment: "center"
        }
        Container { preferredHeight: 300 }
        Row {
            RoundButton {
                label: "Отбой"
                colour: "#eb5757"
                pressedColour: "#ff7b7b"
                onClicked: app.hangUp()
            }
        }
    }
}
