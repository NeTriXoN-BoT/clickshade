# Architecture

ClickShade is C++17, with a Win32 app on Windows and a GTK3 app with explicit Linux backends. This document describes the implementation. The [settings guide](SETTINGS.md) covers user-facing behavior and the [effects reference](EFFECTS.md) the platform coverage.

## Windows

| Component | Responsibility |
| --- | --- |
| `src/app.cpp` | Window procedures, command-line handling, the single-instance mutex, elevation relaunch and the entry point. |
| `src/ui_common.h`, `src/ui_layout.cpp`, `src/ui_paint.cpp`, `src/ui_actions.cpp` | The settings window: shared declarations and tables; the card layout engine, control creation and window sizing; custom painting, the sidebar status block and the spectrum line; commands, dialogs, profiles, shortcuts and the tray. |
| `src/runtime.cpp` | Raw Input for the mouse and, only while keyboard sounds or the key display are on, the keyboard; per-device held buttons, modifier-chord bookkeeping for key labels, foreground/session/rule events, cursor hold/pulse, worker dispatch, cached shake animation. |
| `src/cursor_engine.cpp` | Original/released/pressed cursor ownership for every system cursor role; CUR frame selection, alpha-aware resizing, the vector classic arrow, companion discovery, animated-role reloads, installation/restoration. |
| `src/visual_effects.cpp` | Separate renderer thread, QPC deadlines, cached small layered windows, highlight styles and the spotlight shade, click styles, trails, breathing, idle hide, the magnifier lens (Magnification API bound at run time), the key display row, capture affinity, stacking repair. |
| `src/key_display.h` | Key labels and the shortcuts-only policy shared by the input thread and the renderer. |
| `src/click_audio.cpp` | Bounded-prefix WAV/Media Foundation streaming into a sixteen-voice mixer with separate mouse and keyboard gains, event-driven shared-mode WASAPI output and a waveOut fallback. |
| `src/settings.cpp` | Versioned per-user preferences with in-place upgrades, strict profile import/export/backups, transactional reset, Run-key or Task Scheduler startup, elevation helpers. |
| `src/crash_guard.cpp` | Unhandled-exception and terminate handlers: cursor restoration first, a text report plus minidump, bounded automatic restart with a notice. |
| `src/win_event_listener.h`, `src/input_access.h` | Dedicated event-receiver lifetime and conservative foreground integrity checks. |
| `src/frame_pacing.h`, `src/shake_locator.h` | Shared in-memory adaptive budget and bounded gesture/timing state machine. |
| `src/recovery.cpp` | Idle restoration guard process and emergency saved-scheme reload. |
| `src/ui_scale.h`, `src/hotkeys.h`, `src/version.h` | Pure geometry and shortcut syntax helpers; the single version definition. |

The GUI thread never handles global mouse motion. Input uses Raw Input with `RIDEV_INPUTSINK`, without `RIDEV_NOLEGACY`, low-level mouse injection or synthetic movement. Selected buttons are ORed across devices. A 200 ms reconciliation timer exists only while a button is held, as a missed-release fallback only when foreground integrity makes asynchronous state readable; normal Raw Input release is handled immediately. A higher or unknown integrity level is not evidence of release. Windows move/resize loops do not count as game confinement.

Settings changes are compared by subsystem. Interface colors, extra zoom and startup changes must not restart graphics/audio or interrupt a held cursor. GUI saves and status messages are coalesced. File loading, image preparation, audio decoding and disk persistence stay out of per-movement and per-click dispatch; configuration prepares the caches ahead of use.

### Cursor ownership

The original baseline, configured released appearance, and pressed appearance are independent owned caches. Pause and quit restore the baseline, including when reverse/custom idle mode is active. `SetSystemCursor` consumes the handle passed to it, so installation uses a fresh owned copy. Never pass a shared handle or a cached master.

Static CUR files are bounded and validated before choosing an exact or next-larger frame. PNG/DIB dimensions and hotspots are checked. Alpha images are resized and styled in premultiplied form, then written back with straight alpha, because Windows stores and draws every cursor color bitmap that way (from files and from `CreateIconIndirect` alike); masks and proportional hotspots remain consistent. Unchanged sizes are cloned without a generic resize conversion. Automatic companion discovery only selects a unique same-directory match and does not replace an explicit pressed path.

Current-theme mode covers every system cursor role, including Help, Pin, Person and the pen. The animated busy and working roles are handled separately: copying an animated cursor keeps one frame, so a chosen size reloads the registered `.ani` through `LoadImage` at that size (Windows picks its own 32/48/64 px frames and stretches beyond them), each installation reloads it again rather than copying, press and shake effects leave it alone, and restoration reloads the whole scheme with `SPI_SETCURSORS` instead of reinstalling a flattened snapshot. With a cursor look, the `.ani` is rebuilt in memory: every frame is decoded at the target size, styled, re-encoded as a straight-alpha CUR frame and the RIFF/ACON container is reassembled with the original rate and sequence chunks, then created with `CreateIconFromResourceEx`. The look is applied after inversion and magnification, so a shadow or a color is never inverted. Reconfiguration keeps each role's animated flag. The classic arrow is a vector silhouette with the classic Windows proportions rasterized at the final size, so it matches the original 32 px file and sits in proportion to every other resized shape. An application-owned or software pointer is not necessarily a system role. Local Windows/classic assets are read from the user's OS and never redistributed. A missing profile leaves an editable error rather than killing the runtime.

### Rendering

Highlights reuse a cached DIB and move their own small click-through, non-activating windows. Click particles are bounded to eight; trails to 32. Trail styles rasterize when a stamp is created, then fade immutable pixels. The renderer samples OS cursor coordinates, never a queue of old raw deltas. It does not modify the pointer position, sensitivity or acceleration.

The spotlight is a single layered window over the pointer's monitor with a monitor-sized premultiplied surface. A move restores the shade where the circle was, stamps the pre-rasterized circle at the new position and hands only the union of the two squares to `UpdateLayeredWindowIndirect` as its dirty rectangle, so a move costs about two circle areas of pixel work. Because it is one window, the compositor can never show the dim area and the circle out of step. The surface is freed as soon as the spotlight is switched off. While an application hides the pointer (editors do that while you type), the spotlight and the lens stay where they are; the halo, trails and click rings hide with the pointer.

The magnifier is a color-keyed layered host with a Magnification API child clipped to a circle or rounded square, plus an antialiased frame surface on top; both move in one batch. `magnification.dll` is loaded at run time, so a machine without it keeps every other effect, and a lens failure disables only the lens until settings change. The zoom is the child's `MagSetWindowTransform` matrix, with bitmap smoothing where Windows offers it. Creating the child costs well over 100 ms, so it is created once and later size, shape, frame and cursor changes adjust it in place. The source rectangle is the child's side divided by the zoom, centered on the pointer and clamped to its monitor. The exclusion filter accepts at most 24 windows, so it carries the lens itself, the halo, spotlight and key display, then the click rings and trail stamps that overlap the source square, newest first; a halo is never magnified. While the pointer rests the lens re-reads the screen at 30 Hz; while it moves, at the motion rate.

The key display is one rectangular layered surface holding up to three rows: a shortcut is a row of keycaps, and in every-key mode typed characters join one row that Backspace edits. Typed text comes from `ToUnicodeEx` with its no-state-change flag, using the Shift, AltGr and Caps Lock state reported with the key. Each row is rendered once into its own premultiplied buffer (signed-distance rounded panel, soft shadow, hairline rim, keycaps with a darker lower edge, glyphs rasterized by GDI in grayscale and composited as coverage), and the rows are composited with individual fades. The surface is placed on the foreground window's monitor inside the work area, anchored at its edge so a row that leaves does not move the others. Its rule gate ignores the hidden-pointer pause (editors hide the pointer while typing), so the renderer keeps a second `keyAllowed` flag and hides pointer effects and the key row independently.

Animation uses monotonic QPC time and phase-preserving one-shot waitable-timer deadlines. Late frames skip catch-up work. Auto pacing starts from the monitor refresh rate and adapts the animation budget from short, in-memory work averages with hysteresis. Cached motion keeps a separate display-paced deadline. Manual limits are 30–360; no timer-resolution or display-mode change is made. Automatic/manual pacing is bounded; stationary highlights and fully expired effects need no repeating frame timer. A high-resolution waitable timer is used when available, with a normal timer fallback. No machine-wide timer-resolution request is made.

Separate blocking receiver threads install out-of-context WinEvent hooks. Their callbacks only filter and coalesce notifications; no drawing, input work, file access or foreign-window messaging runs inside them. This isolates accessibility-event floods from the input and render message loops. Stop unsubscribes on the receiver thread even under a continuing event flood.

Out-of-context WinEvent notifications cover relevant window show/reorder/location and menu lifecycle events. Overlap checks repair stacking on active frames. Own-thread events are skipped and bursts coalesced, avoiding a self-triggered stacking loop. Stacking changes never activate an overlay or bypass the common pause gate. Normal desktop z-order is not a guarantee over secure/privileged presentation layers.

### Shake and audio

`ShakeLocator` holds at most 256 turning points in logical OS coordinates. It
compresses same-direction motion, expires old samples and keeps animation time
separate from input arrivals. Published Plasma parameters and original
macOS-inspired parameters are documented in [EFFECTS.md](EFFECTS.md). No upstream
GPL implementation is copied into this MIT project.

Windows prepares at most 1/16-scale Arrow variants, deduplicates image sizes and
caps images/cache memory. Animation selects existing handles; it does not resize
images or open files per input report. A monitor-DPI cancellation must restore
engine magnification as well as controller state. Schedule the next deadline from
the timestamp of the frame actually applied, then convert that deadline against
completion time. Otherwise an API overrun can consume the terminal 1× frame and
leave the cursor enlarged indefinitely. X11 uses the same sampled-time invariant.

WAV files retain a validated source handle/descriptor and a 100 ms decoded prefix.
Each active source has a bounded window, independent of duration. The Windows
worker mixes up to sixteen voices in 10 ms chunks; each voice owns a 100 ms
prefetch ring that is topped up before a chunk is mixed so a live voice is never
padded with silence. Mouse and keyboard voices carry separate gains read per chunk.
Output is event-driven shared-mode WASAPI with automatic format conversion and a
waveOut fallback; the device closes after twenty idle seconds. Media Foundation
produces bounded stereo PCM packets and readers are destroyed before
MFShutdown/CoUninitialize/DLL release. Volume changes affect future chunks; zero
flushes that group's voices. Stop/pause closes the eligibility gate and pending
starts before cleanup. No cursor thread waits for file decoding, playback or the
GUI. An in-flight OS codec/kernel/driver call is not a universally cancellable
operation, so native cursor restoration occurs before optional-worker joins.

Keyboard sounds and the key display register the keyboard usage with Raw Input
only while one of them is enabled. The input thread reduces each key-down to one
of four categories for the audio worker. For the key display it applies the
shortcuts-only policy, tracks which modifiers were held during a chord (so a
tapped Win or Ctrl is labelled on release while Shift+letter typing is not), and
posts the virtual key, scan code and modifier flags to the renderer, which formats
the label from the foreground window's keyboard layout. Nothing about a key is
kept once its label has faded. A set folder provides up to six shuffled key
variants plus optional space, enter and backspace files.

### Rules and shortcuts

The common gate covers enable, engine readiness, lock/suspend/inactive desktop, real cursor visibility, explicit exclusions, and configured fullscreen/confinement rules. Per-app allowances bypass only the latter heuristics. Exclusions and protected/hidden states retain priority. A maximized decorated desktop window is not automatically fullscreen.

Windows shortcuts use `RegisterHotKey` with `MOD_NOREPEAT`. Editable strings normalize to a canonical chord. Bare/Shift-only typing keys are rejected, duplicates/conflicts reported, and a failed edit preserves working bindings. Windows-reserved F12 is rejected before registration. Recording happens only in the focused settings control. No global keyboard hook or key log is used.

### Process lifetime

One instance per desktop session is enforced with a named mutex plus the settings window class; a second launch posts a show request and exits, waiting briefly for a window that is still being created. Administrator mode relaunches through `ShellExecuteEx` with `runas` and a `--replace <pid>` handshake, registers a Task Scheduler logon task with the highest run level for startup, and opens the show/restore/quit messages through the UIPI filter so unelevated launches still reach the elevated window. The crash guard installs an unhandled-exception filter and a terminate handler that restore the system cursors first, write a report and minidump to `crash-reports` under the settings folder (five kept), relaunch once with `--after-crash N`, and stop after three consecutive failures. Overlays declare `WDA_EXCLUDEFROMCAPTURE` when the user hides effects from recordings.

## Linux

| Component | Responsibility |
| --- | --- |
| `linux/app.cpp` | GTK pages/controls, palette modes/zoom, startup/reset, tray, shortcuts. |
| `linux/settings.cpp` | Bounded UTF-8 INI, atomic preference save, XDG autostart. |
| `linux/hotkeys.cpp` | Separate X11 connection, passive key grabs, lock-mask variants, conflicts and repeat handling. |
| `linux/x11_engine.cpp` | XI2 input, XFixes cursor observation/hide ownership, composited small ARGB overlays, the screen-read magnifier lens, the Cairo key display, rules and scheduler. |
| `linux/gnome_adapter.cpp` | Dedicated D-Bus transport/context and audio eligibility for the Shell extension. |
| `linux/gnome-extension/` | Experimental nonreactive Shell actors (highlight, clicks, a cloned-layer lens, key labels) and input observation. |

Detect Wayland before `DISPLAY`; an Xwayland connection does not grant global native Wayland input. Unsupported controls remain visibly unavailable. Source builds are distribution-neutral, but each compiled ELF has an explicit library/architecture minimum. GNOME global hotkeys, trails, cursor substitution, a centered lens and generic Wayland parity are not implemented.

The X11 worker has its own wakeable event loop and connection. Current-cursor inversion uses a software overlay and a connection-owned XFixes hide request. Release restores press inversion; an active shake can continue until its own deadline. Every final locator release/pause/error/exit path balances that hide; connection teardown provides a final fallback. Never use global cursor-name replacement as a shortcut, because names can cover distinct app/DPI/animated variants. Multiple master pointers disable this inversion.

X11 shake prepares a bounded cache of the current static cursor, preserves aspect/hotspot and derives its inverted pair from the same scaled alpha image. It differs from KWin's Arrow-only compositor effect.

X11 trails/clicks use bounded small ARGB windows under a compositor. Root substructure events repair stationary stacking; own-overlay notifications are filtered. No continuous idle animation loop is added. The compositor and software cursor overlay determine presentation latency independently of physical pointer coordinates.

The X11 lens reads its source square with `XGetImage` from the root window, so it must never overlap that square: it always sits beside the pointer and moves further away when a monitor edge would force an overlap. It draws the last captured cursor image into the magnified view and refreshes about 30 times a second while visible. The X11 key display subscribes to raw key presses and releases only while enabled, reads modifier state from XKB, resolves typed characters with `XkbLookupKeySym`, and draws its rows with Cairo, fading each row on its own; its gate skips the transparent-cursor rule. The X11 spotlight is one monitor-sized ARGB window backed by a server-side pixmap: a move redraws the old and new circle in the pixmap and copies that rectangle to the window in one request. The GNOME lens is a `Clutter.Clone` of the window layer inside a clipped chrome actor, which cannot contain the Shell's own chrome or ClickShade's actors; the GNOME key display builds rows of St keycaps, with the stroke rules (typed rows, Backspace, repeat counts, row limit) shared as pure functions in `core.js`.

Xlib error handlers are process-wide. The shared lifecycle mutex in `linux/x11_errors.h` serializes renderer handler install/uninstall with the entire temporary shortcut registration trap; error callbacks never lock it.

X11 shortcuts use explicit passive grabs, not input injection. Grabs include current lock-mask combinations; repeat releases are deduplicated and conflicts roll back. Ctrl+Alt+function-key/Backspace server combinations are rejected. The Linux restore default is Ctrl+Shift+F10; Windows uses Ctrl+Alt+F10.

The GNOME bridge coalesces settings wakes to one pending GLib source and attempts bounded deactivation from worker cleanup, including Stop during initial connection. Audio file loading rejects non-regular inputs through a nonblocking open plus same-descriptor validation.

The GNOME extension observes available captured events plus sampled button state. Very short unsignalled clicks can be missed between samples. It watches the config directory for atomic replacements, keeps actors nonreactive, publishes audio pause eligibility, and synchronously removes actors/timers/signals/watches/D-Bus exports on disable.

## Privacy and limits

Preferences, user-directed exports/backups, a single pre-import backup and an opt-in startup entry are the intended persistent app data. There is no application activity/debug logger, telemetry, network endpoint, automatic update checker, input recording or screenshots. Errors are shown on screen. In-memory counters used by explicit developer checks are not persisted or shown as a diagnostic panel.

Foreground process names are obtained through limited OS metadata APIs for matching rules. This is not process-memory access. Out-of-context accessibility hooks are used on Windows; claiming "no hooks of any kind" would be inaccurate. There is no anti-cheat bypass or universal certification.

## Build and release invariants

Build both binaries after source/resource edits. The packager uses an explicit source manifest, rejects stale/wrong-architecture applications, excludes build debris and compiled tests, preserves Linux permissions, and checks every ZIP entry's SHA-256. CI additionally verifies both build revisions against the checked-out tag.

Test source is included for reproducibility. Test executables require an explicit developer build; interactive suites require `--run` and an isolated desktop. Do not promote Wine/Xvfb results to native Windows/GPU/game certification or GNOME mocks to real compositor coverage.

## Profile transaction

Windows and Linux export complete platform-labelled format 2 snapshots and still
accept complete format 1 snapshots from preview builds, applying the same upgrades
as a settings file without a version. Import requires a bounded regular file,
valid Unicode, the correct platform/version, every schema field, valid ranges and
compatible shortcuts. Unknown/duplicate/incomplete values are errors; preference
loading for ordinary upgrades remains tolerant of missing new keys.

The GUI parses first, asks to replace the current profile, saves one pre-import
backup, applies shortcut registrations transactionally and atomically saves the
candidate. Failure retains or restores working preferences/bindings. Actual
startup registration is a machine choice and is not imported. Only after commit
does the GUI refresh runtime/theme/controls. Backup/export paths are not activity
history; external cursor/audio files are not copied.
