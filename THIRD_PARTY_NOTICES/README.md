# Third-party notices

ClickShade's own code, icon, classic-style arrow and synthesized example sounds
use the root MIT license. No private XSplit, Bandicam, or ScreenPal code/assets
are included. The optional Windows default-arrow preset loads an existing local
Windows file; it does not redistribute Windows cursor artwork.

The supplied Windows Zig build includes compiler/runtime components covered by
the accompanying Zig, LLVM libc++/libc++abi/libunwind, and MinGW notice files.
Retain them when distributing that binary. A different compiler build may have
different runtime notices; review them when changing the toolchain.

The Linux executable dynamically uses system GTK3, GLib/GIO, Cairo, X11 extensions,
libstdc++, glibc, and optionally libpulse. Their shared libraries, compiler SDKs,
and audio server are not bundled here; they remain under their respective licenses
in the user's distribution. Repackaging those libraries later introduces separate
notice/source obligations that this source-only dependency setup does not cover.
