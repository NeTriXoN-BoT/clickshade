#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace clickshade {

// Every member's initializer is the documented default. Settings{} is the
// reference for the per-control Reset buttons, Restore defaults and
// settings-file upgrades, so change a default here and nowhere else.
struct Settings {
    bool enabled = true;
    bool darkCursorEnabled = true;
    int cursorStyle = 0; // 0=current scheme, 1=Windows arrow, 2=classic arrow, 3=custom arrow pair.
    bool cursorReverse = false;
    // Percent of the size Windows draws the cursor at (100..600); 100 keeps
    // the source size. Larger scales are limited to 256 image pixels.
    int cursorScale = 100;
    int cursorOpacity = 100; // 30..100 percent, every cursor shape.
    bool cursorShadow = false; // Soft drop shadow under every cursor shape.
    bool cursorColors = false; // Repaint cursors with the fill and outline colors.
    COLORREF cursorFillColor = RGB(255, 214, 64);
    COLORREF cursorOutlineColor = RGB(24, 24, 28);
    bool cursorHold = true;
    int cursorFlashMs = 120;
    int shakeStyle = 0; // 0=off, 1=Plasma-style, 2=macOS-inspired.
    int shakeSensitivity = 100; // Percent, 50..200; higher needs less shake motion.
    int shakeMaximumScale = 400; // Percent, 150..400; native image cap also applies.
    std::wstring releasedCursorPath;
    std::wstring pressedCursorPath; // Empty: derive pressed appearance by inversion.
    unsigned buttonMask = 3; // Left=1, right=2, middle=4, X1=8, X2=16.
    bool pauseFullscreen = true;
    bool pauseConfinedCursor = true;
    bool startWithWindows = false;
    bool startMinimized = false;
    bool runAsAdministrator = false; // Relaunch elevated so effects also work over admin windows.
    bool captureVisible = true; // False excludes the overlays from screen capture/recording.
    std::wstring excludedProcesses; // Semicolon-separated executable basenames.
    std::wstring allowedProcesses; // Bypasses fullscreen/confinement only; exclusions win.
    bool highlightEnabled = true;
    int highlightStyle = 0; // 0=filled, 1=ring, 2=glow, 3=crosshair, 4=spotlight.
    int highlightRadius = 24;
    int highlightOpacity = 35;
    int highlightThickness = 3; // Logical pixels, outline mode.
    COLORREF highlightColor = RGB(255, 214, 64);
    bool highlightBreathing = false; // Gentle opacity pulse while the pointer rests.
    int highlightIdleHideMs = 0; // 0=always shown; otherwise hide after this idle time (500..10000).
    int spotlightDim = 55; // Percent darkness outside the spotlight circle (10..90).
    int spotlightRadius = 140; // Logical pixels (40..400).
    bool trailEnabled = false;
    int trailStyle = 0; // 0=dots, 1=rings, 2=sparkles, 3=comet, 4=fine sparkle.
    int trailRadius = 5; // Logical pixels.
    int trailOpacity = 45;
    int trailDurationMs = 220;
    COLORREF trailColor = RGB(120, 226, 194);
    bool clickEffectsEnabled = true;
    bool leftClickEnabled = true;
    bool rightClickEnabled = true;
    bool middleClickEnabled = false;
    int clickStyle = 0; // 0=ring, 1=pulse, 2=ripple, 3=burst, 4=neon ring, 5=target.
    int clickRadius = 42;
    int clickDurationMs = 360;
    int clickOpacity = 80;
    int clickThickness = 3;
    int clickEasing = 1; // 0=linear, 1=cubic ease-out, 2=smoothstep.
    int visualRefreshRate = 0; // 0=locally adaptive; manual frame limit 30..360 Hz.
    COLORREF leftClickColor = RGB(0, 200, 255); // Neon blue.
    COLORREF rightClickColor = RGB(255, 49, 49); // Neon red.
    COLORREF middleClickColor = RGB(57, 255, 20); // Neon green.
    bool clickSoundsEnabled = false;
    std::wstring leftSoundPath;
    std::wstring rightSoundPath;
    std::wstring middleSoundPath;
    int soundVolume = 75;
    bool keyboardSoundsEnabled = false;
    std::wstring keyboardSoundPath; // A sound-set folder (key/space/enter/backspace files) or one file.
    int keyboardVolume = 60;
    bool magnifierEnabled = false; // A zoomed lens that follows the pointer.
    int magnifierZoom = 200; // Percent, 125..800.
    int magnifierSize = 220; // Lens diameter in DIP, 80..600.
    int magnifierShape = 0; // 0=circle, 1=rounded square.
    int magnifierPosition = 0; // 0=centered on the pointer, 1=beside the pointer.
    COLORREF magnifierBorderColor = RGB(255, 255, 255);
    bool magnifierFrame = true; // False leaves the lens without a colored frame.
    bool keyDisplayEnabled = false; // On-screen labels for pressed keys.
    int keyDisplayMode = 0; // 0=shortcuts only (Ctrl/Alt/Win chords and function keys), 1=every key.
    bool keyDisplayMouse = false; // Also label mouse button presses.
    int keyDisplayPosition = 0; // 0=bottom center, 1=top center, 2=bottom left, 3=bottom right.
    int keyDisplaySize = 100; // Percent, 60..250.
    int keyDisplayDurationMs = 1500; // How long a label stays after the last press, 300..6000.
    int keyDisplayOpacity = 90; // Backdrop opacity, 20..100.
    COLORREF keyDisplayColor = RGB(28, 31, 38); // Backdrop; the text color is derived for contrast.
    COLORREF uiBackground = RGB(17, 21, 28);
    COLORREF uiAccent = RGB(120, 226, 194);
    bool uiDarkMode = true;
    COLORREF uiDarkBackground = RGB(17, 21, 28);
    COLORREF uiDarkAccent = RGB(120, 226, 194);
    COLORREF uiLightBackground = RGB(241, 244, 249);
    COLORREF uiLightAccent = RGB(54, 113, 215);
    int uiScalePercent = 0; // 0=automatic display fit, otherwise extra UI zoom (100..200%).
    bool uiAnimatedAccent = true; // The glowing spectrum line above the footer.
    std::wstring hotkeyToggleAll, hotkeyHighlight, hotkeyClicks, hotkeySounds, hotkeySettings;
    std::wstring hotkeyRestore = L"Ctrl+Alt+F10";
    std::wstring hotkeyKeyboardSounds;
    std::wstring hotkeyMagnifier, hotkeyKeyDisplay;
};

// Click-style / highlight-style identifiers shared by the GUI and renderer.
enum HighlightStyle : int { kHighlightFilled = 0, kHighlightRing, kHighlightGlow, kHighlightCrosshair, kHighlightSpotlight, kHighlightStyleCount };
enum ClickStyle : int { kClickRing = 0, kClickPulse, kClickRipple, kClickBurst, kClickNeon, kClickTarget, kClickStyleCount };

// Keyboard sound categories. Only the category of a key reaches the audio
// worker; no key code, character or timing is stored anywhere.
enum KeyCategory : unsigned { kKeyGeneric = 0, kKeySpace, kKeyEnter, kKeyBackspace, kKeyCategoryCount };

// Modifier state attached to a key-display event. kModMouse marks a mouse
// button (VK_LBUTTON..VK_XBUTTON2) instead of a keyboard key.
enum KeyModifier : unsigned { kModControl = 1, kModShift = 2, kModAlt = 4, kModWin = 8, kModAltGr = 16, kModMouse = 32, kModCapsLock = 64 };

struct RuntimeStatus {
    float locatorScale = 1.f; // Current logical locator size; no persisted history.
    bool ready = false;
    bool pressed = false;
    bool fullscreen = false;
    bool cursorConfined = false;
    bool sessionLocked = false;
    bool pausedByRule = false;
    bool foregroundElevated = false; // The foreground window belongs to a higher-integrity process.
    unsigned heldButtons = 0;
    std::size_t cursorCount = 0;
    std::uint64_t updateCount = 0;
    std::uint64_t previewRevision = 0;
    double lastUpdateMicroseconds = 0;
    std::wstring foregroundProcess;
    std::wstring stateText = L"Starting cursor engine...";
    std::wstring lastError;
    std::wstring visualError;
    std::wstring audioError;
};

// A dedicated message-driven thread owns Raw Input and the cursor engine.
// Settings UI drawing must never delay the input path.
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Start(HWND notifyWindow, UINT notifyMessage, const Settings& settings,
               std::wstring& error);
    void Stop();
    void UpdateSettings(const Settings& settings);
    void RefreshCursors();
    void PreviewSound(unsigned buttonMask);
    // Plays the keyboard set's generic key sound, ignoring the enable switch.
    void PreviewKeyboardSound();
    // Shows a sample key-display label, ignoring the enable switch.
    void PreviewKeyDisplay();
    RuntimeStatus Snapshot() const;
    // The caller owns this copy and must call DestroyCursor.
    HCURSOR CopyPreview(bool dark) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickshade
