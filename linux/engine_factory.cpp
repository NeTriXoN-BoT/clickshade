#include "engine.h"
#include <cstdlib>
#include <string>

namespace clickshade::linuxapp {
std::unique_ptr<LinuxEngine> CreatePlatformEngine() {
    const char* session = std::getenv("XDG_SESSION_TYPE");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    // DISPLAY is commonly also set in Wayland sessions. Choosing it there would
    // only observe Xwayland applications and falsely promise desktop coverage.
    if ((session && std::string(session) == "wayland") || (wayland && *wayland))
        return CreateGnomeWaylandEngine();
    return CreateX11Engine();
}
} // namespace clickshade::linuxapp
