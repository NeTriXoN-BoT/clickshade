# ClickShade 1.1

Cursor feedback, highlights, click effects and sounds for Windows, with a Linux port.

ClickShade is vibe coded: a one-person project in which AI coding assistants wrote a large share of the code. It has been built, run and tested, but it is still a young project, so please report anything that looks off.

## What's new in 1.1

- **Sharp cursors at any pointer size.** With a larger Windows pointer size (Accessibility > Mouse pointer > Size) or display scaling, ClickShade's cursors used to look pixelated, and reapplying them only helped until the next press. Every cursor it installs now carries an image for every size Windows may draw it at, so the released, pressed, faded, colored, shadowed, resized and shake-enlarged cursors, the animated busy cursors and the classic arrow stay sharp on every monitor. Pausing or quitting restores your own cursors exactly.
- **Effects stay over fullscreen browsers and videos.** The fullscreen rule now pauses only games. Browsers, YouTube in fullscreen, video players and presentations keep their effects. Games are recognized from exclusive fullscreen, Windows' own game list and the usual game libraries (Steam, Epic, GOG, EA, Ubisoft, Xbox and others); add one it misses under **Excluded applications**.
- **Effects hide with the pointer.** When an app hides the pointer, as YouTube does when the mouse rests, the highlight, spotlight, trails, click effects and magnifier hide with it. Firefox's transparent pointer now counts as hidden too. While you type in an editor that hides the pointer, the spotlight and the lens stay put instead of flashing with every word (clicks, scrolling and a game's mouse look never keep them), and the key display and typing sounds keep going.
- **Effects that keep up.** On Windows the halo, spotlight and magnifier follow quick flicks more closely. During steady fast movement they are drawn slightly ahead of the pointer, also with mice that report less often than the display refreshes, and they settle exactly on it when it stops.
- **Administrator startup from protected folders only.** With administrator mode, the prompt-free startup task is only created when ClickShade is in a protected folder such as Program Files. From Downloads or the Desktop, Windows asks for approval at sign-in instead, and the Rules page explains why. An elevated copy also removes a startup task that would start another copy from such a folder.
- **Safer profile imports and DLL loading.** An imported profile that points to network shares or device paths now asks before keeping those paths, and the EXE loads its system DLLs from System32 only.
- **A lighter settings window.** The animated spectrum line costs much less CPU, and the tray icon shows its tooltip and opens once per click.
- **Linux and GNOME.** The same games-only fullscreen rule (Steam, Proton/Wine, gamescope, Heroic, Lutris, itch), a hidden-pointer rule with a typing hold, typing sounds while the pointer is hidden, a magnifier fix for small or high-DPI monitors, and lower idle cost. The GNOME extension now shows effects only while the ClickShade app runs.

The full list of changes is in `CHANGELOG.md` in the source.

## Updating from 1.0

Quit ClickShade 1.0, then extract 1.1 over the old folder or into a new one. Your settings carry over; in portable mode, keep `ClickShade.portable` and `settings.ini` beside the new `ClickShade.exe`. If you move to a new folder, switch **Start with Windows** on again there. With administrator mode and **Start with Windows** in a folder outside Program Files, Windows now asks for approval at sign-in: the first elevated start replaces a silent startup task that 1.0 registered there with the normal startup entry, or removes it if it starts an old copy in another folder. To keep the silent start, copy the ClickShade folder into Program Files (or extract the ZIP there), start it from there and switch **Start with Windows** on again. A folder you move there instead stays owned by your account and still counts as unprotected. On GNOME, run `sh ./install.sh` again from `Linux/gnome-extension`, then log out and back in so GNOME Shell loads the new code.

## What you get

- **A cursor that reacts.** Hold a mouse button and your Windows cursor turns dark; let go and it is back. Use your installed theme, the Windows arrow, a classic arrow that stays crisp at any size, or your own `.cur` pair. A cursor scale from 100% to 600% applies to every cursor shape, busy spinner included.
- **Make the cursor your own.** Fade it, give it a soft shadow, or paint it in your own fill and outline colors. Every cursor shape gets the same look, animated ones included, with or without the press effect.
- **Find the pointer with a shake.** A Plasma-style preset that follows KWin's published behavior exactly, or a macOS-inspired one that only needs a couple of quick wiggles.
- **Five highlight styles.** Filled, ring, soft glow, crosshair, and a spotlight that dims everything except a circle around the pointer. Highlights can pulse gently and can hide when the pointer rests.
- **Six click styles and three buttons.** Ring, pulse, ripple, burst, neon ring, and a target that closes in on the click, each button with its own color and switch. Neon blue, neon red and neon green out of the box.
- **Trails.** Dots, rings, sparkles, a tapered comet or a fine sparkle, off by default.
- **Sounds for clicks and typing.** Eleven original mouse sets including the classic click, and five keyboard sets from classic typing to a typewriter. Keyboard sounds only ever learn whether a key, the space bar, Enter or Backspace went down, and only while the switch is on. Nothing is recorded.
- **Presenter tools.** A magnifier lens that follows the pointer (round or square, centered or beside, 1.25× to 8×) and an on-screen key display in the style of the Mac keystroke visualizers: keycaps for shortcuts and, if you like, the words you type, with optional mouse buttons. Nothing is kept once a row fades.
- **Stays out of the way.** Pauses in fullscreen games, games that confine the pointer and on the lock screen, and hides its pointer effects while an app hides the pointer. Exclude any executable, or allow-list the desktop tools that should keep working.
- **Works where it matters.** Optional administrator mode keeps effects and shortcuts alive over elevated windows. Effects show up in OBS, ShareX, the Snipping Tool and friends, or stay off recordings if you prefer.
- **Looks after itself.** If ClickShade ever crashes it restores your cursors first, writes a report to the settings folder and comes back with a note, up to three times in a row. Only one copy runs at a time.
- **A settings window worth opening.** Sidebar, compact cards, a reset on every card and slider, dark and light palettes, nine global shortcuts, tray controls, profiles and a full defaults reset.

## Builds and verification

The download contains separate **Windows 10 (version 1703 or later) / Windows 11 x64** and **Linux x86-64** applications with matching source. The Linux source is distribution-neutral; the supplied binary has documented library requirements. Wayland support is compositor-specific, with an experimental GNOME companion.

The Windows build was checked on Windows 11; the Linux build was compiled and its headless suites run on Ubuntu 24.04. Physical latency, audio hardware, real games and live Wayland sessions still need acceptance on those systems.

ClickShade does not change pointer speed, acceleration or coordinates. It includes no telemetry, activity logger, driver, game injection or anti-cheat bypass. The Windows EXE is unsigned; antivirus acceptance and anti-cheat compatibility cannot be guaranteed.
