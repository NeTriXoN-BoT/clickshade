# Security and privacy

ClickShade is a local desktop utility. It has no account, telemetry, update server,
network command listener, activity logger, mouse recording or keyboard recording.
It saves user preferences, explicitly requested exports/backups, one pre-import
backup, and an optional per-user startup entry. Normal app
folders contain no debug/test executables or log files. Developer tests produce
output only when someone explicitly runs them; operating-system/toolkit crash
reporting is outside the application's logging policy.

## Desktop access

Windows observes mouse input through Raw Input without disabling normal input.
Out-of-context WinEvent hooks observe relevant window/cursor changes; they do not
inject a DLL into the foreground application. Application rules transiently read
the foreground executable's name using limited process-information access. No
foreground process memory is read, and no application-history log is written.
Configured global shortcuts use RegisterHotKey, not a keyboard recording hook.

Keyboard sounds are the one feature that observes the keyboard. The Raw Input
keyboard registration exists only while that switch is on; each key-down is
reduced on the input thread to one of four categories (key, space, enter,
backspace) and the scan code and virtual key are discarded. Nothing about typed
keys is logged, buffered, stored or transmitted, and the audio worker only ever
sees the category. The same boundary applies on X11 (raw key presses subscribed
only while enabled) and in the GNOME companion (a category over D-Bus).

Administrator mode is opt-in. It relaunches the same executable through the
standard UAC prompt and, for startup, registers a Task Scheduler task with the
highest run level for the current user. The elevated instance opens its window
messages to lower-integrity ClickShade launches only for the show, restore and quit
requests; it does not expose any other interface or run anything on behalf of
other processes.

Crash handling restores the system cursors first, then writes a text report and a
minidump to `crash-reports` under the settings folder (five kept). Reports hold the
exception code, module-relative stack addresses, the version and the settings
path; no screen content, input or file contents. They stay on the computer unless
the user attaches them to a report. The application restarts itself at most three
times in a row.

The X11 backend observes XI2 events and desktop properties; shortcut bindings use
exact passive key grabs on a separate X connection. Its temporary cursor hide
request belongs to the renderer's connection. The GNOME companion executes in
Shell and has a different trust boundary: review and test it as compositor code.
No direct `/dev/input` access, root helper, kernel driver, game injection, input
replay or changes to pointer acceleration/sensitivity are used.

Pausing is not process termination. **Quit** closes the application and its
Windows recovery guard. No anti-cheat vendor has certified ClickShade, and no
blanket promise about third-party enforcement or bans is made.

## Review priorities

- Cursor resource ownership/restoration, especially SetSystemCursor's consuming
  handle contract and failure cleanup.
- Malformed CUR/WAV inputs: bounded reads, offsets, dimensions, hotspots, sample
  format and bounded stream windows. File duration does not size an allocation. Optional Windows codecs use system Media Foundation.
- Bounded click/trail/voice pools, coalesced movement/settings, timer and callback
  lifetime, and event-loop shutdown without input interference.
- Strict platform/version/schema validation for imports, bounded regular-file
  reads, atomic settings replacement, full-default rollback, startup command quoting,
  shortcut-conflict rollback and user-selected path handling.
- Linux cursor hide/show balance and synchronous GNOME extension cleanup.
- The keyboard-sound boundary: registration lifetime, category reduction, and the
  absence of any key storage; the Task Scheduler/elevation path; crash-report
  contents and the restart limit; the single-instance mutex and window messages.
- Matching binaries/source, licenses, clean public/private packaging and checksum
  verification. Checksums detect corruption; unsigned downloads do not establish
  publisher identity.

Profile files are data, not scripts. Imports do not enable startup or run shell
commands, and they preserve the current computer's startup registration. Exports
link external file paths rather than embedding the user's cursor/audio files.
Review those paths before sharing a profile. Auto pacing keeps a short rendering
cost average in memory; it collects no hardware identifier or benchmark history.

The Windows held-button fallback checks foreground integrity before interpreting
an asynchronous zero as a release. It does not elevate the application or bypass
UIPI. Secure desktops, protected surfaces and application-owned cursors remain
outside the ordinary desktop effect boundary.

The project has had an internal engineering review, not independent security
certification.

## Reporting

When publishing the repository, enable GitHub private vulnerability reporting.
Use **Security → Report a vulnerability** for a reproducible security issue. If
that option is unavailable, ask the maintainer for a private reporting route
without posting sensitive local paths or exploit details in a public issue.
No unowned contact address is supplied in this template.

Only the latest release/development line is maintained unless the maintainer
establishes a backport policy. No response-time commitment is implied.

## Antivirus classifications

Release executables are ordinary, optimized native builds with no obfuscation,
executable packing, bundled advertising or automatic code download. The current
Windows build is unsigned. No antivirus vendor result or anti-cheat certification
is implied by the source audit. Reputation warnings and false positives remain
possible. Build from the published source with the documented toolchain if you
want to verify the binary yourself, and report a reproducible false positive
through the vendor's own route.
