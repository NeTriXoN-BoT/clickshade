# Effects and desktop references

ClickShade brings familiar cursor feedback to the normal desktop. The controls
are independent: a highlight can stay on without sounds, trails or cursor changes.
This table describes the implemented effects, not a claim to reproduce every
private operating-system animation or every recorder's renderer.

| Effect | Controls | Windows | Linux X11 | GNOME Wayland companion |
| --- | --- | --- | --- | --- |
| Pointer highlight | Filled, ring, glow, crosshair or spotlight; color, radius, opacity, width; breathing; hide when idle | Yes | Yes | Experimental (spotlight drawn as a glow) |
| Click feedback | Left/right/middle colors; ring, pulse, ripple, burst, neon ring or target; duration, radius, opacity, easing | Yes | Yes | Experimental |
| Keyboard sounds | Set folders with shuffled key variants plus space/enter/backspace; own volume and shortcut | Raw Input key category only while enabled | XI2 raw key category only while enabled | Category over D-Bus from the extension |
| Cursor trails | Dots, rings, sparkles, Comet, Fine sparkle; color/size/fade | Yes | Yes | Unavailable |
| Press/release cursor | Held or brief, selected buttons | Static system roles | Current static cursor overlay | Unavailable |
| Reversed light/dark pair | Dark released, light pressed | Yes | Unavailable | Unavailable |
| Windows/default/classic/custom arrow | Local assets, CUR pairs, exact source or explicit size | Yes | Use desktop cursor theme | Use desktop cursor theme |
| Plasma-style shake locator | Sensitivity, maximum scale | Cached native arrow | Cached current cursor overlay | Use compositor facilities |
| macOS-inspired shake locator | Sensitivity, maximum scale | Cached native arrow | Cached current cursor overlay | Use compositor facilities |
| Click sounds | Left/right/middle, 33 templates in eleven sets, custom files, volume | WAV and installed codecs through WASAPI (waveOut fallback) | WAV | WAV through running bridge |
| Magnifier lens | Zoom 125–800 %, size, round or rounded square, centered or beside, frame color or no frame | Magnification API window; own overlays excluded; live while resting | Beside the pointer, read from the screen about 30 times a second | Square lens beside the pointer, cloned from the window layer |
| Key display | Shortcuts only or every key, mouse buttons, four positions, size, hold time, opacity, backdrop | Raw Input while enabled; keycaps and typed words only in memory while shown | XI2 raw keys while enabled; same rows | Shell captured events; St keycaps |
| Capture visibility | Show or hide every overlay in recordings and screenshots | Display affinity (Windows 10 version 2004 or later) | Always captured | Always captured |

## Published behavior and original tuning

The Plasma-style detector/timing reference is **KWin 6.8.1**, the current stable
Plasma release checked on 21 October 2026 (released 20 October). At default sensitivity
it uses a one-second trace, a 100 logical-pixel span and path/span ratio above 4.
The first trigger targets 3×; repeated triggers add 1×. Growth and return use 200 ms
cubic transitions, with return beginning two seconds after the last trigger.
ClickShade implements these rules independently. It adds a user ceiling and
bounded image cache, and its X11 backend enlarges the current cursor rather than
KWin's themed Arrow. It is not a copy of KWin's GPL implementation or compositor.
The reference also skips held-button and constrained/warped motion. ClickShade
ignores held-button gestures and known confinement on Windows; generic X11 cannot
reliably identify every other client's pointer constraint or warp. Monitor-scale
transitions reset its trace. These differences are documented rather than hidden
behind an "exact clone" label.

Apple documents quick shaking to enlarge the pointer, but publishes no exact
recognizer, magnification or timing constants. **macOS-inspired** is therefore an
explicitly original preset tuned to the same feel, where two quick back-and-forth
passes are enough: 600 ms trace, 60-pixel span, ratio above 3, 2.5× initial size,
120 ms growth, 800 ms hold and 220 ms return. It is not labelled pixel/timing
identical to macOS.

Size, custom cursor colors, shake enlargement and pointer visibility are separate
desktop accessibility features. ClickShade can follow a user's installed cursor
theme and provide its own effects; it does not silently rewrite permanent desktop
theme, acceleration or accessibility preferences.

## Mac effects reviewed

The current [Apple pointer guide](https://support.apple.com/guide/mac-help/make-the-pointer-easier-to-see-mchlp2920/mac)
documents shake enlargement, persistent pointer size, and separate fill/outline
colors. Apple's [screen capture API](https://developer.apple.com/documentation/avfoundation/avcapturescreeninput/capturesmouseclicks)
also describes a click circle in captured output. These are distinct from the
presentation features supplied by independent Mac utilities.

| Reference effect | ClickShade coverage |
| --- | --- |
| macOS shake enlargement | Optional macOS-inspired preset on Windows and composited X11; original timing, not a claim about Apple's private implementation. |
| Pointer size and colors | Windows cursor scale, opacity, soft shadow, fill/outline colors and installed/custom cursor artwork. On Linux use the desktop theme for permanent artwork; X11 shake temporarily enlarges the captured image. |
| Presentify-style constant halo or click-only pulse | Independent filled/outline halo and left/right click ring/pulse controls. Turn off Highlight while leaving Clicks enabled for click-only feedback. |
| Mouseposé-style per-button click colors and sounds | Independent left/right animation colors, plus left/right/middle sound files and templates. ClickShade does not count double/triple clicks as separate numbered indicators. |
| Cursor Pro-style highlight customization | Radius, opacity, ring width and colors; five optional trails. The ClickShade halo is circular, not every shape from that app. |
| Screen-dimming spotlight and magnifying lens | The Spotlight highlight style dims the pointer's monitor around a clear circle, and the Presenter page adds a magnifier lens (round or square, centered or beside the pointer, 1.25–8×). On Windows the lens is a Magnification API window, so no screen capture service of ClickShade's own is involved. |
| Camtasia-style cursor scale | **Cursor scale** enlarges the pointer like the scale slider of screen-recording editors, but live, on every pointer role and measured from Windows' own pointer size. Each size is redrawn from the sharpest source image instead of stretching one bitmap. The cursor look adds the editors' opacity, drop shadow and color options to the live pointer, and the **Target** click style closes in on the click like their scope-style click effect. |
| On-screen keystrokes | The Presenter page's key display shows shortcuts as keycaps on rounded panels, typed words in every-key mode, and optional mouse buttons, in the style of the Mac keystroke visualizers. Rows exist only while shown; there is no typing recorder. |
| Annotation and whiteboard | Outside this release's scope. |

A restrained presentation setup is an outline halo at 24 DIP, two-DIP width and
45% opacity; click rings around 38 DIP with 300 ms cubic ease-out; macOS-inspired
shake at 100% sensitivity; sounds and trails optional. These are original starting
values that can be adjusted independently, not imported proprietary presets.

Primary product references:
[Presentify cursor highlight](https://presentifyapp.com/highlight),
[Mouseposé](https://boinx.com/mousepose/),
[Cursor Pro](https://appahead.studio/apps/cursor-pro/), and
[Camtasia cursor editing](https://www.techsmith.com/learn/tutorials/camtasia/cursor-editing-basics/).
Only their publicly described behavior was reviewed; no commercial code, artwork
or sound files were included.

## Linux coverage

Distribution and compositor are different compatibility questions. The source
build uses ordinary CMake/pkg-config dependencies and XDG paths. It is not tied
to Ubuntu, a package manager or GNOME. The supplied x86-64 binary still has a
documented libc/library minimum; a source rebuild is the route to older or
different architectures.

X11 provides desktop-wide facilities used by the X11 backend. Wayland does not
give an ordinary client unrestricted global pointer observation and overlays.
The optional GNOME companion runs inside Shell and has its own capability limits.
KDE Plasma Wayland and wlroots compositors need separate integrations for
ClickShade's effects. KDE's own built-in shake locator remains available through
the desktop's effects settings. Xwayland does not remove this boundary.

Read the [settings guide](SETTINGS.md) and the [platform guide](../linux/README.md)
for practical controls and tested limits.

## Primary references

- [KDE Plasma 6.8.1 release](https://kde.org/announcements/plasma/6/6.8.1/)
- [KWin shake cursor behavior at v6.8.1](https://github.com/KDE/kwin/blob/v6.8.1/src/plugins/shakecursor/shakecursor.cpp)
- [KWin gesture detector at v6.8.1](https://github.com/KDE/kwin/blob/v6.8.1/src/plugins/shakecursor/shakedetector.cpp)
- [Apple: make the pointer easier to see](https://support.apple.com/guide/mac-help/make-the-pointer-easier-to-see-mchlp2920/mac)
- [Wayland protocol reference](https://wayland.freedesktop.org/docs/html/apa.html)

Reference review: 21 October 2026. No KDE, Apple, Microsoft or XSplit cursor artwork
is distributed with this project.
