# ClickShade on GNOME Wayland

This optional GNOME Shell extension supplies desktop highlighting and click effects for the native ClickShade GTK app on GNOME Wayland. It draws inside the compositor, so it does not depend on Xwayland observing native Wayland applications. Other Wayland compositors do not load GNOME Shell extensions.

**Validation status:** JavaScript syntax and the headless suite (`node linux/gnome-extension/tests.mjs`) pass: configuration, input-state, drawing-lifecycle, style, game detection, hidden-pointer and typing-hold, keyboard-bridge, key-display, lens, app-ownership, audio-rule notification and cleanup checks. The C++ D-Bus adapter compiles with warnings enabled. A real GNOME Shell/Mutter session was unavailable in the build environment; native compositor behavior, mixed-monitor scaling and display latency remain unverified. Treat this backend as experimental.

## Install and enable

Run these commands as your ordinary desktop user from this folder:

```sh
sh ./install.sh
gnome-extensions enable clickshade@local
```

If GNOME has not discovered the newly installed extension, log out and back in before enabling it. To update an installed extension, run `sh ./install.sh` again from the new release, then log out and back in so GNOME Shell loads the new code; disabling and re-enabling keeps running the code already loaded. The installer copies three files into your user extension directory; it does not enable the extension, restart the desktop, alter the cursor theme, or request administrator access.

Open the native ClickShade application and choose **Retry desktop connection** under **Appearance → This session**. The status line under **Effects active** should then name the GNOME Shell extension and its sampling limit. The extension draws nothing until the running app activates it. It watches the app's D-Bus connection, so the effects stop when the app quits, crashes or is killed, and stay off at the next sign-in until the app starts. The lock screen disables and re-enables the extension; afterwards the effects resume by themselves while the app that activated them still runs. Settings are shared through `$XDG_CONFIG_HOME/clickshade/settings.ini`, or `~/.config/clickshade/settings.ini` when `XDG_CONFIG_HOME` is unset. The extension watches the containing directory so atomic saves are detected.

The metadata lists GNOME Shell **45–51** as source targets. GNOME 45 introduced the ECMAScript module format used here. Optional backend, cursor-tracker, stage-view and idle-monitor methods are checked at runtime. The version list is **not** a list of GNOME releases tested in an interactive session; do not force-enable the extension on unlisted versions.

## Available features

| Feature | GNOME backend behavior |
| --- | --- |
| Steady highlight | Filled, ring, glow and crosshair styles with color, radius, opacity and outline thickness; the spotlight style is drawn as a glow because the Shell backend has no monitor-wide shade. Optional breathing (a 2.6 s swell of radius and opacity) repaints the texture about 30 times a second whatever the sampling rate, and an idle session (no input of any kind for about 1.2 s) wakes the extension only at that pace; otherwise a cached Cairo texture only moves with the pointer. Idle hide uses a one-shot timer once sampling has stopped. |
| Click effects | Separate left, right and middle colors and enable switches, ring/pulse/ripple/burst/neon/target styles, radius, opacity, thickness, duration and three easing curves. At most eight particles exist. |
| Keyboard sounds | While the app's keyboard switch is on, each captured key press is reduced to one of four categories (key, space, enter, backspace) and sent over D-Bus; repeats are skipped and nothing is stored. Sounds play in the app. A hidden pointer does not silence them, because editors hide it while you type; fullscreen-game, exclusion and lock rules do. |
| Magnifier lens | A square lens beside the pointer (flipping to stay on the monitor) built from a `Clutter.Clone` of the window layer, scaled 1.25–8× and clipped, with the configured size and frame color, or no frame. The Shell's own chrome and ClickShade's actors are not part of the window layer, so the lens never shows itself. The shape setting is not honored; the real pointer is not drawn inside the lens. A hidden pointer hides the lens like every other pointer effect, except during a typing hold: when the pointer hides within about 1.5 s of a key press, the lens stays in place until the pointer shows again or about 3 s pass without a key. |
| Key display | While the app's key display switch is on, captured key presses and releases become rows of St keycaps on rounded, nonreactive panels at the configured position of the focused monitor's work area. Shortcuts only shows Ctrl/Alt/Super chords, function keys and tapped modifiers; every key also joins typed characters (the event's own character) into one row that Backspace edits; mouse buttons are optional. A repeated shortcut adds a count, up to three rows fade one by one after the hold time, and rows exist only while shown. The row ignores the hidden pointer but obeys the fullscreen-game, exclusion and lock rules. |
| Refresh limit | Auto reads the active stage view's refresh rate when available; otherwise it uses 60 Hz. This is compositor-controlled sampling, not the Windows/X11 adaptive workload policy. Manual limits from 30 to 360 Hz are accepted. These are sampling/submission limits, not guaranteed presented frame rates. |
| Scaling | Geometry follows the GNOME UI scale; the compositor supplies drawing-resource scaling. Mixed-monitor and fractional-scale behavior requires native verification. |
| Application rules | Pauses for a focused fullscreen game, for a hidden pointer when the tracker reports visibility, and for exact application/executable exclusions. A fullscreen window counts as a game when its window class starts with `steam_app_` or is `gamescope`, or its process (`/proc/<pid>/exe`, and for a fullscreen window `/proc/<pid>/cmdline`, each read once per window) runs from a Steam library (`steamapps/common`), is Wine/Proton (an `.exe` on its command line, `wine`, `wine64`, `wine-preloader` or `wine64-preloader`) or gamescope, or runs from a Heroic, Lutris or itch game folder (`~/Games`, `Games/Heroic`, a `lutris` folder, `itch/apps`). Browsers, video players and presentations in fullscreen keep their effects. Explicit allowances bypass the fullscreen pause only; exclusions, lock state and available hidden-pointer checks retain priority. Exclusions accept executable basenames, window classes and application IDs; `.desktop` suffixes and case are ignored. |
| Hidden pointer | When the Shell's cursor tracker reports the pointer hidden, the highlight, click effects and the lens hide, and click sounds and mouse-button labels stop. Editors hide the pointer while you type: if it hides within about 1.5 s of a captured key press, a typing hold keeps the lens in place until the pointer shows again or about 3 s pass without a key. Keyboard sounds and the key display ignore the hidden pointer. The tracker reports the Shell's own pointer visibility; a pointer that an application hides by itself, such as an idle video page in a browser, may not be reported. |
| Click audio | The extension emits button notifications to the running GTK app, which plays left/right/middle PCM or float WAV with a preloaded start and bounded streaming through the shared native audio worker. Rule changes flush active sounds when effects pause; a hidden pointer stops and flushes only click sounds, while keyboard sounds continue. Closing the app stops audio. |
| Idle behavior | No highlight rerasterization during movement. Pointer sampling runs only while a pointer effect is on (highlight, click effects or sounds, the lens, or mouse buttons in the key display); keyboard sounds and the key display alone use captured key events. When the idle-monitor API is available, pointer sampling stops after 1.2 seconds of user inactivity and a one-shot activity watch resumes it. Existing pulses finish first; a breathing highlight keeps its 30 Hz pace. |
| Input/focus | Overlay actors are nonreactive and excluded from the input region. Captured events always propagate. The extension never grabs the keyboard or pointer. |

Global click observation combines available `captured-event` button events with transitions in `global.get_pointer()` button state. GNOME does not deliver every application event through a Shell actor's event hierarchy. A press and release shorter than one sample can therefore be missed; a sampled click origin is the pointer position when that state is observed. This backend does not claim the same event guarantees as Windows Raw Input or the X11 backend.

## Explicitly unavailable capabilities

The GNOME backend does not implement shake enlargement, trails, global shortcuts or a lens centered on the pointer. Use the desktop's own pointer-locator feature when available. It does not replace, invert, resize, hide or synthesize the real cursor. Dark-on-click cursor switching, custom `.cur`/`.ani` cursor files, cursor reverse/hold/flash profiles and confined-pointer detection are unavailable. Their settings remain available in the shared configuration for the Windows backend, but have no effect here. The app's session card lists these limitations even if those settings are disabled.

Game detection covers Steam, Wine/Proton, gamescope and the Heroic, Lutris and itch default folders. A game started some other way keeps its effects in fullscreen; add it to the exclusions instead. There is no promise of universal game/pointer-lock detection or support on other Wayland compositors.

## Disable or remove

```sh
gnome-extensions disable clickshade@local
gnome-extensions uninstall clickshade@local
```

Disabling synchronously removes every overlay actor, timer, signal connection, idle watch, bus name watch, file monitor and exported D-Bus object. In-flight configuration reads are canceled and stale callbacks cannot re-enable the effect. Only the bus name of the app that activated the effects is remembered, so they can resume after the lock screen. The real pointer remains untouched throughout, so there is no cursor theme to restore. ClickShade's **Pause and restore** action clears these visuals and pauses the extension; switching **Effects active** back on resumes it.

## Troubleshooting and verification

Use `gnome-extensions info clickshade@local` to inspect extension state, and the native app's status line and error text for user-facing errors. ClickShade does not write activity or debug logs.

If an optional API is missing, the extension falls back as described above (without the idle monitor, for example, sampling continues while a pointer effect is on) and names the limitation in the state text of `GetStatus`, which the app shows in the status line under **Effects active** (for example "this Shell does not report a hidden pointer"). The separate capability text of `GetStatus` is not shown. An initialization/rendering error pauses effects and leaves ordinary input propagation intact. The GTK application remains usable when the extension is missing or disabled.

The headless suite is available in the complete source repository, separate from the runtime extension files. From the source root:

```sh
node linux/gnome-extension/tests.mjs
```

It checks INI parsing and limits, easing, frame-deadline behavior, left/right event deduplication, sampled clicks, actor bounds, cache reuse, rules, game detection, the hidden pointer and typing hold, scaling, idle wakeup, the 30 Hz breathing pace, sampling only for pointer effects, key labels and the key display row, lens placement, app ownership of the effects (activation, the app leaving the bus, the lock screen) and deterministic cleanup against mocked Shell APIs. `AllowedChanged` and `MouseAllowedChanged` assertions cover audio eligibility before and after activation, fullscreen pause/resume, keyboard sounds while the pointer is hidden, a reconnecting client receiving the current state, and false being published before disable unexports the bridge. These checks verify notifications rather than audio output. They cannot establish native ABI compatibility, compositor stacking, visual quality, audio latency, mixed-DPI behavior or performance. Native acceptance should include a Wayland application and an Xwayland application, both mouse buttons, focus/click-through, fullscreen entry/exit, an excluded application, monitor changes, and extension disable/re-enable.

## Bridge contract

Service `org.gnome.Shell`, object `/org/gnome/Shell/Extensions/ClickShade`, interface `org.gnome.Shell.Extensions.ClickShade1`:

| Method or signal | Signature | Purpose |
| --- | --- | --- |
| `SetActive` | `b → ()` | Enable or pause effects without changing saved settings. The caller's unique bus name is watched after `true`; the effects pause when it leaves the bus. |
| `Reload` | `() → ()` | Request an asynchronous bounded INI read. |
| `Restore` | `() → ()` | Clear current effects and pause. |
| `GetStatus` | `() → (bbsss)` | Active flag, required API availability, state, error and capability text. While active, the state also lists limits of the Shell build. The app shows the state and error, ignores the capability text and also accepts `(bbss)`. |
| `Click` signal | `(u)` | Observed enabled-sound button mask: 1 for left, 2 for right, and 4 for middle. |
| `Key` signal | `(u)` | Coarse key category while keyboard sounds are on: 0 key, 1 space, 2 enter, 3 backspace. Never the key itself. |
| `AllowedChanged` signal | `(b)` | Eligibility under every rule except the hidden pointer; false flushes active native audio voices. Re-emitted on bridge activation so a new client learns the current state. |
| `MouseAllowedChanged` signal | `(b)` | The same plus the hidden pointer, so the app can stop click voices alone while keyboard sounds continue. Re-emitted with `AllowedChanged`; an app that ignores it still works. |

The C++ bridge uses a dedicated GLib context/thread for transport. GUI updates and audio notifications do not perform synchronous D-Bus work on the input path. No key identities, text or window contents are sent to the app.

## Primary API references

- [GNOME 45 extension module format](https://gjs.guide/extensions/upgrading/gnome-shell-45.html)
- [GNOME 49 cursor-tracker API changes](https://gjs.guide/extensions/upgrading/gnome-shell-49.html)
- [GNOME 51 changes and synchronous disable requirement](https://gjs.guide/extensions/upgrading/gnome-shell-51.html)
- [Shell.Global.get_pointer](https://gnome.pages.gitlab.gnome.org/gnome-shell/shell/method.Global.get_pointer.html)
- [Clutter.Actor captured-event propagation](https://gnome.pages.gitlab.gnome.org/mutter/clutter/signal.Actor.captured-event.html)
- [Clutter.StageView refresh rate](https://gnome.pages.gitlab.gnome.org/mutter/clutter/method.StageView.get_refresh_rate.html)
- [St.DrawingArea Cairo texture and repaint lifecycle](https://gnome.pages.gitlab.gnome.org/gnome-shell/st/class.DrawingArea.html)
- [Meta idle watch](https://gnome.pages.gitlab.gnome.org/mutter/meta/method.IdleMonitor.add_idle_watch.html) and [one-shot activity watch](https://gnome.pages.gitlab.gnome.org/mutter/meta/method.IdleMonitor.add_user_active_watch.html)
