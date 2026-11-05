# Contributing to ClickShade

Read README.md and docs/ARCHITECTURE.md before changing cursor behavior.
A small, responsive native utility is the product. Preserve the custom GUI,
user colors, independent controls, and restoration behavior.

## Useful contributions

Native Windows acceptance on real displays is especially valuable. Include
Windows version, monitor/DPI/refresh setup, cursor theme/size, application,
reproduction steps, and whether the issue occurs with overlays/sounds disabled.
For Linux, report distro, architecture, X11/Wayland, compositor/version, and
whether the X11 compositor or GNOME extension is active.

Other priorities are compositor-specific Wayland integrations, accessible GUI
control names, mixed-DPI behavior, resource/latency measurement, and distribution
packaging. Discuss broad new features before adding dependencies or threads.

## Development

Build with the documented MSVC/Zig or Linux commands. Use an isolated desktop
for cursor/input integration tests. They may temporarily move/hide/replace the
pointer; the Windows runtime suite also opens a fullscreen window and a
fullscreen test "game" (a copy of itself in a temporary Steam-style folder under
`%TEMP%`), and the overlay suite presses F24 and writes its BMP screenshots
beside its own executable (`visual_smoke.exe` from the Zig build,
`ClickShadeVisualTest.exe` from CMake). Leave the mouse and keyboard alone while
they run, and close ClickShade first. Do not claim Windows compatibility from a
Wine pass, audible playback from decoder tests, or Wayland compatibility from
JavaScript syntax checks.

Keep input handling free of file I/O, decoding, GUI work, and unbounded queues.
Keep overlays non-activating and input-transparent. Restore owned cursor state
on pause, error, exit, and backend changes. Never write permanent cursor-theme
settings as a shortcut for runtime effects.

A pull request should explain the concrete problem, changed behavior, validation,
and remaining limitations. Include a real application screenshot for visible UI
changes. Add a focused test when behavior or resource ownership warrants it;
avoid tests that merely restate the implementation. Register a headless test
with `add_test` in CMakeLists.txt (or linux/CMakeLists.txt): CI runs every
registered test through `ctest` on both platforms. `linux/build.sh --test` and
`tools/build_linux.py --test` keep their own lists of the same Linux suites, so
add a new Linux suite there too. Suites that move the pointer, replace cursors or
open windows stay unregistered and need `--run` on an isolated desktop.

Contributions are accepted under the project's MIT license. Do not add another
product's private source, cursor assets, sounds, or incompatible licensed code.

## Original assets

The icon and GitHub banner are generated from repository-owned geometry by
`tools/generate_icon.py` and `tools/generate_branding.py`. Regenerating PNG/ICO
assets requires Python3 and Pillow; the banner uses an installed Segoe UI,
DejaVu Sans or Nimbus Sans font. The native applications have no Python/Pillow
runtime dependency. Keep the vector masters, small ICO frames and embedded GTK
icon consistent; rebuild both apps after changing resources. Example sounds are
recreated by `tools/generate_example_sounds.py` using the Python standard library.
