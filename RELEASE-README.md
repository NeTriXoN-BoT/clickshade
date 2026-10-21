# ClickShade 1.0

Cursor feedback, highlights, click effects and sounds for Windows and Linux.
Written in C++ with local settings and no telemetry.

ClickShade is vibe coded: a one-person project in which AI coding assistants
wrote a large share of the code. It has been built, run and tested, but it is a
first release, so please report anything that looks off.

## Windows

Run **Windows/ClickShade.exe** on Windows 10 version 1703 or later, or Windows 11, x64. Keep `sounds/` beside the executable. The app is portable and unsigned.

Start with **Cursor**, then explore Highlight, Clicks, Sounds, Presenter, Rules, Appearance and Shortcuts. Settings save automatically. Closing settings keeps the app in the tray; **Quit** restores the original cursor and exits. **Ctrl+Alt+F10** restores and pauses by default, and you can change that shortcut. Every card and slider has its own reset; **Appearance → Restore all defaults** resets everything and disables startup.

Out of the box a held left or right button darkens your cursor, a yellow highlight follows the pointer, and clicks draw neon blue and neon red rings. Five highlight styles (including a spotlight), six click styles, trails, middle-click effects, click sounds, keyboard sounds, shake-to-locate, a magnifier lens and an on-screen key display are all a switch away.

The Windows cursor choices include your installed static theme, the local Windows arrow, a classic arrow drawn crisply at any size and custom static CUR pairs. Reverse appearance, cursor scale (100% to 600%), the cursor look (opacity, soft shadow, fill and outline colors) and held/brief press are independent; scale and look apply to every cursor shape, busy spinner included, even with the press effect off. App-owned and software-rendered pointers may not use standard system cursor roles.

**Run as administrator** (on Rules) keeps effects and shortcuts working over elevated windows such as Task Manager. **Show effects in screenshots and recordings** is on by default. Only one copy of ClickShade runs at a time. If it ever crashes it restores your cursors first, writes a report to the settings folder and comes back once with a note.

## Linux

Follow [Linux/README.md](Linux/README.md), then run **Linux/ClickShade**. If extraction removed the executable bit:

```sh
cd Linux
chmod +x ClickShade
./ClickShade
```

The native GTK3 x86-64 binary requires compatible system libraries, including GLIBC 2.38 and GLIBCXX 3.4.32. Build the included source on distributions with older libraries or a different architecture.

Composited X11 supports highlights in every style, click effects for all three buttons, all five trails, current-cursor inversion, shake enlargement, a magnifier lens beside the pointer, the key display, click and keyboard sounds, and shortcuts. It requires XI2.1 and XFixes 4. Restore/pause defaults to **Ctrl+Shift+F10**. Windows-specific cursor profiles/reverse/custom pairs/explicit sizing are not implemented on Linux.

The optional GNOME Wayland companion is experimental. It supports a smaller documented set of effects (highlights, click effects, a square lens and the key display) and has no cursor replacement, shake enlargement, trails or global shortcuts. Other Wayland compositors require separate integrations. Unavailable controls are shown as unavailable.

## Useful starting points

- **Cursor:** pick a cursor style and size, enable the preferred shake preset, or adjust held/reversed press behavior.
- **Highlight / Clicks:** choose a style, tune radius, opacity and animation, and select Auto or a manual 30–360 fps effect limit.
- **Sounds:** choose a click set per button and a keyboard set from the dropdowns. Browse starts in the bundled sound directory. Sounds are off initially.
- **Presenter:** turn on the magnifier lens for a demo, or the key display when you record or teach; **Shortcuts only** keeps typed text private.
- **Rules:** allow a desktop app or exclude a game, turn on administrator mode, or hide effects from recordings.
- **Appearance:** change Dark/Light colors, interface zoom, export/import/backup, or restore defaults.

There is no activity/debug logging, telemetry or account. Settings and explicitly requested profile backups stay on your computer. Profiles contain selected file paths, so inspect them before sharing. Pointer speed, acceleration and coordinates are not modified. Keyboard input is observed only while keyboard sounds or the key display are switched on: sounds only distinguish an ordinary key, Space, Enter and Backspace, and the key display keeps a label only while it is on screen.

## Source and publication

**source/** is the complete public repository: code, original assets and sounds, tests, MIT license, notices and build/draft-release workflows.

- [source/README.md](source/README.md): screenshots, features and support matrix.
- [source/docs/SETTINGS.md](source/docs/SETTINGS.md): control ranges and behavior.
- [source/docs/EFFECTS.md](source/docs/EFFECTS.md): Apple/KDE/recorder references and implemented coverage.
- [source/docs/ARCHITECTURE.md](source/docs/ARCHITECTURE.md): how the pieces fit together.

`CHECKSUMS.sha256` covers every other ZIP member. `BUILD-INFO.txt` identifies both binaries. App folders contain no compiled developer tests, debug symbols, settings or caches.

Windows tests ran on Windows 11; the Linux build was compiled and its headless suites run on Ubuntu 24.04; GNOME logic and transport used mocks. No claim of zero latency, universal hardware support, exact proprietary effects, antivirus clearance or anti-cheat certification is made.
