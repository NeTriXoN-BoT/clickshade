#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace clickshade {

// Last-resort handling for faults the normal error paths cannot catch:
// access violations, uncaught C++ exceptions, pure-virtual calls and CRT
// parameter faults. The handler does three things, in this order:
//   1. restore the user's own cursor scheme (SPI_SETCURSORS),
//   2. write one plain-text report (and a small minidump when dbghelp is
//      available) to the crash-reports folder beside the settings file,
//   3. start a fresh instance that shows what happened, unless this process
//      was itself restarted after a crash a moment ago.
// Reports contain the version, the fault code/address, module offsets and a
// return-address stack. They never contain settings, paths you selected,
// screen contents or input. At most five reports are kept.
struct CrashGuardOptions {
    bool restartAfterCrash = true;
    unsigned restartGeneration = 0; // From --after-crash <n>; limits restart loops.
};

void InstallCrashGuard(const CrashGuardOptions& options);
// Called once the restarted instance has run normally for a while, so a later
// unrelated crash may restart again instead of counting toward the loop limit.
void ResetRestartGeneration() noexcept;

// Written by the crash handler and read by the restarted instance.
struct CrashNotice {
    bool present = false;
    std::wstring reportPath;
    std::wstring summary;
};
CrashNotice TakeCrashNotice();

// Records a non-fatal internal error for the status card without any file
// I/O. The GUI reads it through the same notice mechanism.
void NoteInternalError(const std::wstring& text);

} // namespace clickshade
