#pragma once

#include <mutex>

namespace clickshade::linuxapp {

// XSetErrorHandler is process-wide even when each subsystem owns a different
// Display. A temporary registration trap must finish before the persistent
// engine handler is installed or removed; otherwise a detached handler can be
// restored later and form a recursive forwarding chain on restart.
// Hold this mutex for the full lifetime of a temporary trap, or around an
// engine handler install/uninstall. Error callbacks must never acquire it.
inline std::mutex& XErrorHandlerLifecycleMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

} // namespace clickshade::linuxapp
