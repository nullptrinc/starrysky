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

## Related projects

- [Stardust](https://github.com/nullptrinc/stardust): the application framework (QML, materials, `Theme`).
- [Constellation](https://github.com/nullptrinc/constellation): the `/blinks` broker for system state;
  starboard reads the battery, `/blinks/power/saver` and the app registry from it.
- [cosmic](https://github.com/nullptrinc/cosmic): `.apf` packages and the app registry (`/blinks/apps/`).
- [sysapps](https://github.com/nullptrinc/sysapps): the system apps.

## Layout

| Path | |
|---|---|
| `compositor/` | wlroots compositor; `system.c` implements `starry-system-v1` and supervises starboard |
| `starboard/` | system shell client; `home.c` grid/dock, `status.c` status bar and plugin host |
| `protocol/starry-system-v1.xml` | compositor ↔ starboard protocol: layers, scenes, go home |
| `include/starry-plugin.h` | plugin ABI |
| `plugins/` | bundled plugins |
| `launcher/` | `starry-launchd`: starts apps for starboard, packages under their own uid |
| `tools/` | `starry-shot`: screenshots through wlr-screencopy |
| `data/starry-run.in` | starts the compositor through `seatd-launch` |

## Building

Dependencies: wlroots 0.20 (DRM, libinput, GLES2, GBM, session), wayland,
wayland-protocols, xkbcommon, freetype2, libpng, the Constellation client
library, meson, ninja, wayland-scanner.

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
wlr-screencopy only to root clients and to starboard.

With a `starry` user (in the groups `seat` and `haptics`), the service runs
the compositor and starboard as that user. `seatd` hands them the display and
input devices, so they never open `/dev/dri/card0`, `/dev/fb0` or the
touchscreen themselves; `starry-launchd`, as root, starts apps for starboard
and answers only the `starry` user. `data/udev/70-starry-haptics.rules` gives
the vibration motor to `haptics`. Without that user, everything runs as root
through `seatd-launch`.

To start Starry at boot on a sysvinit system, link the bundled service and enable it:

```sh
ln -s /opt/starry/share/starry/sysvinit/starry.init /etc/init.d/starry
update-rc.d starry defaults 99 20
```

The app catalogue lives in `<prefix>/etc/starry/apps`, one app per line
(`Name|#RRGGBB|dock(0/1)|shell command`), for development; installed packages come from cosmic.

Environment overrides: `STARRY_STARBOARD` (starboard binary), `STARRY_APPS`,
`STARRY_PLUGIN_DIR`, `STARRY_DEBUG` (verbose wlroots log), `STARRY_MODE`.

## TV mode

`STARRY_MODE=tv` turns Starry into a TV shell for a remote control. Put it in
`/etc/default/starry`, which the service and `starry-run` read:

```sh
echo STARRY_MODE=tv > /etc/default/starry
```

- The compositor drives the TV at 1920x1080 (up to 60 Hz, even when the set
  prefers 4K) at scale 1; apps get the whole screen, with no status bar.
- starboard shows a home screen of app cards from the same catalogue and
  app registry: favourites (the dock apps) in the top row, the rest below,
  a top shelf with the focused app, and the clock in the corner. The arrow
  keys move the focus (held, they repeat), OK opens the app, Back returns
  to the first card.
- Keys go to the foreground app, or to starboard while the home screen is
  shown. Home (`KEY_HOMEPAGE`) always goes to starboard (through
  `starry_system_v1.key`) and brings the home screen back with that app in
  focus.
- `starry-launchd` passes `STARRY_MODE` to apps, so Stardust apps pick the
  TV variant of `Theme`.
- Home twice (within 380 ms) opens the app menu over the screen: a row of
  the running apps (the compositor's scenes, most recent first), with the
  open app left visible under a dim. Left and right choose, OK switches, Back
  or Home goes back, and holding OK (600 ms), Stop or 0 closes the chosen app.
  Over an app a single Home waits out those 380 ms before going home; on the
  home screen it acts at once.
- Holding Home (700 ms) opens the control centre in the top right corner:
  Sleep, the volume (left and right, or the volume keys; OK mutes), the
  Siri Remote's battery from `/run/siri-remote`, and "Restart or Shut
  Down…". Power or Sleep (`KEY_POWER`, `KEY_SLEEP`) opens the power menu:
  Sleep, Restart, Shut Down; pressed again there, the box goes to sleep.
- Sleep turns the panel off (`starry_system_v1.set_screen`, version 3) and
  asks `lunar-settingsd` to put the TV in standby over HDMI-CEC; any key
  wakes it (that key goes no further) and the TV is asked back (Image View
  On, Active Source). `kill -USR2 $(pidof starry-compositor)` wakes it too,
  from a shell. The volume, CEC and Restart/Shut Down go through
  `lunar-settingsd` as Constellation requests (`cn_ask`), and it lets the
  `starry` user ask for `audio` and `power` only.
- No touch input in TV mode yet.

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
