#pragma once

#include <cstdint>
#include <string>

namespace clickshade::linuxapp {

// Colors use the same r | (g << 8) | (b << 16) packing as the Windows INI.
constexpr std::uint32_t Rgb(unsigned r, unsigned g, unsigned b) noexcept {
    return (r & 255u) | ((g & 255u) << 8u) | ((b & 255u) << 16u);
}

struct Settings {
    bool enabled = true;
    bool darkCursorEnabled = true;
    int cursorStyle = 0;
    bool cursorReverse = false;
    int cursorSize = 0;
    bool cursorHold = true;
    int cursorFlashMs = 120;
    int shakeStyle = 0;
    int shakeSensitivity = 100;
    int shakeMaximumScale = 400;
    std::string releasedCursorPath, pressedCursorPath;
    unsigned buttonMask = 3;
    bool pauseFullscreen = true;
    bool pauseConfinedCursor = true;
    bool startWithWindows = false; // Shared key; Linux implements XDG autostart.
    bool startMinimized = false;
    bool captureVisible = true; // Shared key; no X11 equivalent exists (retained for profiles).
    std::string excludedProcesses;
    std::string allowedProcesses;
    bool highlightEnabled = true;
    int highlightStyle = 0; // 0=filled, 1=ring, 2=glow, 3=crosshair, 4=spotlight (X11: dims the pointer's screen).
    int highlightRadius = 24;
    int highlightOpacity = 35;
    int highlightThickness = 3;
    std::uint32_t highlightColor = Rgb(255, 214, 64);
    bool highlightBreathing = false;
    int highlightIdleHideMs = 0;
    int spotlightDim = 55;
    int spotlightRadius = 140;
    bool trailEnabled = false;
    int trailStyle = 0;
    int trailRadius = 5;
    int trailOpacity = 45;
    int trailDurationMs = 220;
    std::uint32_t trailColor = Rgb(120, 226, 194);
    bool clickEffectsEnabled = true;
    bool leftClickEnabled = true;
    bool rightClickEnabled = true;
    bool middleClickEnabled = false;
    int clickStyle = 0; // 0=ring, 1=pulse, 2=ripple, 3=burst, 4=neon ring, 5=target.
    int clickRadius = 42;
    int clickDurationMs = 360;
    int clickOpacity = 80;
    int clickThickness = 3;
    int clickEasing = 1;
    int visualRefreshRate = 0;
    std::uint32_t leftClickColor = Rgb(0, 200, 255);
    std::uint32_t rightClickColor = Rgb(255, 49, 49);
    std::uint32_t middleClickColor = Rgb(57, 255, 20);
    bool clickSoundsEnabled = false;
    std::string leftSoundPath, rightSoundPath, middleSoundPath;
    int soundVolume = 75;
    bool keyboardSoundsEnabled = false;
    std::string keyboardSoundPath; // Set folder (key*/space/enter/backspace) or one file.
    int keyboardVolume = 60;
    bool magnifierEnabled = false; // X11: a lens beside the pointer; GNOME: a square lens from the extension.
    int magnifierZoom = 200; // Percent, 125..800.
    int magnifierSize = 220; // Lens diameter in logical pixels, 80..600.
    int magnifierShape = 0; // Shared key; 0=circle, 1=rounded square (X11 honors it).
    int magnifierPosition = 0; // Shared key; Linux always places the lens beside the pointer.
    std::uint32_t magnifierBorderColor = Rgb(255, 255, 255);
    bool magnifierFrame = true; // False leaves the lens without a colored frame.
    bool keyDisplayEnabled = false; // On-screen labels for pressed keys.
    int keyDisplayMode = 0; // 0=shortcuts only, 1=every key.
    bool keyDisplayMouse = false;
    int keyDisplayPosition = 0; // 0=bottom center, 1=top center, 2=bottom left, 3=bottom right.
    int keyDisplaySize = 100; // Percent, 60..250.
    int keyDisplayDurationMs = 1500; // 300..6000.
    int keyDisplayOpacity = 90; // 20..100.
    std::uint32_t keyDisplayColor = Rgb(28, 31, 38);
    std::uint32_t uiBackground = Rgb(17, 21, 28);
    std::uint32_t uiAccent = Rgb(120, 226, 194);
    bool uiDarkMode = true;
    std::uint32_t uiDarkBackground = Rgb(17, 21, 28);
    std::uint32_t uiDarkAccent = Rgb(120, 226, 194);
    std::uint32_t uiLightBackground = Rgb(241, 244, 249);
    std::uint32_t uiLightAccent = Rgb(54, 113, 215);
    int uiScalePercent = 0;
    bool uiAnimatedAccent = true;
    std::string hotkeyToggleAll, hotkeyHighlight, hotkeyClicks, hotkeySounds, hotkeySettings;
    // Ctrl+Alt+Fn is reserved for virtual-terminal switching on Xorg.
    std::string hotkeyRestore = "Ctrl+Shift+F10";
    std::string hotkeyKeyboardSounds;
    std::string hotkeyMagnifier, hotkeyKeyDisplay;
};

enum HighlightStyle : int { kHighlightFilled = 0, kHighlightRing, kHighlightGlow, kHighlightCrosshair, kHighlightSpotlight, kHighlightStyleCount };
enum ClickStyle : int { kClickRing = 0, kClickPulse, kClickRipple, kClickBurst, kClickNeon, kClickTarget, kClickStyleCount };
enum KeyCategory : unsigned { kKeyGeneric = 0, kKeySpace, kKeyEnter, kKeyBackspace, kKeyCategoryCount };
// Modifier flags for key-display labels; kModMouse marks a mouse button.
enum KeyModifier : unsigned { kModControl = 1, kModShift = 2, kModAlt = 4, kModWin = 8, kModAltGr = 16, kModMouse = 32 };

std::string ExecutablePath();
std::string SettingsPath();
Settings LoadSettings();
bool SaveSettings(const Settings&, std::string& error);
// Reset all preferences and remove the actual XDG autostart entry. No history
// or backup files are created. On persistence failure, retain current settings.
bool ResetSettings(Settings&, std::string& error);
bool ConfigureStartup(bool enabled, std::string& error);
bool IsStartupEnabled();
void NormalizeSettings(Settings&) noexcept;
// Explicit path variants support isolated tests and tools; no autostart changes.
bool LoadSettingsFile(const std::string& path, Settings&, std::string& error);
bool SaveSettingsFile(const std::string& path, const Settings&, std::string& error);
// Explicit profile operations; imports are Linux-only versioned snapshots.
// Loading a profile never changes preferences or startup registration by itself.
bool SaveSettingsProfile(const std::string& path, const Settings&, std::string& error);
bool LoadSettingsProfile(const std::string& path, Settings&, std::string& error);
std::string SettingsBackupPath(bool beforeImport = false);

} // namespace clickshade::linuxapp
