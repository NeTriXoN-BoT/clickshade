#pragma once

#include "settings.h"
#include <functional>
#include <memory>
#include <string>

namespace clickshade::linuxapp {

enum class HotkeyAction { ToggleAll, ToggleHighlight, ToggleClicks, ToggleSounds, ShowSettings, RestorePause, ToggleKeyboardSounds,
                          ToggleMagnifier, ToggleKeyDisplay };
constexpr std::size_t kHotkeyActionCount = 9;

struct HotkeyChord {
    enum : unsigned { Control = 1, Alt = 2, Shift = 4, Win = 8 };
    unsigned modifiers = 0;
    std::string key;
    std::string canonical;
};

// Empty disables a binding. Nonempty chords require Ctrl, Alt or Win/Super.
bool ParseHotkey(const std::string& text, HotkeyChord&, std::string& error);

// Standard X11 passive key grabs on a dedicated connection. No keyboard hooks,
// device access, event injection, activity recording, or Wayland fallback.
// All methods/callbacks belong to the GTK/GLib main context.
class GlobalHotkeys {
public:
    GlobalHotkeys();
    ~GlobalHotkeys();
    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;
    bool Start(const Settings&, std::function<void(HotkeyAction)>, std::string& error);
    bool Apply(const Settings&, std::string& error);
    void Stop();
    std::string LastError() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickshade::linuxapp
