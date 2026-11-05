# ClickShade settings guide

[Back to the project](../README.md) · [Linux guide](../linux/README.md)

## Windows settings

### Cursor

**Your Windows theme** preserves the installed artwork, including static custom Windows cursors. The engine prepares independent released/pressed images for supported standard roles: arrow, text, hand, crosshair, resize, move, prohibited, and up arrow.

**Windows arrow** reads `%WINDIR%\Cursors\aero_arrow.cur` from the user's own Windows installation. That file is not redistributed. **Classic arrow** uses the local Windows `arrow_r.cur` artwork when that file has an image of exactly the size needed. Any other size, including a larger pointer size or an explicit cursor scale, is drawn directly at that size from the original classic-style silhouette instead of resampling the file, avoiding enlarged bitmap stair-steps. An independently drawn fallback is used when the local file is missing. These two choices replace the **arrow**; the other supported shapes keep the installed theme. They are not full imported cursor schemes.

**Custom pair** accepts static `.cur` files. Choose a released arrow and, optionally, a pressed arrow. The picker looks for a uniquely named companion in the same directory (for example, released/pressed or light/dark). It leaves an explicitly selected pressed file alone. Ambiguous names are not guessed. Leave the pressed file empty for automatic color inversion. Missing or invalid files produce an editable error; the application stays open so you can correct the setting or select another profile.

**Reverse light and dark** swaps the prepared released and pressed appearances. With a normal white arrow, this gives a dark idle arrow and a white pressed arrow. For a colored custom cursor, automatic inversion inverts its actual colors; supply an explicit pair for exact artwork.

The loader validates the CUR directory and chooses an exact or next-larger embedded image when resizing, avoiding enlargement of the first low-resolution image. It preserves image proportions, soft alpha edges and the scaled hotspot: images are filtered in premultiplied form and handed to Windows with straight alpha, the form Windows stores and draws cursor bitmaps in. Legacy non-XOR masks are filtered as alpha; true background-XOR shapes keep their paired AND/XOR semantics. Source images cannot gain detail they do not contain. Installed-theme larger CUR frames are used only when their original-size artwork matches the captured baseline, preserving accessibility recoloring.

**Cursor scale** works like the cursor scale of screen-recording editors, but live and on every pointer shape. It is measured from the size Windows draws the cursor at, so 100% keeps the source size whatever pointer size or display scaling you use; 600% is the maximum, and the result never exceeds Windows' 256-pixel cursor limit. Every role, including the animated busy and working cursors, is redrawn from its sharpest embedded image (the Classic arrow is drawn as a shape), preserving aspect ratio and the hotspot. Halo/click radii are in **DIP** and follow the monitor's Windows scaling.

Cursors stay crisp at any pointer size and display scaling. Every cursor ClickShade installs (pressed, scaled, faded, shadowed, recolored, Classic, Windows arrow, custom pair, the shake sizes, and animated cursors that are resized or styled) is loaded from a multi-size cursor file that holds an image at every size Windows draws it, rendered from the sharpest source: the theme's own file, or for built-in cursors the image Windows itself draws. With the soft shadow, each size is built so the finished cursor, shadow included, matches what Windows draws. Your original theme is restored exactly as Windows loads it. These files are written while settings change, never on a click, into a private temporary folder under `%TEMP%\ClickShade\cursors` that is removed when ClickShade quits. Each file stays open for reading while it may be installed, so temporary-file cleaners such as Disk Cleanup or Storage Sense cannot delete it. Folders left behind by a copy that was closed the hard way are removed at the next start of a copy that does not run as administrator. See [SECURITY.md](../SECURITY.md).

When **Windows arrow**, **Classic arrow** or a **Custom pair** replaces an animated arrow, the arrow is static from then on, so the press and shake effects reach it; pausing or quitting brings back the animated arrow.

**Cursor look** changes how every pointer shape is drawn, animated ones included (each frame keeps its timing):

| Control | Range / effect |
| --- | --- |
| Opacity | 30–100%; the whole cursor fades evenly, colors stay as they are |
| Soft shadow under the cursor | A blurred dark shadow offset to the lower right. The image grows to make room and the click tip moves with the artwork, within Windows' 256-pixel limit |
| Paint the cursor in my colors | Light parts take the **fill color**, dark parts the **outline color**, blended by brightness so antialiased edges stay smooth. The pressed cursor is inverted first, so it shows the two colors swapped. Picking a color turns this on |

The chosen cursor, its scale and its look apply whenever ClickShade is active, even with **Change cursor on press** off; reverse mode belongs to the press effect and needs it. An application hiding the pointer does not reset them: the pointer comes back with its scale and look. Genuine background-inverting pixels of old monochrome cursors cannot carry alpha, so such cursors keep their original look. Settings keys: `CursorOpacity`, `CursorShadow`, `CursorColors`, `CursorFillColor`, `CursorOutlineColor`.

**Keep the pressed cursor while holding a button** is on initially. Overlapping selected buttons keep it pressed until the final selected button is released. Turn it off to use the brief-press duration; a pulse can finish while a button is still held and can outlive a very quick release. Another selected press restarts the pulse. Pause, exclusion, and shutdown cancel it immediately.

Window or cursor-shape changes do not themselves release a held button, and neither does the cursor reload after a Windows theme, pointer-size or scaling change: a held button stays pressed through it. The asynchronous missed-release fallback is disabled across unreadable privilege boundaries, where Raw Input release remains authoritative. A Windows move/size loop is not classified as game confinement. Actual Firefox dragging and native administrator-window behavior remain native acceptance cases.

Left, right, middle, and two side buttons can trigger the appearance. Left/right are selected initially. Wheel scrolling does not count as a middle click.

### Shake to locate

Shake is off initially. Choose **Plasma-style** or **macOS-inspired**, set sensitivity
from 50–200%, and maximum enlargement from 150–400%. Higher sensitivity requires
less deliberate movement. Both use current OS cursor coordinates after your mouse
settings; no acceleration, pointer warping or movement interpolation is added.

At 100% sensitivity the Plasma-style preset uses a one-second gesture history,
a 100-logical-pixel span and a path/span ratio above four. It grows to 3× over
200 ms, holds until two seconds after the last detected shake, then returns over
200 ms. Repeated shakes add 1× up to your ceiling. Both transitions use cubic
easing. The cap is a ClickShade safeguard, not KDE's unbounded repeated growth.

The macOS-inspired preset has original tuning chosen to feel like Apple's locator,
which reacts to a couple of quick wiggles: a 600 ms gesture window, 60-pixel span,
ratio above 3, 2.5× starting scale, 120 ms growth, 800 ms hold and 220 ms return.
Apple documents shake enlargement but does not publish its exact timings.

Windows selects prepared Arrow sizes, with a 256 px image ceiling and bounded
cache. X11 scales the current static cursor in a software overlay and balances its
XFixes hide request. Held-button motion does not trigger a new gesture. Existing
animations return normally; pause, exclusions and shutdown cancel immediately.
Shake also works with press-color changes disabled. GNOME Wayland enlargement is
not provided by the companion; use the compositor's own accessibility facilities.

### Highlight

Choose a style, then adjust it:

| Style | What it draws |
| --- | --- |
| Filled | A soft translucent disc under the pointer. |
| Ring | An outline only, so text under the pointer stays readable. |
| Glow | A radial glow that fades out towards the edge. |
| Crosshair | A thin ring with four arms, for pointing at details. |
| Spotlight | Dims the pointer's monitor and leaves a clear circle around the pointer. It hides when an app hides the pointer, except while you type, when it stays in place. |

| Control | Range |
| --- | --- |
| Radius | 8–100 DIP |
| Opacity | 5–100% |
| Ring width | 1–12 DIP (ring and crosshair) |
| Spot radius | 40–400 DIP (spotlight) |
| Dimming | 10–90% (spotlight) |
| Hide after | 500–10,000 ms when **Hide the highlight when the mouse is idle** is on |

**Gently pulse the highlight** fades the highlight between about 55% and 100% of its opacity on a slow 3.2 s cycle. Only the opacity changes, not the size, and the pulse keeps running while the pointer moves; the spotlight does not breathe. On Linux (X11 and the GNOME companion) the pulse takes 2.6 s and also swells the radius slightly. The idle hide restores the highlight on the next movement. Color is independent of the interface palette. **Subtle**, **Presentation**, and **High visibility** presets adjust the halo and click animation while retaining your chosen colors. The spotlight shade stays click-through and is skipped by the GNOME companion, which draws a glow instead.

### Cursor trails

Trails are **off by default**. Enable them on Highlight to add dots, rings, four-point sparkles, a tapered Comet, or Fine sparkle behind the pointer. Choose the color, 2–16 DIP radius, 5–100% opacity, and 80–700 ms fade lifetime.

The trail uses at most 32 small stamps. It samples the current OS pointer position and fades cached artwork; it never adds smoothing, acceleration, or a delay to the real pointer. Stamps expire after movement ends, and the animation timer then stops. Pause/disable clears every stamp. Trails are implemented on Windows and composited X11; the GNOME adapter does not yet provide them.

### Clicks

Left, right and middle click switches and colors are independent of the cursor-appearance button mask. Click effects originate at the press location; moving the pointer does not drag an existing ripple across the screen. The defaults are neon blue for left, neon red for right and neon green for middle; middle clicks are off until you enable them.

| Control | Range / choices |
| --- | --- |
| Shape | Ring, pulse (filled), ripple (trailing rings), burst (sparks flying outward), neon ring (ring with a glow) or target (a ring with four inward ticks that closes in on the click; the center stays clear) |
| Radius | 12–120 DIP |
| Duration | 100–1,500 ms |
| Opacity | 5–100% |
| Ring width | 1–12 DIP |
| Animation feel | Linear, soft ease-out, smooth step |

**Duration** controls how long an effect lasts. **Animation feel** controls its expansion curve. How often effects are redrawn is set by **Frame pacing** on the Appearance page.

### Sounds

Choose separate **left, right and middle** button sounds with a library dropdown
or **Browse**. Browse starts in `sounds/` beside the running executable; you can
then navigate anywhere else. **Test** previews and **Clear** removes a selection.
Each dropdown lists only the bundled files made for that button, plus any file in
the folder without a button name. Playback is on button-down; scrolling is not a
middle-button click. The master switch and app rules still apply. Audio is
disabled initially.

The library contains 33 original mouse sounds in eleven sets: Soft, Glass, Wood,
Tick, Bubble and Arcade from earlier releases, plus Classic (the familiar mouse
click), Mechanical, Pop, Laser and Retro, each with left/right/middle variations.
The paths are `sounds/<style>/<style>-<button>.wav`. Moving the installation with
its sounds folder preserves the library. Missing legacy bundled Soft paths are
migrated without guessing arbitrary custom file locations.

**Keyboard typing** has its own switch, volume and sound set. A set is a folder with
`key-1.wav` to `key-6.wav` plus optional `space.wav`, `enter.wav` and
`backspace.wav`; a single file also works for every key. Five sets ship under
`sounds/keyboard/`: Classic, Mechanical Blue, Mechanical Brown, Laptop and
Typewriter. Key variants are shuffled so fast typing never repeats one sample, held
keys do not repeat, and **Test** plays a variant even while the switch is off.
Keyboard input is observed only while this switch or the key display is on, through
the same Raw Input registration as the mouse, and the sound path only distinguishes
an ordinary key, Space, Enter and Backspace. It never records, stores or sends which
keys you press. Typing sounds keep playing while an application hides the pointer;
click sounds pause with the other pointer effects.

Volume is 0–100% for mouse sounds and separately for keyboard sounds. Long playback
responds to volume changes in subsequent buffers; setting zero flushes that group.
There is no ten-second duration limit. Workers prepare the first 100 ms, stream the
remainder through bounded buffers, and keep at most sixteen overlapping voices on
Windows (twelve on Linux). On Windows an extra start replaces the voice with the
least left to play (a compressed file whose length is not known yet counts as the
longest); on Linux it is skipped until a voice is free.
Audio work does not run on the mouse-input thread. Windows plays through
event-driven shared-mode WASAPI and falls back to waveOut; the output device closes
after twenty idle seconds and reopens on the next sound. On Windows, a sound file
is held open against replacement while its sounds are switched on (or after you
press **Test**, until you next change a sound file, switch a sound group on or
off, or set a volume to or from zero). Switched-off sounds keep their files
closed, so a path from an imported profile is not opened until you switch those
sounds on or press **Test**. A click-sound **Test** opens only the file it plays;
the keyboard **Test** opens the files of the chosen set. Clear or change a selection, switch its sounds
off, or quit, before editing the same file in place.

Both platforms directly decode mono/stereo PCM or IEEE-float RIFF WAV. Windows
also uses installed Media Foundation codecs for MP3, WMA, FLAC, M4A and AAC. Linux
uses WAV. RIFF's own 32-bit chunk limits still apply. Unavailable codecs/devices
and malformed files show an audio error without disabling cursor effects.

### Presenter

**Screen magnifier** shows a live lens that follows the pointer. It is off by
default.

| Control | Choices / range |
| --- | --- |
| Shape | Round, or a rounded square |
| Placement | Centered on the pointer, or beside it (above and to the right, flipping to stay on the monitor) |
| Zoom | 125–800 % |
| Lens size | 80–600 DIP (the lens follows the monitor's scaling) |
| Frame | On by default; off leaves the lens without a colored edge |
| Frame color | Any color; the thin antialiased frame also hides the lens edge |

On Windows the lens is a window of the Windows Magnification API, so it shows
the real screen underneath, including video, at the display's pace while the
pointer moves and about thirty times a second while it rests. ClickShade's own
highlight, trails, click effects and key display are excluded from the magnified
view, and the lens stays click-through. **Beside** also shows the pointer inside
the lens; **Centered** leaves the real pointer on top. If the Magnification API is
unavailable in a session the Presenter card reports it and every other effect
keeps working. The lens follows the same pause rules as the highlight: it hides
when an application hides the pointer, except while you type, when it stays where
it is so it does not flash with every word. On Linux the
lens always sits beside the pointer; see the [Linux guide](../linux/README.md).

**Key display** shows the keys you press, for recordings and teaching, in the style
of the Mac keystroke visualizers.
It is off by default.

| Control | Choices / range |
| --- | --- |
| What to show | **Shortcuts only**: chords with Ctrl, Alt or Win, function keys, Esc, Print Screen and a tapped modifier. **Every key**: letters, numbers and everything else too |
| Also show mouse buttons | Left, right, middle, back and forward presses, with held modifiers |
| Position | Bottom center, top center, bottom left, bottom right of the active window's monitor, inside its work area |
| Text size | 60–250 % |
| Hold time | 300–6,000 ms after the last press, then a short fade |
| Opacity | 20–100 % backdrop opacity |
| Backdrop color | Any color; the text color is chosen for contrast |

Each shortcut appears as a row of keycaps on a rounded panel (**Ctrl** **Shift**
**S**, or **Win** **Shift** **S**) in Windows' own order; letters and symbols come
from the active keyboard layout, named keys use fixed names, and AltGr chords are
shown as AltGr. Pressing the same shortcut again within a second adds a count
(**×3**). With **Every key**, typed characters join into one row in the case you
typed them, and Backspace edits that row. Up to three rows stack towards the screen
edge, newest nearest it, and each fades on its own after the hold time. **Show a
sample** displays an example while the switch is off. Changing the text size,
position, opacity or color does not remove it before its hold time ends, so press
it again to keep tuning against it.

Privacy: keyboard input is observed only while the key display or keyboard
sounds are on, through the same Raw Input registration as the mouse. The identity
of a key travels to the renderer only to become a keycap or a typed character,
which exists in memory while its row is on screen and nowhere else; nothing is recorded, stored or sent.
**Shortcuts only** never shows typed text. The key display keeps working whenever
an application hides the pointer, for typing, a video or a game alike; excluded
apps, the fullscreen-game and confined-pointer rules and the lock screen still
pause it.

Both tools have entries in the tray menu and their own global shortcuts.

### Rules

| Rule | Default | Meaning |
| --- | --- | --- |
| Fullscreen game pause | On | Pause when a recognised game covers a monitor. Other fullscreen applications keep their effects. |
| Confined-pointer pause | On | Pause for an application pointer clip, excluding ordinary Windows move/size loops. |
| Hidden/suppressed pointer | Always observed on Windows | Hide the pointer effects and pause click sounds while an application hides the cursor. The key display and keyboard sounds keep working. |
| Executable exclusions | Empty | Pause for configured executable basenames. |

**Pause all effects in fullscreen games** pauses only games: browsers, video players, presentations and other fullscreen applications keep their effects. A fullscreen window counts as a game when it uses exclusive fullscreen, is on Windows' own game list (the one Game Bar uses), or runs from one of the usual game library folders: Steam, Epic, GOG, EA/Origin, Ubisoft, Riot, Xbox, Rockstar, Amazon and itch. Add a game it does not recognise under **Excluded applications**. On Linux, the X11 backend and the GNOME companion recognise games run through Steam, Proton/Wine, gamescope, Heroic, Lutris and itch.

Use **Add app** or semicolon-separated names such as `game.exe; another-game.exe`. Matching is case-insensitive and uses the executable basename, not a window title.

**Always allow in these apps** bypasses fullscreen and confined-pointer rules for explicitly chosen desktop applications. Exclusions take priority if an app is in both lists. Session locks, inactive desktops, and intentionally hidden Windows pointers still pause the effects. Ordinary captioned/resizable maximized Windows applications are not classified as fullscreen merely because they fill a screen.

When an application hides the pointer (a video player such as YouTube, or a game), every pointer effect hides with it: the highlight, the spotlight, trails, click effects and the magnifier. Click sounds, the pressed cursor and shake to locate pause too. Firefox's transparent cursor (CSS `cursor: none`) counts as hidden, like a hidden or empty cursor in other apps. The exception is typing: when the pointer disappears within about 1.5 seconds of keyboard input, as it does in many editors, the spotlight and the lens stay in place so they do not flash with every word. They hide about 3 seconds after the last input, or as soon as the hidden pointer moves, and follow the pointer again when it shows. ClickShade only learns the time of the last input from Windows, not which key it was, so a player that hides an idle pointer after a few seconds hides everything. It also notes its own mouse input (movement, buttons and the wheel, even while the pointer is hidden), and the hold starts and continues only while the newest input was not the mouse, so clicking into an app, scrolling or a game's mouse look never keeps the spotlight or the lens on screen. The key display and keyboard sounds keep working while the pointer is hidden, and a customized cursor comes back with its scale and look.

These are practical rules, not a universal game detector. Windowed games with visible, unconfined pointers, and fullscreen games the rule does not recognise, need an explicit exclusion. Overlays stay above ordinary desktop applications without activating or consuming clicks. They do not force drawing into secure desktops, anti-cheat protected rendering, or exclusive fullscreen surfaces.

**Start with Windows** creates an entry only for the current user, launching with `--tray`. **Open in the system tray** also hides settings on manual launches. Startup remains opt-in.

**Run ClickShade as administrator** relaunches the app elevated (one UAC prompt per manual start). Windows normally prevents an ordinary app from drawing over, reading the button state of, or receiving shortcuts next to elevated windows such as Task Manager or installers; elevation removes that limit. A declined prompt continues unelevated with a note on the Rules page. The elevated instance still accepts the show, restore and quit requests of an unelevated launch, so starting ClickShade again opens its settings window.

With this on, **Start with Windows** uses a Task Scheduler task with the highest run level instead of the Run key, so sign-in shows no prompt, but only when `ClickShade.exe` is in a protected folder. That task starts the file elevated without asking, so the file, its folder and every parent folder must be owned by Administrators, SYSTEM or TrustedInstaller, and their permissions must not let your account without administrator rights, or any other account or group, change them. The drive must be fixed and local, with no junction or symbolic link in the path. Program Files qualifies; a ZIP extracted to Downloads, the Desktop or a folder you created on `C:\` does not. Elsewhere ClickShade keeps the normal Run entry, Windows shows a UAC prompt at sign-in, and **Administrator windows** on the Rules page says why. A task registered for such a folder by an earlier version is replaced by the Run entry at the next elevated start. To start without a prompt, copy the ClickShade folder into Program Files (or extract the ZIP there), start that copy and switch **Start with Windows** on again; the switch shows off for a copy in a new folder. A folder you move into Program Files on the same drive keeps your account as its owner and still counts as unprotected. The task runs at normal priority.

An elevated ClickShade also checks a startup task that starts another copy. If that copy is in an unprotected folder or no longer exists (for example a 1.0 folder in Downloads), the task is removed and **Administrator windows** says so; switch **Start with Windows** on in the copy you use. A task for another copy in a protected folder is left alone. Turning administrator mode on in a copy that already runs elevated (for example through Explorer's **Run as administrator**) moves the startup entry to the task right away.

**Show effects in screenshots and recordings** is on by default: OBS, ShareX, the Snipping Tool, Discord and similar tools capture the overlays together with your screen. Turn it off to exclude every overlay from capture (Windows 10 version 2004 or later); the effects remain visible on your display.

### Appearance and scaling

Choose **Dark** or **Light** explicitly. Each mode remembers its own background and accent colors. Presets and custom colors remain available; effect colors are independent. Surface and text colors are derived for readability.

Windows Per-Monitor V2 DPI awareness is always enabled. **Automatic display and resolution scaling** adds extra GUI zoom when the monitor's logical work-area height is large: 115% at 1,280 DIP or more, 150% at 1,800 DIP or more. This avoids double-enlarging a 4K desktop that Windows already scales to a normal logical size. You can choose 100–200% extra zoom instead. Zoom is constrained to the available work area; all settings remain scrollable and keyboard-focusable, with scrollbar chrome hidden. Use the wheel, Page Up/Down, Home/End, or Tab to reach controls; focus scrolls into view.

The GUI retains native checkbox, trackbar, edit, keyboard, and focus behavior under the custom painting. Sliders allow precise numeric entry beside them and ignore the mouse wheel, so scrolling a page never changes a value. Cursor-size and GUI-zoom changes commit after a thumb drag to avoid rebuilding cursor/font resources repeatedly. Visual sliders update live; disk writes are debounced.

The window opens wide enough for two card columns and tall enough for its tallest page where the desktop allows; narrower windows fall back to one column. **Animated spectrum line** controls the slow color sweep along the footer. It redraws only its own thin strip, and only while the window is visible on the current virtual desktop, so an open settings window costs little CPU.

**Frame pacing** sets how often ClickShade redraws its effects: **Auto** (adaptive) or a manual limit of 30–360 fps.

| Effect | Auto | Manual limit |
| --- | --- | --- |
| Click effects and trails | Adapts to ClickShade's measured drawing cost, up to the monitor's refresh rate | Capped at the limit |
| Highlight, spotlight and magnifier following the pointer | The monitor's refresh rate | Capped at the limit |
| Breathing highlight, idle magnifier refresh, key display fades | Their own fixed timing | Not affected |
| The pointer itself | Drawn by Windows | Not affected |

An effect reaches the screen a moment after ClickShade reads the pointer, and a fast pointer has moved on by then. So during steady fast movement the highlight, spotlight and magnifier are drawn slightly ahead, where the pointer is heading. The lead stays steady with mice that report less often than the display refreshes. It fades out when the pointer slows down, brakes or turns, so precise aiming is never shifted, and it drops as soon as the pointer stops, so the effects settle exactly on it within about one report of the mouse. Trails and click effects always use the real positions. This works the same at any refresh rate and with a manual limit.

A higher limit costs more work while effects are active; it does not alter your display mode or hardware mouse rate, or guarantee that each submitted update is presented on screen. Auto starts from the current monitor rate (bounded to 30–360), observes actual drawing cost in memory, and lowers or recovers the animation budget gradually. No hardware inventory, stress benchmark or saved history is collected. On the GNOME companion, Auto uses compositor timing instead of claiming a measured hardware budget.

## Background, restoration, and removal

Close/minimize keeps the Windows application in the tray. Only one copy runs per desktop session: launching another, from any folder or version, brings the existing settings window to the front instead, also when the running copy is elevated. The tray icon's tooltip shows whether effects are enabled or paused; a click opens settings and a right click opens the menu, which offers master/highlight/click/sound/keyboard/magnifier/key display toggles, restoration, and Quit. The icon comes back by itself after Explorer restarts.

On Windows, the **Shortcuts** page configures nine actions: toggle all effects, highlight, clicks, click sounds, keyboard sounds, the magnifier or the key display; show settings; and restore/pause. Linux puts the same actions under **Appearance → Keyboard shortcuts**. A blank field disables that shortcut. Windows supports recording a chord as well as typed entry; Linux X11 uses typed chords with Apply/Clear. Conflicts are reported without replacing a working binding. Global Wayland shortcuts are not implemented.

The default restore shortcut is **Ctrl + Alt + F10 on Windows** and **Ctrl + Shift + F10 on Linux X11**. Other actions have no default shortcut. Linux rejects Ctrl+Alt+function-key combinations used by virtual terminals. Windows rejects F12 because the OS reserves it for debugging. If the default is occupied, choose another chord, use the tray, or on Windows run:

```text
ClickShade.exe --restore
```

**Reload cursors** restores the baseline, reloads the saved Windows scheme, and prepares the selected appearance again. Use it after changing the Windows theme or accessibility size if an automatic refresh did not catch the change. If the engine could not start, this button becomes **Retry engine** and retries after safely cleaning up the previous attempt.

Normal pause/quit restores the original user cursors exactly as Windows loads them from the theme, even when reverse mode or a custom released appearance is active, and removes ClickShade's temporary cursor files on quit. A second, idle guard process waits for an unexpected main-process exit and attempts `SPI_SETCURSORS` restoration. The guard cannot restore anything if both processes are terminated.

If ClickShade itself crashes, its crash handler restores the system cursors first, writes a short text report and a minidump to `crash-reports` inside the settings folder (the five newest crashes are kept, plus `last-crash.txt` for the notice), and restarts the app with a notice on the Appearance page. It restarts up to three times in a row and shows a message on the next crash instead; once a restarted copy has run normally for 90 seconds, the count starts again. The text report contains the version, the kind of fault (with the message of an uncaught C++ exception, which can name a file), the exception code and a stack of module offsets. The minidump also holds the memory of every thread's stack, which can contain fragments of recent input or file paths: look through it before attaching it to a public bug report, or attach the text report alone. Neither leaves your computer unless you attach it. Errors inside individual settings actions are caught separately and reported in the window without ending the process.

Windows settings live in `%LOCALAPPDATA%\ClickShade\settings.ini`. When a `ClickShade.portable` marker file exists beside `ClickShade.exe`, `settings.ini`, `profiles\` and, after a crash, `crash-reports\` are created directly beside the executable instead. Settings files carry `SettingsVersion=2`; files from earlier preview builds are upgraded in place. Startup uses the current-user `Software\Microsoft\Windows\CurrentVersion\Run` value `ClickShade`, or the scheduled task `ClickShade` in administrator mode from a protected folder. Cursor-scheme registry values are not overwritten. To remove: disable startup, Quit, then delete the extracted files and optionally the settings folder. Quit removes its own temporary cursor folder under `%TEMP%\ClickShade\cursors` itself; the empty `cursors` and `ClickShade` folders above it stay.

## Restore every default

Every card has its own **Reset** button, enabled only while something in that card differs from its default, and every slider has a small reset arrow. Appearance also has **Restore all defaults** with a confirmation. It resets every saved option, both theme color sets, shortcuts, trails, paths and application lists, and disables the actual per-user startup entry. It does not delete selected cursor or audio files. Cancel changes nothing. If persistence/startup removal fails, the app reports the error and attempts to retain the previous working configuration. Reset does not create an activity log or erase existing profile exports/backups.

## Export, import and backup

Use **Appearance → Settings profiles**. **Export** writes a complete versioned
profile to a chosen file; **Back up current** writes one stable manual backup.
**Import** validates format, platform, completeness, numeric ranges, strings and
shortcuts before applying anything. It saves the previous preferences to a
separate, single pre-import backup, so repeated imports do not create an unlimited
history. Use Import again to restore either backup.

On Windows, Import also checks the profile's cursor and sound paths. If any point
to another computer or a device (a network share such as `\\server\share`, WebDAV
such as `\\host@SSL\...`, a mapped network drive, `\\?\UNC\`, `\\.\` or `\??\`),
the confirmation lists them and asks: **Yes** keeps them, **No** (the default)
imports the profile without them, and **Cancel** imports nothing. Opening such a
path would sign in to that computer with your Windows account. Paths you pick
yourself in a file dialog are not affected.

Profiles are platform-labelled: Windows profiles import on Windows, Linux profiles
on Linux. They include interface colors, effects, paths, rules and shortcut
preferences. External CUR/audio files are linked, not copied. File paths may need
to be selected again on another computer and may contain account or directory
names, so inspect a profile before sharing it publicly. The repository and release
packages contain no private settings profiles.

Import retains the current computer's actual startup-registration choice. Shortcut
conflicts and persistence failures are reported, and the working settings are
retained or rolled back. Cancelling the dialog leaves the current setup intact.
Backups are user-directed settings files, never activity/debug logs.

## Mouse settings and platform limits

Effects use the cursor position after the OS has applied your chosen sensitivity and acceleration. Both enabled and disabled acceleration are compatible with this approach. ClickShade does not set pointer speed/acceleration, reposition the pointer, or replay mouse movement. Windows uses native static system cursor replacements; X11 cursor inversion is a software overlay and has different presentation behavior.

For the closest match to a configured Windows accessibility/custom theme, select **Your Windows theme** and a **Cursor scale** of 100%. Each installed cursor carries an image for every size Windows draws it at, so a larger pointer size or a monitor with different scaling stays sharp. Use Reload cursors after changing the OS scheme if an automatic refresh did not catch it.
