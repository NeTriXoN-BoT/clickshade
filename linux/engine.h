#pragma once

#include "settings.h"
#include <memory>
#include <string>

namespace clickshade::linuxapp {

struct EngineStatus {
    bool running = false;
    bool globalSupported = false;
    unsigned heldButtons = 0;
    bool pressed = false;
    bool pausedByRule = false;
    std::uint64_t inputEvents = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t wakeups = 0;
    std::string backend;
    std::string stateText;
    std::string lastError;
    std::string capabilities;
};

// Backends own their event processing. GUI updates never perform work inside
// the input path. Status must be safe to request from the GTK main thread.
class LinuxEngine {
public:
    virtual ~LinuxEngine() = default;
    virtual bool Start(const Settings&, std::string& error) = 0;
    virtual void Update(const Settings&) = 0;
    virtual void Stop() = 0;
    virtual EngineStatus Status() const = 0;
    virtual void PreviewSound(unsigned buttonMask) = 0;
    // Plays one key variant of the selected keyboard set, even while keyboard
    // sounds are switched off, so the picker can audition a set.
    virtual void PreviewKeyboardSound() = 0;
    virtual void Restore() = 0;
};

std::unique_ptr<LinuxEngine> CreateX11Engine();
std::unique_ptr<LinuxEngine> CreateGnomeWaylandEngine();
// Detect Wayland before DISPLAY. Xwayland is never presented as global Wayland
// access; unsupported compositors must expose the limitation through Status.
std::unique_ptr<LinuxEngine> CreatePlatformEngine();

} // namespace clickshade::linuxapp
