// SPDX-License-Identifier: MIT
// The dark material set by hand: black glass, chrome, backlit in the accent.
// Themes will provide these looks; this file shows what the core can draw.
import Stardust 1.0

Window {
    id: app
    title: "Батарея"
    appId: "stardust-dark"
    background: "#17191d"
    backgroundEnd: "#08090a"
    material: "brushed"
    lightAngle: app.tilt
    duration: 1400

    property real tilt: 200
    property bool saver: false
    property bool optimize: true
    property int period: 1

    // Sweeps the light over the chrome until the tilt sensor drives it.
    property Timer sweep: Timer {
        interval: 1600
        repeat: true
        running: true
        onTriggered: app.tilt = app.tilt > 200 ? 140 : 280
    }

    component Switch: Container {
        id: track
        property bool on: false
        layout: "dock"
        preferredWidth: 58
        preferredHeight: 30
        radius: 15
        padding: 2
        background: on ? Theme.accentDeep : "#050506"
        backgroundEnd: on ? Theme.accentShade : "#1b1d21"
        innerShadow: 5
        innerGlow: on ? 10 : 0
        glowColor: Theme.accentGlow
        Container {
            preferredWidth: 26
            preferredHeight: 26
            radius: 13
            horizontalAlignment: "start"
            verticalAlignment: "center"
            background: "#ffffff"
            material: "chrome"
            shadow: 3
            translationX: track.on ? 28 : 0
        }
    }

    component Row: Container {
        property alias text: caption.text
        layout: "dock"
        preferredHeight: 52
        padding: 14
        Label {
            id: caption
            verticalAlignment: "center"
            horizontalAlignment: "start"
            fontSize: 16
            textColor: "#e6e8eb"
            textShadow: "#000000"
            textShadowOffset: -1
        }
    }

    component Divider: Container {
        preferredHeight: 2
        background: "#000000"
        backgroundEnd: "#14ffffff"
    }

    component Segment: Container {
        id: segment
        property alias text: label.text
        property bool selected: false
        layout: "dock"
        preferredWidth: 109
        preferredHeight: 34
        background: selected ? "#060607" : segment.pressed ? "#b4b9bf" : "#eef0f2"
        backgroundEnd: selected ? "#1b1d21" : "#b4b9bf"
        bevel: selected ? 0 : 1
        innerShadow: selected ? 5 : 0
        Label {
            id: label
            horizontalAlignment: "center"
            verticalAlignment: "center"
            fontSize: 14
            bold: true
            textColor: segment.selected ? Theme.accentLit : "#23262a"
            textShadow: segment.selected ? "" : "#99ffffff"
            textGlow: segment.selected ? 3 : 0
            textGlowColor: Theme.accent
        }
    }

    // A lamp in a chrome bezel; picks the system accent.
    component Swatch: Container {
        id: swatch
        property string color
        property bool chosen: Theme.accent === swatch.color
        layout: "dock"
        preferredWidth: 42
        preferredHeight: 42
        radius: 21
        padding: 6
        background: "#ffffff"
        material: "chrome"
        shadow: 3
        scale: swatch.pressed ? 0.92 : 1
        onClicked: Theme.setAccent(swatch.color)
        Container {
            radius: 15
            verticalAlignment: "fill"
            background: swatch.color
            backgroundEnd: "#000000"
            gloss: 1
            innerShadow: swatch.chosen ? 0 : 3
            glow: swatch.chosen ? 8 : 0
            glowColor: swatch.color
        }
    }

    // Status bar and the glass navigation bar.
    Container {
        layout: "dock"
        preferredHeight: 28
        background: "#000000"
        padding: 6
        Label { text: "12:40"; bold: true; fontSize: 14; horizontalAlignment: "start" }
        Label { text: "98%"; bold: true; fontSize: 14; horizontalAlignment: "end" }
    }
    Container {
        layout: "dock"
        preferredHeight: 52
        background: "#3b3f46"
        backgroundEnd: "#121316"
        gloss: 1
        bevel: 1
        shadow: 2
        Container {
            id: back
            layout: "dock"
            horizontalAlignment: "start"
            verticalAlignment: "center"
            preferredWidth: 104
            preferredHeight: 32
            translationX: 10
            radius: 8
            borderWidth: 1
            borderColor: "#000000"
            background: back.pressed ? "#0d0e10" : "#2c2f35"
            backgroundEnd: back.pressed ? "#1d2024" : "#0f1012"
            bevel: back.pressed ? 0 : 1
            innerShadow: back.pressed ? 4 : 0
            onClicked: app.tilt = app.tilt + 60
            Label {
                text: "‹ Настройки"
                fontSize: 14
                horizontalAlignment: "center"
                verticalAlignment: "center"
                textShadow: "#000000"
                textShadowOffset: -1
            }
        }
        Label {
            text: "Батарея"
            fontSize: 19
            bold: true
            horizontalAlignment: "center"
            verticalAlignment: "center"
            textShadow: "#000000"
            textShadowOffset: -1
        }
    }

    Container {
        padding: 16
        spacing: 18

        // The gauge: chrome bezel, recessed face, backlit ring and readout.
        Container {
            layout: "dock"
            horizontalAlignment: "center"
            preferredWidth: 216
            preferredHeight: 216
            radius: 108
            padding: 9
            background: "#ffffff"
            material: "chrome"
            shadow: 8
            borderWidth: 1
            borderColor: "#000000"
            Container {
                layout: "dock"
                verticalAlignment: "fill"
                radius: 99
                background: "#1f2228"
                backgroundEnd: "#060607"
                innerShadow: 8
                Container {
                    horizontalAlignment: "center"
                    verticalAlignment: "center"
                    preferredWidth: 160
                    preferredHeight: 160
                    radius: 80
                    borderWidth: 3
                    borderColor: Theme.accent
                    glow: 6
                    innerGlow: 4
                    glowColor: Theme.accentGlow
                }
                Container {
                    horizontalAlignment: "center"
                    verticalAlignment: "center"
                    Label {
                        text: "98%"
                        horizontalAlignment: "center"
                        fontSize: 52
                        bold: true
                        textColor: Theme.accentLit
                        textGlow: 6
                        textGlowColor: Theme.accent
                    }
                    Label {
                        text: "НЕ ЗАРЯЖАЕТСЯ"
                        horizontalAlignment: "center"
                        fontSize: 12
                        textColor: "#8a9099"
                    }
                }
            }
        }

        // A recessed group of settings rows.
        Container {
            radius: 12
            borderWidth: 1
            borderColor: "#000000"
            background: "#202328"
            backgroundEnd: "#15171a"
            material: "brushed"
            bevel: 0.6
            Row {
                text: "Энергосбережение"
                onClicked: app.saver = !app.saver
                Switch { on: app.saver; horizontalAlignment: "end"; verticalAlignment: "center" }
            }
            Divider {}
            Row {
                text: "Оптимизация зарядки"
                onClicked: app.optimize = !app.optimize
                Switch { on: app.optimize; horizontalAlignment: "end"; verticalAlignment: "center" }
            }
            Divider {}
            Row {
                text: "Температура"
                Label { text: "28,3 °C"; fontSize: 14; textColor: "#8a9099"; horizontalAlignment: "end"; verticalAlignment: "center" }
            }
        }

        // The accent for the whole system; every app follows it.
        Container {
            orientation: "horizontal"
            horizontalAlignment: "center"
            spacing: 14
            Swatch { color: "#ffb340" }
            Swatch { color: "#4fc3ff" }
            Swatch { color: "#4cd964" }
            Swatch { color: "#ff4d4d" }
            Swatch { color: "#b48cff" }
            Swatch { color: "#f2f2f2" }
        }

        // Chrome segments; the chosen one sinks and lights up.
        Container {
            orientation: "horizontal"
            horizontalAlignment: "center"
            radius: 9
            borderWidth: 1
            borderColor: "#000000"
            Segment { text: "День"; selected: app.period === 0; onClicked: app.period = 0 }
            Segment { text: "Неделя"; selected: app.period === 1; onClicked: app.period = 1 }
            Segment { text: "Месяц"; selected: app.period === 2; onClicked: app.period = 2 }
        }

        // A chrome button and a backlit glass button.
        Container {
            orientation: "horizontal"
            spacing: 12
            Container {
                id: metal
                layout: "dock"
                preferredWidth: 158
                preferredHeight: 48
                radius: 10
                borderWidth: 1
                borderColor: "#000000"
                background: metal.pressed ? "#a0a6ac" : "#f2f4f6"
                backgroundEnd: metal.pressed ? "#c8ccd0" : "#a7adb3"
                gloss: metal.pressed ? 0 : 0.6
                bevel: metal.pressed ? 0 : 1
                innerShadow: metal.pressed ? 5 : 0
                shadow: metal.pressed ? 0 : 4
                onClicked: console.log("details")
                Label {
                    text: "Подробнее"
                    bold: true
                    horizontalAlignment: "center"
                    verticalAlignment: "center"
                    textColor: "#23262a"
                    textShadow: "#99ffffff"
                }
            }
            Container {
                id: glass
                layout: "dock"
                preferredWidth: 158
                preferredHeight: 48
                radius: 10
                borderWidth: 1
                borderColor: "#000000"
                background: glass.pressed ? "#08090a" : "#34373d"
                backgroundEnd: glass.pressed ? "#17191c" : "#0b0c0e"
                gloss: glass.pressed ? 0 : 1
                bevel: glass.pressed ? 0 : 1
                innerShadow: glass.pressed ? 6 : 0
                innerGlow: glass.pressed ? 12 : 3
                glowColor: Theme.accentGlow
                onClicked: console.log("calibrate")
                Label {
                    text: "Калибровать"
                    bold: true
                    horizontalAlignment: "center"
                    verticalAlignment: "center"
                    textColor: Theme.accentLit
                    textGlow: glass.pressed ? 4 : 3
                    textGlowColor: Theme.accent
                }
            }
        }
    }
}
