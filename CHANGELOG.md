# Changelog

## 1.1

### Fixed

- Cursors no longer look pixelated with a larger Windows pointer size (Accessibility > Mouse pointer > Size) or display scaling. ClickShade's cursors used to be single bitmaps that Windows stretched, and reapplying them helped only until the next press. Every cursor ClickShade installs is now a multi-size cursor file with an image for every size Windows may draw it at, so released, pressed, faded, colored, shadowed, resized and shake-enlarged cursors, the animated busy cursors and the classic arrow stay sharp at any pointer size and on every monitor. Pausing or quitting restores your own scheme exactly.
- With the soft shadow, the extra sizes are built so that each finished cursor matches the size Windows draws it at.
- An animated cursor whose prepared animation file has gone missing falls back to the theme's own file instead of a still frame.
- With a larger pointer size, copies of odd-sized cursors no longer lose a pixel row.
- The Windows arrow, the Classic arrow or a custom pair chosen over an animated arrow now gets the press and shake effects. Restoring still brings back your animated arrow.
- The Classic arrow uses the local Windows file only when it has an image of exactly the size needed; any other size is drawn crisply instead of resampled.
- Effects now hide with Firefox's transparent pointer (CSS `cursor: none`, as in YouTube's idle player), like any other hidden pointer.
- Typing sounds keep playing while an editor hides the pointer. Only click sounds pause.
- With the waveOut fallback, click and key sounds no longer play choppy with half the audio missing.
- When all sixteen voices are busy, a new sound replaces the one closest to its end instead of a long one.
- A press held through a cursor reload (Reload cursors, or a display, scaling or theme change) keeps the pressed cursor and the held button.
- The cursor no longer flashes or jumps in size when an app hides and shows the pointer; the chosen size and look stay installed meanwhile.
- The Target, Ripple and Burst click styles are no longer cut flat at the sides with thick lines or at high DPI.
- The first key-display row after startup no longer gets keycaps that are too narrow.
- Changing the key display's size, position, opacity or color with the key display switched off no longer removes the **Show a sample** row; it fades after its hold time as usual.
- Switching breathing off returns the highlight to full opacity at once.
- The tray icon now shows its tooltip, one click opens the window once and a right click opens the menu once. The tray icon of an elevated ClickShade comes back after Explorer restarts.
- Launching ClickShade while an elevated copy runs now opens that copy's settings window instead of doing nothing, and the window comes to the front instead of opening behind Explorer.
- With administrator mode, the sign-in start from the normal startup entry no longer opens the settings window. A crash restart that relaunches elevated keeps its restart count and recovery note.
- After a crash restart, the window opens on Appearance, where the About card shows the recovery note.

### Changed

- **Pause all effects in fullscreen games** (Rules) now pauses only games. Browsers, YouTube in fullscreen, video players and presentations keep their effects. A fullscreen window counts as a game when it uses exclusive fullscreen, is on Windows' own game list (the one Game Bar keeps), or runs from one of the usual game library folders (Steam, Epic, GOG, EA/Origin, Ubisoft, Riot, Xbox, Rockstar, Amazon, itch). Add a game it misses under **Excluded applications**. The settings key is still `PauseFullscreen`.
- When an app hides the pointer, every pointer effect hides with it, the spotlight and the magnifier included, so a video that hides an idle pointer leaves no dimmed screen or lens behind. When the pointer hides right after typing, the spotlight and the lens stay in place until about three seconds after the last input, so they do not flash with every word. Only typing counts: clicking into an app, scrolling or a game's mouse look never starts or keeps that hold. The key display and keyboard sounds keep working.
- The breathing switch is now called **Gently pulse the highlight**, because the pulse also runs while the pointer moves.
- Administrator mode creates the silent startup task only when ClickShade.exe is in a protected folder such as Program Files. From a folder that programs without administrator rights can change (Downloads, the Desktop, a folder you made on C:), Start with Windows keeps the normal startup entry and Windows asks for approval at sign-in; the Rules page says why. A task that 1.0 registered for such a folder is replaced at the next elevated start.
- An elevated ClickShade removes a ClickShade startup task that would start another copy from an unprotected or missing folder (for example a 1.0 folder in Downloads) and says so on the Rules page. A task for another copy in a protected folder is left alone.
- A folder also counts as unprotected when its permissions let any other account or group, not only yours, change the file, its folder or a folder above it.
- The elevated startup task runs at normal priority instead of Task Scheduler's below-normal default. A task registered by 1.0 is updated at the next elevated start.
- Turning on **Run ClickShade as administrator** in a copy that already runs elevated moves Start with Windows to the elevated task right away, if the folder is protected.

### Improved

- The halo, spotlight and magnifier keep up better with quick flicks. The halo now moves with the same layered-window update that draws it, which reaches the screen sooner, and during steady fast movement these three effects are drawn slightly ahead of the pointer. The lead stays steady with mice that report less often than the display refreshes, fades out for slow, braking or turning movement, and drops when the pointer stops, so the effects settle exactly on it within about one report of the mouse. Trails and click effects always use the real positions. This works at any refresh rate and with a manual frame limit.
- An open settings window costs much less CPU: the animated spectrum line redraws only its own strip and pauses while the window is on another virtual desktop.
- Fewer full cursor reloads. Taskbar and work-area changes, accent and theme colors (wallpaper slideshows included), policy refreshes, taskbar and regional settings, screen saver and wheel settings no longer reload every cursor. Cursor scheme and high contrast changes still do.
- Startup builds the cursor artwork once instead of twice.
- Auto frame pacing recovers its rate across short, separate animations, such as a series of clicks.
- The effects renderer listens to desktop-wide window events only while an effect is on screen, and a held spotlight, lens or key display stays above other always-on-top windows while the pointer is hidden.
- Sound files of a switched-off sound group are no longer opened, including paths from an imported profile. A click-sound **Test** opens only the file it plays, and the keyboard **Test** only the chosen set.

### Security

- On Windows 10 version 1607 and later, ClickShade.exe loads its system DLLs (winmm, dwmapi, uxtheme and the rest) from System32 only, so a DLL placed beside the executable is ignored.
- Profile import checks cursor and sound paths. If any point to a network share (including WebDAV), a mapped network drive or a device path, the confirmation lists them and offers Yes (keep them), No (import without them, the default) or Cancel. Opening such a path would sign in to that computer with your Windows account.
- Every cursor file ClickShade writes stays open for reading while it may be installed, so temporary-file cleaners such as Disk Cleanup or Storage Sense cannot delete it, and no other program can replace it or rename a folder above it. A file that changed between writing and locking is refused.
- The temporary cursor folder of an elevated ClickShade has a high integrity label, so ordinary programs cannot write into it, and an elevated copy no longer deletes the leftover folders of other copies; a normal copy cleans those up.
- The crash text report now notes that the minidump holds thread stack memory. That memory can include recent input or file paths, so review the minidump before sharing it.
- ClickShade no longer creates an empty `crash-reports` folder at every start; the folder appears when a crash report is written.

### Linux

- **Pause in fullscreen games** now pauses only games: programs from a Steam library, Proton or Wine, gamescope, Heroic, Lutris or itch, and windows with a `steam_app_` class. Browsers, video players and presentations keep their effects. The settings key is still `PauseFullscreen`.
- On X11 a hidden pointer (a fully transparent cursor, as Firefox shows for `cursor: none`) now hides the spotlight and the lens too, and stops click sounds, while keyboard sounds and the key display continue. Only when the pointer hides right after a key press do the spotlight and the lens stay in place while you type; this needs keyboard sounds or the key display switched on.
- Fixed a crash or wrong capture with a large magnifier on a small or high-DPI monitor. The lens now shows a smaller part of that monitor, magnified a little more than the chosen zoom, instead.
- The lens reads the screen only when the pointer moves and about 30 times a second at rest. Breathing redraws about 30 times a second in a window of fixed size. The shake cache is built when a shake starts, the key display no longer slows down with long typed rows, and dragging a window no longer floods the rule checks.
- A display-scale (`Xft.dpi`) change applies while the app runs.
- The sound stream pauses when idle so the device can suspend, and the app keeps no sound-server connection while all sounds are off.
- `--background` and **Open settings minimized** no longer flash the window. A tray that docks the icon only after the app's short wait still takes the minimized window, unless you opened it meanwhile.
- The key display measures its rows again when it is drawn on a monitor of a different width, so it always fits that monitor.
- Keyboard sound sets no longer appear in the click-sound lists.
- `cmake --install` names the launcher `io.clickshade.ClickShade.desktop`, so GNOME on Wayland matches it to the window.
- The version comes from the same source as on Windows; `--version` prints 1.1.
- `linux/build.sh --test` and `tools/build_linux.py --test` run the same eight headless suites as CTest.

### GNOME companion

- Fullscreen pause applies only to games, with similar rules plus games in `~/Games`.
- When the Shell reports the pointer hidden, the lens now hides along with the highlight and click effects, and click sounds and mouse-button labels stop. Only when the pointer hides right after a key press does the lens stay in place while you type. Keyboard sounds and the key display ignore the hidden pointer.
- The extension draws nothing until the ClickShade app activates it, and its effects stop when the app quits, crashes or is killed. After the lock screen they resume by themselves while the app runs.
- **Pause and restore** and the master switch take effect at once.
- Pointer sampling runs only while a pointer effect is on. Breathing repaints about 30 times a second whatever the sampling rate, and an idle session (no input for about a second) wakes the extension only at that pace.
- The status line under **Effects active** names limits of the Shell build, such as a Shell that does not report a hidden pointer.
- A new `MouseAllowedChanged` signal lets the app stop click sounds alone while typing sounds continue. The 1.1 app still works with the 1.0 extension, but run `install.sh` again and log out and back in to get these changes.

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
- Crash recovery: a crash restores the cursors first, writes a report and minidump to the settings folder and restarts with a notice, up to three times in a row; the next crash shows a message instead. The count starts again after 90 seconds of normal running.
- One instance per session; a second launch only brings the running settings window forward. Task Manager shows "ClickShade".

### Settings window

- A sidebar, compact cards, a reset button on every card and every slider, dark and light palettes with presets, extra zoom, and an animated spectrum line that can be turned off.
- Nine global shortcuts, tray controls, optional startup, profile export/import/backup, and a full defaults reset.

### Linux

- A GTK3 settings app with a native X11 backend (highlights, trails, click effects, current-cursor inversion, shake enlargement, magnifier lens, key display, click and keyboard sounds, global shortcuts) and an experimental GNOME Wayland companion extension with highlights, click effects, a lens and the key display.
