// SPDX-License-Identifier: MIT
// The battery from Constellation, live, and the power saver switch.
import Stardust 1.0
import Constellation 1.0

Window {
    id: app
    title: "Батарея"
    appId: "starry-battery"
    background: "#0b0d17"
    padding: 24
    spacing: 14

    property Battery battery: Battery {}
    property Blink saver: Blink { path: "/blinks/power/saver" }
    property bool saving: saver.values.enabled === true

    Label {
        text: app.battery.present ? app.battery.level + "%" : "Нет батареи"
        fontSize: 64
        bold: true
        textColor: app.battery.charging ? "#27ae60" : app.saving ? "#f2c94c" : "#ffffff"
    }
    Label {
        text: app.battery.charging ? "Заряжается" :
              app.battery.plugged ? "Подключено, не заряжается" : "От батареи"
        fontSize: 18
        textColor: "#a0a8c0"
    }
    Label {
        text: app.battery.voltage.toFixed(3) + " В   " +
              (app.battery.current * 1000).toFixed(0) + " мА   " +
              app.battery.temperature.toFixed(1) + " °C"
        fontSize: 16
        textColor: "#a0a8c0"
    }
    Container {
        id: toggle
        padding: 18
        radius: 16
        background: app.saving ? "#f2c94c" : toggle.pressed ? "#3a4060" : "#262b45"
        scale: toggle.pressed ? 0.97 : 1
        onClicked: app.saver.publish({ enabled: !app.saving })
        Label {
            text: app.saving ? "Энергосбережение: вкл" : "Энергосбережение: выкл"
            fontSize: 18
            bold: true
            textColor: app.saving ? "#0b0d17" : "#ffffff"
        }
    }
}
