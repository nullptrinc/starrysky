# Starry

A phone shell for Linux, split the way iOS splits backboardd and SpringBoard.

- **starry-compositor**: a Wayland compositor built on wlroots. It owns the panel,
  input, the bottom-edge home gesture and application windows ("scenes"). It draws
  no UI of its own.
- **starboard**: the system shell. A separate, privileged Wayland client
  that draws the home screen, dock, status bar and home indicator, launches apps,
  and drives scenes through the private `starry-system-v1` protocol. The compositor
  starts it on a private socket and restarts it when it dies. Open applications
  survive the restart and are adopted again by the new starboard.
- **Plugins**: shared objects loaded by starboard through a stable C ABI
  (`include/starry-plugin.h`). They run in the starboard process only, so a
  faulty plugin can never take down the compositor or running apps.

Developed on a Xiaomi Mi 11 Lite 4G (courbet, 1080x2400, Adreno 618).

## Status

Working:

- full-screen apps (xdg-shell), touch, keyboard, power key screen toggle
- swipe up from the bottom edge to go home; the app follows the finger
- home grid and dock; long press to rearrange, drag between grid and dock,
  order saved in `/var/lib/starry/layout`; haptic tick on pick-up (evdev FF)
- status bar with clock and plugin items; `wifi` plugin
- app switcher: slow swipe up from the bottom edge (or a swipe up on the home
  screen); cards scroll sideways, fling up to close, tap to open
- starts at boot through a sysvinit service
- starboard crash recovery ("respring") without losing apps

Not yet: lock screen, notifications, on-screen keyboard
(text-input/input-method), rounded cards and live switcher previews, zoom
from the icon when opening an app.

## Stardust

Stardust is Starry's application framework, in the spirit of BlackBerry 10's
Cascades. An application describes its UI in QML; the QtQml engine is used
only as the language. Every property change, bindings included, becomes a
mutation for a render core that runs on its own thread and owns the window,
layout (StackLayout, DockLayout), implicit animations, touch handling and
GLES2 drawing. A busy application thread therefore never stalls an
animation: an `onClicked` handler that blocks for a second leaves a running
slide at 60 frames per second.

```qml
import Stardust 1.0

Window {
    layout: "dock"
    background: "#101425"
    Container {
        id: card
        padding: 18
        radius: 16
        background: card.pressed ? "#5a9cff" : "#2f80ed"
        scale: card.pressed ? 0.96 : 1
        onClicked: console.log("tapped")
        Label { text: "Tap me"; fontSize: 18; bold: true }
    }
}
```

Run it with `stardust app.qml`. Elements: `Window`, `Container`, `Label`,
`Timer`; see `stardust/qml/dialer.qml` for a fuller example.
`STARDUST_TRACE=1` prints frame statistics for every animation.

## Constellation

Constellation is Starry's hardware abstraction, after QNX PPS on BlackBerry 10.
System services are the only code that touches drivers; they publish state as
small typed objects in the `/blinks` namespace through the `constellationd`
broker (`/run/constellation.sock`), and applications read and subscribe to them.

```
$ blinks get /blinks/power/battery
@/blinks/power/battery
present:b:true
level:n:98
status::not-charging
charging:b:false
plugged:b:true
voltage:n:4.274228
temperature:n:28.3
```

- Attributes are `name:type:value`: `n` number, `b` boolean, empty for a string.
  Publishing merges attributes; `-name` removes one.
- The connection that creates an object owns it until it disconnects.
- Subscribers get the whole object on every change. A slow subscriber skips
  intermediate states but always gets the latest one. A path ending in `/`
  subscribes to everything under it.
- Objects live in the broker's memory: services publish theirs again after a
  broker restart, values set by hand are gone.

`blinks ls|get|set|del|watch` pokes at the namespace from a shell. C services
and clients use `constellation/constellation.h`. QML apps use
`import Constellation 1.0`: `Battery` for the battery, `Blink { path: ... }`
for any object (`values`, `exists`, `publish({...})`). See
`stardust/qml/battery.qml`.

Services: `constellation-power` publishes `/blinks/power/battery`. starboard
reads the battery and `/blinks/power/saver` (`enabled:b`) from Constellation.

## Layout

| Path | |
|---|---|
| `compositor/` | wlroots compositor; `system.c` implements `starry-system-v1` and supervises starboard |
| `starboard/` | system shell client; `home.c` grid/dock, `status.c` status bar and plugin host |
| `protocol/starry-system-v1.xml` | compositor ↔ starboard protocol: layers, scenes, go home |
| `include/starry-plugin.h` | plugin ABI |
| `plugins/` | bundled plugins |
| `stardust/` | Stardust: `core/` render core, `qml/` QML front end and examples |
| `constellation/` | Constellation: `constellationd` broker, client library, `blinks` tool, `services/` |
| `data/starry-run.in` | starts the compositor through `seatd-launch` |

## Building

Dependencies: wlroots 0.20 (DRM, libinput, GLES2, GBM, session), wayland,
xkbcommon, freetype2, meson, ninja, wayland-scanner.

```sh
meson setup build --prefix=/opt/starry --libdir=lib
ninja -C build
DESTDIR=$PWD/stage meson install -C build --strip
```

Cross-compiling: pass a meson `--cross-file` for your target sysroot, and a
`--native-file` if `wayland-scanner` is not installed on the build machine.

Binaries get an rpath to `<prefix>/lib`, so a private wlroots build can sit next to them.

## Running

On the device, as root on a free VT:

```sh
/opt/starry/bin/starry-run
```

The log goes to `/run/starry.log`.

Screenshots of the whole screen, as root: `starry-shot shot.ppm`, or a series
with `starry-shot -n 20 -i 50 frame-%02d.ppm`. The compositor offers
wlr-screencopy only to `starry-shot` run as root and to starboard.

To start Starry at boot on a sysvinit system, link the bundled service and enable it:

```sh
ln -s /opt/starry/share/starry/sysvinit/starry.init /etc/init.d/starry
update-rc.d starry defaults 99 20
ln -s /opt/starry/share/starry/sysvinit/constellation.init /etc/init.d/constellation
update-rc.d constellation defaults 60 20
```

The app catalogue lives in `<prefix>/etc/starry/apps`, one app per line
(`Name|#RRGGBB|dock(0/1)|shell command`). Without it, a demo set of Weston clients is used.

Environment overrides: `STARRY_STARBOARD` (starboard binary), `STARRY_APPS`,
`STARRY_PLUGIN_DIR`, `STARRY_DEBUG` (verbose wlroots log).

## Writing a plugin

```c
#include <stdio.h>
#include <starry-plugin.h>

static void hello_text(void *data, char *buffer, size_t size) {
	snprintf(buffer, size, "Hello");
}

static bool hello_init(struct starry_host *host, const struct starry_host_api *api) {
	const struct starry_status_item item = {
		.size = sizeof(item), .id = "hello", .side = STARRY_STATUS_LEFT,
		.text = hello_text,
	};
	return api->add_status_item(host, &item);
}

const struct starry_plugin starry_plugin = {
	.abi = STARRY_PLUGIN_ABI, .name = "hello", .init = hello_init,
};
```

Build it as a shared object and drop it into `<prefix>/lib/starry/plugins/`.

## License

MIT, see [LICENSE](LICENSE).
