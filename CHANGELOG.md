# Changelog

## 1.0

First public release.

### Cursor

- Hold a mouse button and your Windows cursor switches to a dark version; release and it comes back. Works with the installed theme, the Windows arrow, a classic arrow drawn at any size, or your own `.cur` pair.
- Reverse light and dark, keep the pressed look while holding or flash it briefly, choose which buttons trigger it, and scale the cursor from 100% to 600% of the size Windows uses. The scale applies to every cursor shape, including the animated busy and working cursors, and redraws each from its sharpest source image.
- Cursor look: opacity from 30% to 100%, a soft drop shadow, and custom fill and outline colors that repaint the light and dark parts of the cursor (the pressed cursor shows them swapped). It reaches every cursor shape, animated ones keep every frame, and the chosen cursor, scale and look apply even with the press effect turned off.
- Shake to find the pointer: a Plasma-style preset that matches KWin's published numbers exactly, and a gentler macOS-inspired preset that reacts to a couple of quick wiggles.

### Highlights, trails and clicks

- Five highlight styles: filled, ring, soft glow, crosshair, and a spotlight that dims the rest of the screen around the pointer. Optional breathing and hide-when-resting.
- Five trail styles: dots, rings, sparkles, comet and fine sparkle.
- Six click styles: ring, pulse, ripple, burst, neon ring and target, a scope that closes in on the click. Separate colors and switches for left, right and middle buttons; neon blue, neon red and neon green by default.
- Adaptive frame pacing for all effects that stays within the monitor's refresh rate, or a manual 30 to 360 fps limit, on the Appearance page.

### Sound

- Click sounds per button from eleven original sets, including the classic mouse click, or any file you choose. Each dropdown lists only the files made for that button.
- Keyboard sounds with their own switch, volume and shortcut. A set is a folder with shuffled key variants plus space, enter and backspace; five sets ship, from classic typing to a typewriter. Only the coarse key category reaches the mixer, and only while the switch is on.
- An event-driven WASAPI mixer with a waveOut fallback, sixteen voices, separate live volumes for mouse and keyboard, and bounded streaming for long files.

### Presenter tools

- A magnifier lens that follows the pointer: round or rounded square, centered on the pointer or beside it, 1.25× to 8×, with a frame color or no frame at all. On Windows it is hosted in the Magnification API and leaves ClickShade's own overlays out of the magnified view; on Linux it sits beside the pointer.
- An on-screen key display in the style of the Mac keystroke visualizers: shortcuts appear as keycaps on rounded panels, and in every-key mode typed letters join into words. Shortcuts only (Ctrl, Alt or Win chords, function keys, tapped modifiers) or every key, optional mouse buttons, four positions, text size, hold time, opacity and backdrop color. A repeated shortcut adds a count. The display keeps working while an editor hides the pointer; labels exist only while shown.
- Both have their own global shortcuts and tray entries.

### Rules and compatibility

- Pause in fullscreen apps, when a game confines the pointer, on the lock screen, and for excluded executables; allow-list the desktop tools that should keep working.
- Optional administrator mode so effects, shortcuts and held-button tracking also work over elevated windows; startup then uses a scheduled task without a prompt.
- Effects appear in OBS, ShareX, the Snipping Tool and similar capture tools, with a switch to keep them off recordings.
- Crash recovery: a crash restores the cursors first, writes a report and minidump to the settings folder, restarts once with a notice, and stops after three failures.
- One instance per session; a second launch only brings the running settings window forward. Task Manager shows "ClickShade".

### Settings window

- A sidebar, compact cards, a reset button on every card and every slider, dark and light palettes with presets, extra zoom, and an animated spectrum line that can be turned off.
- Nine global shortcuts, tray controls, optional startup, profile export/import/backup, and a full defaults reset.

### Linux

- A GTK3 settings app with a native X11 backend (highlights, trails, click effects, current-cursor inversion, shake enlargement, magnifier lens, key display, click and keyboard sounds, global shortcuts) and an experimental GNOME Wayland companion extension with highlights, click effects, a lens and the key display.
