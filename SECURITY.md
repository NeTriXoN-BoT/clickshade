# Security and privacy

ClickShade is a local desktop utility. It has no account, telemetry, update server,
network command listener, activity logger, mouse recording or keyboard recording.
It saves user preferences, explicitly requested exports/backups, one pre-import
backup, an optional per-user startup entry and, only after a crash, a
`crash-reports` folder (up to five text reports with their minidumps, plus
`last-crash.txt` for the restart notice). While it runs, the Windows app also
keeps the cursor files it installs in a private temporary folder, described
below, that is removed on exit. Normal app folders contain no debug/test
executables or log files. Developer tests produce output only when someone
explicitly runs them; operating-system/toolkit crash reporting is outside the
application's logging policy.

## Desktop access

Windows observes mouse input through Raw Input without disabling normal input.
Out-of-context WinEvent hooks observe relevant window/cursor changes; they do not
inject a DLL into the foreground application. Application rules transiently read
the foreground executable's name and path using limited process-information
access; the fullscreen-game rule compares them with Windows' own game list (read,
never changed, from `HKCU\System\GameConfigStore`) and the usual game library
folders. No foreground process memory is read, and no application-history log is
written. To keep the spotlight and lens steady while you type, ClickShade asks
Windows only for the time of the last input, not which key it was, and compares
it with the time of the last mouse input it saw itself. Configured
global shortcuts use RegisterHotKey, not a keyboard recording hook.

Keyboard sounds and the key display are the two features that observe the
keyboard. The Raw Input keyboard registration exists only while at least one of
them is on. For keyboard sounds, each key-down is reduced on the input thread to
one of four categories (key, space, enter, backspace), and the audio worker only
ever sees the category. The key display needs more: the input thread posts the
virtual key, scan code and modifier state of each shown press to the renderer,
which turns it into a keycap label or, with **Every key**, typed text (at most
120 characters per row). That label or text is held in memory only while its row
is on screen and is dropped when the row fades; **Shortcuts only** never keeps
typed text. Nothing about typed keys is logged, stored or transmitted. The same
boundary applies on X11 (raw key events subscribed only while one of the
features is on) and in the GNOME companion (sounds receive a category over
D-Bus; the key display's rows live in Shell memory while shown).

While it runs, the Windows app installs its cursors from multi-size cursor files
it writes to a private temporary folder, `%TEMP%\ClickShade\cursors\<process>-...\`,
so Windows can draw each cursor sharply at every size. The files hold only
cursor artwork derived from the theme or the files you chose. They are written
while settings change, never on a click. Each file is then reopened for reading,
checked to be the same file that was just written (a file swapped in between is
refused) and kept open while it can still be installed. While it is open, no
other program can delete or replace it or rename a folder above it, and
temporary-file cleaners skip it. The folder is removed on exit. Folders left by
instances that have ended are removed at the next start of an instance that does
not run as administrator; an elevated instance never deletes by path inside
folders that ordinary programs can change. Cleanup deletes only ClickShade's own
`.cur`/`.ani` files and subfolders and does not follow links. An administrator
instance also labels its own folder high integrity, so ordinary programs cannot
write into it.

Administrator mode is opt-in. It relaunches the same executable through the
standard UAC prompt. For startup it registers a Task Scheduler task with the
highest run level for the current user, which starts the file elevated without a
prompt, so the task is only created when `ClickShade.exe` is in a protected
location: the file, its folder and every parent folder are owned by
Administrators, SYSTEM or TrustedInstaller; neither the user's standard
(non-administrator) rights nor any other account or group named in their
permissions can change or replace them; the drive is fixed and local; and the
path contains no junction, symbolic link or substituted drive. Anything that
cannot be checked counts as unprotected. Program Files qualifies when ClickShade
was copied or extracted there (a folder moved there on the same drive keeps the
user as owner and does not); an extracted ZIP in Downloads or on the Desktop does
not, and there the normal Run entry and a UAC prompt at sign-in are used instead.
A task that an earlier version registered for an unprotected location is replaced
at the next elevated start, and an elevated instance deletes a ClickShade task
that starts another copy from an unprotected or missing location, so a planted
program there cannot start elevated. A task for another copy in a protected
location is left alone. A manual
**Run as administrator** from a writable folder remains the user's choice. The
elevated instance opens its window messages to lower-integrity ClickShade launches
only for the show, restore and quit requests (plus Explorer's taskbar-restart
broadcast); it does not expose any other interface or run anything on behalf of
other processes.

On Windows 10 version 1607 and later, `ClickShade.exe` resolves its static DLL
imports (winmm, dwmapi, uxtheme, wtsapi32, secur32 and the rest) from System32
only, so a DLL placed beside the executable in a download folder is not loaded.
Common Controls v6 still comes from WinSxS through the manifest.

Crash handling restores the system cursors first, then writes a text report and a
minidump to `crash-reports` under the settings folder (the five newest crashes are
kept). The text report holds the version, the time, the kind of fault and, for an
uncaught C++ exception, its message (which can name a file path), thread and
process IDs, the exception code and module-relative stack addresses; no screen
content. The minidump (`MiniDumpNormal`) also holds the module list and the stack
memory of every thread, which can contain fragments of recent input, key-display
text or file paths. Review it before attaching it to a public report, or attach
the text report alone. Both stay on the computer unless the user attaches them.
The application restarts itself after a crash up to three times in a row and
shows a message instead on the next crash; a restarted instance that runs
normally for 90 seconds starts the count again.

The X11 backend observes XI2 events and desktop properties; shortcut bindings use
exact passive key grabs on a separate X connection. Its temporary cursor hide
request belongs to the renderer's connection. To recognise fullscreen games it
reads the foreground window's `/proc/<pid>/exe`, `/proc/<pid>/cmdline` (bounded)
and parent process names once per focus change; the GNOME companion reads
`/proc/<pid>/exe` and `cmdline` once per window. None of it is stored. The GNOME
companion executes in Shell and has a different trust boundary: review and test
it as compositor code.
No direct `/dev/input` access, root helper, kernel driver, game injection, input
replay or changes to pointer acceleration/sensitivity are used.

Pausing is not process termination. **Quit** closes the application and its
Windows recovery guard. No anti-cheat vendor has certified ClickShade, and no
blanket promise about third-party enforcement or bans is made.

## Review priorities

- Cursor resource ownership/restoration, especially SetSystemCursor's consuming
  handle contract and failure cleanup; the temporary cursor folder's integrity
  label, the read handles that lock the cursor files written there (and the
  identity check before locking), and cleanup that never follows links.
- Malformed CUR/WAV inputs: bounded reads, offsets, dimensions, hotspots, sample
  format and bounded stream windows. File duration does not size an allocation. Optional Windows codecs use system Media Foundation.
- Bounded click/trail/voice pools, coalesced movement/settings, timer and callback
  lifetime, and event-loop shutdown without input interference.
- Strict platform/version/schema validation for imports, bounded regular-file
  reads, atomic settings replacement, full-default rollback, startup command quoting,
  shortcut-conflict rollback and user-selected path handling.
- Linux cursor hide/show balance and synchronous GNOME extension cleanup.
- The keyboard boundary: registration lifetime while keyboard sounds or the key
  display are on, category reduction for sounds, and the key display's in-memory
  rows with no key storage beyond them; the Task Scheduler/elevation path and
  the protected-location check that guards it; System32-only DLL loading;
  network-path detection on profile import; crash-report and minidump contents
  and the restart limit; the single-instance mutex and window messages.
- Matching binaries/source, licenses, clean public/private packaging and checksum
  verification. Checksums detect corruption; unsigned downloads do not establish
  publisher identity.

Profile files are data, not scripts. Imports do not enable startup or run shell
commands, and they preserve the current computer's startup registration. Exports
link external file paths rather than embedding the user's cursor/audio files.
Review those paths before sharing a profile. Opening a path on another computer
signs in to it with the user's Windows account (NTLM), so a shared profile could
point at a server that collects that sign-in. On Windows, Import therefore lists
any cursor or sound path that leads to a network share (UNC, WebDAV such as
`\\host@SSL\...`, `\\?\UNC\`), a mapped network drive or a device namespace
(`\\.\`, `\\?\GLOBALROOT\`, `\??\`) before anything opens it, and keeps those paths
only on an explicit **Yes**; the default imports without them. The Windows audio
worker also leaves the sound files of a switched-off group closed until those
sounds are switched on or tested. Auto pacing keeps a short rendering
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
