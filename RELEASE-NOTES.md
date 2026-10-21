# ClickShade 1.0

Cursor feedback, highlights, click effects and sounds for Windows, with a Linux port.

ClickShade is vibe coded: a one-person project in which AI coding assistants wrote a large share of the code. It has been built, run and tested, but this is a first release, so please report anything that looks off.

## What you get

- **A cursor that reacts.** Hold a mouse button and your Windows cursor turns dark; let go and it is back. Use your installed theme, the Windows arrow, a classic arrow that stays crisp at any size, or your own `.cur` pair. A cursor scale from 100% to 600% applies to every cursor shape, busy spinner included.
- **Make the cursor your own.** Fade it, give it a soft shadow, or paint it in your own fill and outline colors. Every cursor shape gets the same look, animated ones included, with or without the press effect.
- **Find the pointer with a shake.** A Plasma-style preset that follows KWin's published behavior exactly, or a macOS-inspired one that only needs a couple of quick wiggles.
- **Five highlight styles.** Filled, ring, soft glow, crosshair, and a spotlight that dims everything except a circle around the pointer. Highlights can breathe and can hide when the pointer rests.
- **Six click styles and three buttons.** Ring, pulse, ripple, burst, neon ring, and a target that closes in on the click, each button with its own color and switch. Neon blue, neon red and neon green out of the box.
- **Trails.** Dots, rings, sparkles, a tapered comet or a fine sparkle, off by default.
- **Sounds for clicks and typing.** Eleven original mouse sets including the classic click, and five keyboard sets from classic typing to a typewriter. Keyboard sounds only ever learn whether a key, the space bar, Enter or Backspace went down, and only while the switch is on. Nothing is recorded.
- **Presenter tools.** A magnifier lens that follows the pointer (round or square, centered or beside, 1.25× to 8×) and an on-screen key display in the style of the Mac keystroke visualizers: keycaps for shortcuts and, if you like, the words you type, with optional mouse buttons. Nothing is kept once a row fades.
- **Stays out of the way.** Pauses in fullscreen apps, games that confine the pointer and on the lock screen. Exclude any executable, or allow-list the desktop tools that should keep working.
- **Works where it matters.** Optional administrator mode keeps effects and shortcuts alive over elevated windows. Effects show up in OBS, ShareX, the Snipping Tool and friends, or stay off recordings if you prefer.
- **Looks after itself.** If ClickShade ever crashes it restores your cursors first, writes a report to the settings folder and comes back once. Only one copy runs at a time.
- **A settings window worth opening.** Sidebar, compact cards, a reset on every card and slider, dark and light palettes, nine global shortcuts, tray controls, profiles and a full defaults reset.

## Builds and verification

The download contains separate **Windows 10 (version 1703 or later) / Windows 11 x64** and **Linux x86-64** applications with matching source. The Linux source is distribution-neutral; the supplied binary has documented library requirements. Wayland support is compositor-specific, with an experimental GNOME companion.

The Windows build was checked on Windows 11; the Linux build was compiled and its headless suites run on Ubuntu 24.04. Physical latency, audio hardware, real games and live Wayland sessions still need acceptance on those systems.

ClickShade does not change pointer speed, acceleration or coordinates. It includes no telemetry, activity logger, driver, game injection or anti-cheat bypass. The Windows EXE is unsigned; antivirus acceptance and anti-cheat compatibility cannot be guaranteed.
