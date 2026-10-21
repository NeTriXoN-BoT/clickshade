# ClickShade for Linux

The Linux edition uses a native GTK3 settings window and a platform backend. It
is supplied as an x86-64 Linux executable, with complete source. The source and CMake/pkg-config build are distribution-neutral. The supplied binary was
built on Ubuntu 24.04 and needs the ABI and libraries listed below; it is not a
universal binary for every distribution, architecture or Wayland compositor.

## Run

```sh
chmod +x ClickShade
./ClickShade
```

Keep the binary in a stable location before enabling **Start when I sign in**.
That switch creates a per-user XDG autostart entry pointing to this executable.
No startup entry is created just by launching or extracting the program.

The native window has five tabs: **Cursor and rules**, **Highlights**, **Sounds**,
**Presenter** and **Appearance**. Keyboard shortcuts are in **Appearance → Keyboard shortcuts**.
Changes save automatically. GTK follows the display's scale; the
Appearance page adds optional interface zoom. Requested zoom is retained, but
effective zoom is limited on small logical work areas so controls remain
reachable, including when display scaling is already high. Closing settings uses the tray if
the desktop supports it, otherwise minimizes the window so it remains reachable.
Use **Quit** to stop the application and its effects.

```sh
./ClickShade --background  # Start in the tray or minimized taskbar window
./ClickShade --restore     # Pause and restore through the running instance
./ClickShade --quit        # Quit the running instance
```

Normal application uniqueness uses the desktop session D-Bus service. Run the
program as your ordinary desktop user, not through sudo.

## Session support

| Capability | Native X11 session | GNOME Wayland companion |
| --- | --- | --- |
| Pointer highlight in five styles (filled, ring, glow, crosshair, spotlight) with breathing and idle hide | Supported with a compositing window manager; the spotlight dims the pointer's monitor through one window that is redrawn only around the circle | Implemented by the bundled Shell extension; the spotlight is drawn as a glow |
| Left, right and middle click effects in six styles (ring, pulse, ripple, burst, neon, target) | Supported with a compositing window manager | Implemented by the bundled Shell extension |
| Keyboard sounds (set folders with key variants, space, enter and backspace) | Raw key presses are subscribed only while the option is on and reduced to a coarse category | The extension forwards the same coarse category; sounds play in the app |
| Optional Dots, Rings, Sparkles, Comet and Fine sparkle trails | Supported, bounded to 32 stamps; off by default | Not implemented |
| Shake to find the pointer | Optional Plasma-style or macOS-inspired enlargement of cached native cursor pixels | Not implemented; use a compositor-provided locator |
| Magnifier lens (zoom, size, shape, frame color or no frame) | A lens beside the pointer, read from the screen about 30 times a second with the current cursor drawn in; needs a 24/32-bit display | A square lens beside the pointer cloned from the window layer; the Shell's own chrome is not part of it |
| Key display (shortcuts only or every key, mouse buttons, position, size, hold time, colors) | Raw key presses and releases are subscribed only while the option is on; keycaps and typed words exist only while shown | Shell captured key events become St keycaps; same rules |
| Animation rate | Auto adapts effect work within the monitor limit; manual 30–360 FPS | Auto follows compositor sampling; manual 30–360 Hz sampling cap |
| Global shortcuts | Configurable X11 passive grabs | Not implemented |
| Current pointer dark while pressed / brief flash | Temporary software inversion; system artwork is not permanently changed | Unavailable |
| Cursor profile replacement, reverse mode, custom cursor files and cursor resizing | Unavailable; preferences retained in the UI | Unavailable; preferences retained in the UI |
| Cursor look (opacity, soft shadow, fill and outline colors) | Unavailable (Windows only) | Unavailable (Windows only) |
| Fullscreen, application exclusions and allowances | Supported according to desktop window properties | Supported through Shell window information |
| Generic pointer confinement detection | Unavailable | Unavailable |
| Left, right and middle click sounds | PCM/float WAV streaming through a PulseAudio-compatible audio server | Same worker through the running native app and extension click notifications |
| Other Wayland desktops | Never routed through Xwayland as if it offered global access | Unsupported; the settings window explains the limitation |

The X11 backend requires **XInput2 2.1 or newer** and uses input-transparent ARGB
overlay windows. Version 2.1 delivers raw release events during another
application's implicit button grab, which is necessary for reliable hold and
overlapping-button behavior. It also requires XFixes 4 or newer.
A compositing manager is needed for transparent visuals. Cursor hiding belongs
to the backend's X connection; closing that connection releases it. The app does
not rewrite a named cursor theme or attempt to replace every application's
private cursor resource.

Pressed-cursor inversion is software-rendered and follows compositor timing;
it does not have the Windows edition's hardware-cursor presentation path.
Inversion and shake enlargement are disabled when the server has multiple independent master pointers,
because XFixes hiding would affect unrelated pointer sprites. Fully transparent
cursor images pause effects, but X11 cannot reveal every application's pointer
confinement or its separate XFixes cursor-hide requests. Keep fullscreen pause
enabled and add explicit exclusions for windowed games.

The GNOME adapter is experimental. It must be installed and enabled separately;
see [the extension instructions](gnome-extension/README.md). Pointer/button
sampling can miss a press shorter than one sampling interval. GNOME Shell and a
Wayland compositor were not available in the build environment, so successful
source and mock checks do not establish real GNOME runtime compatibility.

The active backend's capabilities and errors appear under
**Appearance → This session**. Activity counters and diagnostic logging are not exposed. Unsupported cursor controls are disabled and
explained, while their saved values are retained.

## Sound files

Each mouse button has its own selected file, **Test**, **Clear**, and sound-library
dropdown. The dropdown lists actual WAV files under the executable-relative
`sounds/` folder: the original Soft, Glass, Wood, Tick, Bubble and Arcade sets plus
the Classic, Mechanical, Pop, Laser and Retro sets, each with left/right/middle
files. Add your own WAV files there and choose **Refresh list**.

**Keyboard sounds** have their own switch, volume and set picker. A set is a folder
under `sounds/keyboard/` (Classic, Mechanical Blue, Mechanical Brown, Laptop and
Typewriter ship in the release) holding `key-1.wav` to `key-6.wav` plus optional
`space.wav`, `enter.wav` and `backspace.wav`; a single WAV also works. Variants are
shuffled so fast typing never repeats one sample. On X11 the backend subscribes to
raw key presses only while the switch is on, reduces each press to one of four
categories and discards it; on GNOME the extension sends the same category over
D-Bus. No key identity or text is ever stored or logged.
Scanning is bounded to two nested folder levels, 2,048 inspected entries and 256
files. It runs when the page is built or explicitly refreshed, never on a click.
The file chooser always starts in that `sounds/` folder; you can navigate
elsewhere. Cancel preserves the selected file, and choosing a file does not
turn on the sound master switch.

Mono/stereo PCM or floating-point RIFF WAV at 8–192 kHz is supported. There is no
application limit of ten seconds or ten MiB. A 100 ms initial prefix is decoded
in advance; longer files stream through fixed read-ahead windows on the audio
worker. Up to twelve voices overlap with bounded memory, and mouse and keyboard
voices carry separate live volumes. RIFF's own 32-bit chunk
sizes still apply; RF64 and compressed formats are not implemented here.
Volume edits affect subsequent mixed buffers of an active sound without
restarting it; zero volume stops/flushes voices. Pause and Quit also flush them.
Ordinary regular-file I/O may still be delayed by a failing or remote filesystem.
Named pipes and devices are rejected before reading.

The audio server may be PulseAudio or PipeWire's PulseAudio compatibility
service. Missing audio support reports an error without disabling visual
effects. Audible playback and physical output latency still need target-device
verification.

## Shake and animation

**Cursor and rules → Shake to find the pointer** is off by default. Plasma-style
uses published KWin 6.8.1 gesture/timing behavior with ClickShade's configurable
maximum. The macOS-inspired preset implements the visible shake-to-locate idea;
Apple does not publish an exact detector or timing contract. Sensitivity and
maximum enlargement are adjustable. Held-button drags cannot trigger a shake.
Rules, session pause, monitor transitions, disable and quit restore the normal
pointer. The locator uses current OS coordinates without warping the pointer or
changing acceleration.

X11 magnifies cached pixels of the **actual current cursor**, preserving its
aspect ratio, transparency, hotspot and click-time inversion. This differs from
KWin's themed-arrow replacement. A bounded cache (up to 32 sizes, 24 MiB total)
is rebuilt on cursor/configuration changes, not on each animation frame. An
exceptionally large cursor that exceeds the cache bound remains native and a
capability error is shown. The animation finishes and the worker has no idle
render timer. GNOME Wayland shake/enlargement is explicitly unavailable in this
companion; it does not alter a global cursor-size setting as a substitute.

**Highlights → Animation** offers Auto (adaptive) and a manual 30–360 FPS control.
On X11, Auto observes only ClickShade's own drawing cost, reduces particle/trail
work after sustained overload and recovers with headroom. The cached halo can
continue following at the monitor limit. No benchmark, hardware inventory or
network request is performed. GNOME Auto is compositor-controlled sampling,
not this X11/Windows workload policy. These are work/submission limits, not a
promise of presented frame rates on every device.

## Desktop behavior and shortcuts

Effects use the current OS pointer position. Mouse acceleration can be enabled or disabled; ClickShade does not change acceleration, sensitivity, pointer speed or position. The X11 inverse cursor is a software overlay, so presentation follows the compositor even though the real pointer position is untouched.

Fullscreen heuristics distinguish ordinary decorated/maximized windows. **Always allow in these apps** exempts explicitly selected desktop tools from fullscreen pause. Explicit exclusions, lock screens and transparent cursor images still take priority. Root window notifications repair stationary overlay order over newly opened popups; there is no idle redraw loop.

Shortcuts are optional. Restore/pause defaults to **Ctrl+Shift+F10** on X11, and the other eight actions (including **Toggle keyboard sounds**, **Toggle magnifier** and **Toggle key display**) start unassigned. In **Appearance → Keyboard shortcuts**, edit a chord and choose Apply; Clear unassigns it. Conflicts preserve the previous working bindings. Ctrl+Alt+function-key and Ctrl+Alt+Backspace server combinations are rejected. Wayland global shortcuts are visibly unavailable in this version.

Appearance has explicit Dark/Light modes, separate remembered color sets, and **Restore defaults** with confirmation. Reset clears every setting and removes the actual XDG startup entry; it never deletes chosen sound/cursor files. Reset itself creates no backup or activity history. Hidden scrollbar chrome still permits wheel, keyboard and focus scrolling.

## Presenter tools

The **Presenter** tab holds the magnifier lens and the key display. On X11 the
lens always sits above and to the right of the pointer (flipping to stay on the
monitor) because it is read from the screen with `XGetImage`, and a lens centered
on the pointer would capture itself. It shows the last captured cursor image in
place, refreshes about thirty times a second while visible, honors the round or
rounded-square shape, zoom, size and frame color (or no frame), and follows the same pause
rules as the highlight, except that it (like the spotlight) stays put while an
editor shows a transparent cursor as you type. It needs a compositing manager and a 24- or 32-bit
display; otherwise the session card reports why it is hidden.

The key display subscribes to raw key presses and releases only while it (or
keyboard sounds) is switched on. **Shortcuts only** shows chords with Ctrl, Alt or
Super, function keys, Escape, Print and a tapped modifier as rows of keycaps on
rounded panels; **Every key** also shows typing, joining characters into one row in
the case you typed them (current layout with Shift, Caps Lock and AltGr), and
Backspace edits that row. A shortcut repeated within a second adds a count, up to
three rows stack towards the screen edge, and each fades on its own after the hold
time. Rows live only while they are on screen; nothing is recorded. Unlike the other effects it keeps working
while an application shows a fully transparent cursor, because editors do that
while you type; excluded and fullscreen applications and the lock screen still
pause it. On GNOME Wayland the companion extension provides both tools with the
limits listed above.

## Settings

Settings are UTF-8 INI text at:

```text
$XDG_CONFIG_HOME/clickshade/settings.ini
```

When `XDG_CONFIG_HOME` is unset, the location is
`~/.config/clickshade/settings.ini`. Saves replace the file atomically and use
private user permissions. Startup uses `~/.config/autostart/clickshade.desktop`
or the corresponding XDG path.

The `[ClickShade]` key names match the Windows version. Color integers use
`red | (green << 8) | (blue << 16)`, rather than a hexadecimal RGB string.
Linux expects UTF-8; a Windows UTF-16 INI must be converted before reuse. Paths
and executable names are naturally platform-specific.

Settings files carry `SettingsVersion=2`. A file from an earlier preview build is
read as before: its `HighlightFilled` and `ClickFilled` switches become the matching
`HighlightStyle` and `ClickStyle` values, and click colors that were never changed
from the preview defaults move to neon blue and neon red. Colors you picked yourself
are kept.

### Export, backup and import

**Appearance → Settings profiles** exports a complete Linux profile to a chosen
`.ini` file. **Backup current** replaces one intentional backup at
`clickshade/profiles/settings-backup.ini` under the configuration root. Before a
confirmed import, one `profiles/before-import.ini` backup preserves the previous
preferences. These are explicit settings backups, not activity logs or an
accumulating history.

Profiles have `[ClickShadeProfile]`, `FormatVersion=2`, `Platform=Linux` and a
complete `[ClickShade]` section. Complete format 1 profiles from earlier preview
builds still import, with the same upgrades as a preview settings file. Import accepts bounded regular UTF-8 files,
validates recognized fields, types, ranges and shortcut assignments, then saves
atomically. An invalid profile leaves preferences intact. Shortcut registration
failure retains the previous bindings; persistence failure rolls them back.
The actual start-at-sign-in preference/registration is preserved on import.
Profiles do not copy cursor/sound assets, so paths may need reselection on a
different computer. Windows profiles are deliberately rejected rather than
silently applying incompatible paths and platform-specific settings.

## Build from source

On Ubuntu 24.04 / compatible Debian-family systems, install the development
dependencies using your normal package manager:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libpulse-dev \
  libx11-dev libxi-dev libxfixes-dev libxrender-dev libxext-dev \
  libxrandr-dev libcairo2-dev
```

Package names above are one example. On other distributions, install a C++17
compiler, CMake or a POSIX shell, pkg-config, and development packages providing
`gtk+-3.0`, `gio-2.0`, `x11`, `xi`, `xfixes`, `xrender`, `xext`, `xrandr`, `cairo`
and `libpulse`. Tests additionally use `xtst` and a `dbus-daemon` executable.
No Ubuntu path is embedded in the app or required by the normal build.

Then, from the source root:

```sh
sh linux/build.sh --test
./build-linux/ClickShade
```

The script compiles C++17, links normal system libraries, and never installs or
downloads anything. `CXX`, `CXXFLAGS`, `LDFLAGS`, and `PKG_CONFIG` are supported.
`CLICK_SHADE_LINUX_BUILD` selects the output directory. Both build scripts and
CMake copy the bundled `sounds/` tree beside the executable so the library is
available in source builds too.

CMake is also supported:

```sh
cmake -S . -B build-linux-cmake -DCMAKE_BUILD_TYPE=Release -DCLICKSHADE_BUILD_TESTS=ON
cmake --build build-linux-cmake -j
ctest --test-dir build-linux-cmake --output-on-failure
```

All test build paths need a `dbus-daemon` executable for the private mock-bus test.
The CMake test build and the Python builder's `--test` option also need
`libxtst-dev`. CMake builds `ClickShadeX11Test`,
which requires a compositing X11 test desktop and an explicit `--run` argument.
CTest runs seven headless targets: settings/profiles, streaming WAV/audio boundaries, shortcut syntax, portable layout geometry, adaptive frame pacing, shake detection/timing, and the native GNOME bridge against a private mock D-Bus service. No live GNOME Shell is contacted. The shipping GUI has no self-test or status-dump mode. The Python builder's
`--test` option also builds `build-linux/x11_smoke` without executing it.

The optional `tools/build_linux.py` offers the same native build plus
`CLICK_SHADE_LINUX_SDK` / `--sdk` for isolated CI SDKs. It embeds no SDK runtime
path and downloads nothing. The release does not bundle the compiler, GTK,
GLib, X11, Cairo, or the audio server.

## Runtime dependencies and testing

The binary needs GTK 3.24, GLib/GIO, Cairo, X11, XInput2, XFixes, XRender,
Xext, XRandR, the C++ runtime, and glibc from a compatible x86-64 system.
PulseAudio's client library is loaded dynamically for sound support.
Ubuntu's GTK3 runtime package is `libgtk-3-0t64`; package names differ on
other distributions. The supplied executable requires **GLIBC_2.38** and
**GLIBCXX_3.4.32** (GCC 13's C++ runtime), as confirmed from its ELF version
requirements. Ubuntu 24.04 satisfies these requirements. **Ubuntu 22.04 needs a
source rebuild**; copying newer runtime libraries into system directories is not
the recommended way to run this binary. Rebuild from source on other
distributions with an older ABI.

`build-linux/settings_test` is a separate developer executable for settings parsing, persistence, bounds, UTF-8 paths, reset and startup rollback. The headless suites cover complete profile validation, Unicode persistence, long-file streaming and live gain, shortcut syntax, private-bus queue/lifecycle behavior, layout, frame pacing and shake timing. These test executables are not distributed in the normal application folder.
Interactive X11 checks require a disposable composited desktop (for example Xvfb
with Openbox and Picom) and must be explicitly requested with `--run`. They can
move the pointer and generate mouse events. They verify inverted overlay pixels,
held/overlapping buttons, flash timing, ordinary app input delivery, fullscreen and
exclusion rules, transparent-cursor and common screen-lock signals, audio-error
isolation, latest-position motion, bounded frames and idle wakes, restoration,
restart, all five trail styles, adaptive/manual timing, both shake presets, scaled
hotspot and click-time inversion during enlargement, stationary popup stacking and
allowed-app priority. Real mixed-DPI monitors, audio hardware, games, session
transitions, and GNOME Shell still require acceptance testing on the target desktop.

Two things the X11 suite deliberately does not claim:

1. Physical compositor/hardware presentation latency and actual audio audibility
   require a real Linux desktop with display and sound hardware.
2. General pointer-confinement and XFixes-hidden-state queries are unavailable in
   the X11 protocol; these capabilities are not claimed or simulated.

Primary protocol references: [XInput2 specification](https://www.x.org/releases/current/doc/inputproto/XI2proto.txt)
and [XFixes specification](https://www.x.org/releases/current/doc/fixesproto/fixesproto.txt).
