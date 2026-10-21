#include "engine.h"
#include "audio.h"
#include "x11_errors.h"
#include "../src/frame_pacing.h"
#include "../src/shake_locator.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#undef Status
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <gio/gio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

namespace clickshade::linuxapp {
namespace {
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kParticleCount = 8;
constexpr std::size_t kTrailCount = 32;
constexpr std::size_t kShakeFrames = 32;
constexpr std::uint64_t kShakeCacheBytes = 24u * 1024u * 1024u;
constexpr std::size_t kKeyRowLimit = 3; // Rows on the key display; the oldest leaves first.
constexpr std::size_t kTypedBytes = 480; // UTF-8 bytes kept in one typed row.
constexpr auto kKeyFade = std::chrono::milliseconds(350);
constexpr auto kLensInterval = std::chrono::milliseconds(33); // Live lens redraws while the pointer rests.
double Seconds(TimePoint now) { return std::chrono::duration<double>(now.time_since_epoch()).count(); }
constexpr auto kSafetyInterval = std::chrono::milliseconds(200);

bool IsModifierKeysym(KeySym symbol) noexcept {
    switch (symbol) {
    case XK_Control_L: case XK_Control_R: case XK_Shift_L: case XK_Shift_R: case XK_Alt_L: case XK_Alt_R:
    case XK_Meta_L: case XK_Meta_R: case XK_Super_L: case XK_Super_R: case XK_Hyper_L: case XK_Hyper_R:
    case XK_ISO_Level3_Shift: case XK_Mode_switch:
        return true;
    default: return false;
    }
}
unsigned ModifierBitOf(KeySym symbol) noexcept {
    switch (symbol) {
    case XK_Control_L: case XK_Control_R: return kModControl;
    case XK_Shift_L: case XK_Shift_R: return kModShift;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R: return kModAlt;
    case XK_Super_L: case XK_Super_R: case XK_Hyper_L: case XK_Hyper_R: return kModWin;
    case XK_ISO_Level3_Shift: case XK_Mode_switch: return kModAltGr;
    default: return 0;
    }
}
// Shortcuts-only mode: chords with Ctrl, Alt or Super, function keys, Escape,
// Print and a tapped modifier. AltGr characters count as typing.
bool KeyDisplayWanted(int mode, KeySym symbol, unsigned modifiers) noexcept {
    if (modifiers & kModMouse) return true;
    if (mode != 0) return true;
    if (modifiers & kModAltGr) return false;
    if (modifiers & (kModControl | kModAlt | kModWin)) return true;
    if (symbol >= XK_F1 && symbol <= XK_F35) return true;
    return symbol == XK_Escape || symbol == XK_Print || IsModifierKeysym(symbol);
}
std::string Utf8(unsigned codepoint) {
    std::string out;
    if (codepoint < 0x80) out += static_cast<char>(codepoint);
    else if (codepoint < 0x800) { out += static_cast<char>(0xc0 | (codepoint >> 6)); out += static_cast<char>(0x80 | (codepoint & 0x3f)); }
    else if (codepoint < 0x10000) {
        out += static_cast<char>(0xe0 | (codepoint >> 12)); out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (codepoint >> 18)); out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)); out += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
    return out;
}
// A short label for one keysym. Named keys use fixed names; characters come
// from the base level of the current layout, shown in upper case.
std::string KeyName(KeySym symbol, unsigned modifiers) {
    if (modifiers & kModMouse) {
        switch (symbol) {
        case 1: return "Left click"; case 2: return "Right click"; case 4: return "Middle click";
        case 8: return "Back button"; case 16: return "Forward button"; default: return "Mouse";
        }
    }
    if (symbol >= XK_F1 && symbol <= XK_F35) return "F" + std::to_string(symbol - XK_F1 + 1);
    if (symbol >= XK_KP_0 && symbol <= XK_KP_9) return std::string("Num ") + static_cast<char>('0' + (symbol - XK_KP_0));
    switch (symbol) {
    case XK_Control_L: case XK_Control_R: return "Ctrl";
    case XK_Shift_L: case XK_Shift_R: return "Shift";
    case XK_Alt_L: case XK_Meta_L: return "Alt";
    case XK_Alt_R: case XK_Meta_R: return (modifiers & kModAltGr) ? "AltGr" : "Right Alt";
    case XK_ISO_Level3_Shift: case XK_Mode_switch: return "AltGr";
    case XK_Super_L: case XK_Super_R: case XK_Hyper_L: case XK_Hyper_R: return "Super";
    case XK_Escape: return "Esc"; case XK_Tab: case XK_ISO_Left_Tab: return "Tab"; case XK_Caps_Lock: return "Caps Lock";
    case XK_Return: case XK_KP_Enter: return "Enter"; case XK_BackSpace: return "Backspace"; case XK_space: return "Space";
    case XK_Insert: case XK_KP_Insert: return "Insert"; case XK_Delete: case XK_KP_Delete: return "Delete";
    case XK_Home: case XK_KP_Home: return "Home"; case XK_End: case XK_KP_End: return "End";
    case XK_Page_Up: case XK_KP_Page_Up: return "Page Up"; case XK_Page_Down: case XK_KP_Page_Down: return "Page Down";
    case XK_Left: case XK_KP_Left: return "←"; case XK_Up: case XK_KP_Up: return "↑";
    case XK_Right: case XK_KP_Right: return "→"; case XK_Down: case XK_KP_Down: return "↓";
    case XK_KP_Begin: return "Clear"; case XK_Print: return "Print Screen"; case XK_Scroll_Lock: return "Scroll Lock";
    case XK_Pause: return "Pause"; case XK_Num_Lock: return "Num Lock"; case XK_Menu: return "Menu";
    case XK_KP_Multiply: return "Num *"; case XK_KP_Add: return "Num +"; case XK_KP_Subtract: return "Num -";
    case XK_KP_Decimal: case XK_KP_Separator: return "Num ."; case XK_KP_Divide: return "Num /";
    case 0x1008ff12: return "Mute"; case 0x1008ff11: return "Volume Down"; case 0x1008ff13: return "Volume Up";
    case 0x1008ff17: return "Next Track"; case 0x1008ff16: return "Previous Track"; case 0x1008ff15: return "Stop";
    case 0x1008ff14: return "Play/Pause"; case 0x1008ff26: return "Back"; case 0x1008ff27: return "Forward";
    default: break;
    }
    if (symbol >= 0x20 && symbol <= 0x7e) {
        char c = static_cast<char>(symbol);
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return std::string(1, c);
    }
    if (symbol >= 0xa0 && symbol <= 0xff) return Utf8(static_cast<unsigned>(symbol >= 0xe0 && symbol <= 0xfe && symbol != 0xf7 ? symbol - 0x20 : symbol));
    if ((symbol & 0xff000000u) == 0x01000000u) return Utf8(static_cast<unsigned>(symbol & 0x00ffffffu));
    if (const char* text = XKeysymToString(symbol)) {
        std::string name(text);
        for (char& c : name) if (c == '_') c = ' ';
        if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
        return name;
    }
    return "Key";
}
// The keys of a chord in display order: modifiers first (Super, Ctrl, Alt,
// Shift), then the key. A modifier that is itself the key is not repeated.
std::vector<std::string> ChordParts(KeySym symbol, unsigned modifiers) {
    std::vector<std::string> parts;
    unsigned own = (modifiers & kModMouse) ? 0u : ModifierBitOf(symbol);
    if (own & kModAlt) own |= kModAltGr;
    const unsigned shown = modifiers & ~own;
    if (shown & kModWin) parts.emplace_back("Super");
    if (shown & kModAltGr) parts.emplace_back("AltGr");
    else {
        if (shown & kModControl) parts.emplace_back("Ctrl");
        if (shown & kModAlt) parts.emplace_back("Alt");
    }
    if (shown & kModShift) parts.emplace_back("Shift");
    parts.push_back(KeyName(symbol, modifiers));
    return parts;
}
// The text a keysym types, or nothing for function, control and dead keys.
std::string KeysymText(KeySym symbol) {
    if (symbol == XK_space) return " ";
    if (symbol >= 0x21 && symbol <= 0x7e) return std::string(1, static_cast<char>(symbol));
    if (symbol >= 0xa0 && symbol <= 0xff) return Utf8(static_cast<unsigned>(symbol));
    if (symbol == XK_EuroSign) return Utf8(0x20ac);
    if (symbol >= XK_KP_0 && symbol <= XK_KP_9) return std::string(1, static_cast<char>('0' + (symbol - XK_KP_0)));
    if ((symbol & 0xff000000u) == 0x01000000u) {
        const unsigned codepoint = static_cast<unsigned>(symbol & 0x00ffffffu);
        if (codepoint >= 0x20 && codepoint != 0x7f && (codepoint < 0x80 || codepoint >= 0xa0) && codepoint <= 0x10ffff) return Utf8(codepoint);
    }
    return {};
}
// Removes the last UTF-8 character of a string.
void PopCharacter(std::string& text) {
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
    if (!text.empty()) text.pop_back();
}
// Removes the first UTF-8 character of a string.
void DropFirstCharacter(std::string& text) {
    std::size_t length = 1;
    while (length < text.size() && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) ++length;
    text.erase(0, std::min(length, text.size()));
}
void RoundedRect(cairo_t* cr, double x, double y, double width, double height, double radius) {
    radius = std::max(0.0, std::min({radius, width / 2, height / 2}));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -kPi / 2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, kPi / 2);
    cairo_arc(cr, x + radius, y + height - radius, radius, kPi / 2, kPi);
    cairo_arc(cr, x + radius, y + radius, radius, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

// Xlib's handler is process-wide. Only errors on this engine's connection are
// intercepted; GTK and all other connections retain their original handler.
std::atomic<Display*> gEngineDisplay{nullptr};
std::atomic<int> gEngineError{0};
std::atomic<unsigned long> gErrorResource{0};
std::atomic<XErrorHandler> gPreviousErrorHandler{nullptr};
int EngineXError(Display* display, XErrorEvent* event) {
    if (display == gEngineDisplay.load(std::memory_order_acquire)) {
        gErrorResource.store(event->resourceid, std::memory_order_relaxed);
        gEngineError.store(event->error_code, std::memory_order_release);
        return 0;
    }
    const auto previous = gPreviousErrorHandler.load(std::memory_order_acquire);
    return previous ? previous(display, event) : 0;
}
std::string Lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
std::vector<std::string> ParseExclusions(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input(text);
    for (std::string item; std::getline(input, item, ';');) {
        const auto begin = item.find_first_not_of(" \r\n\t");
        if (begin == std::string::npos) continue;
        const auto end = item.find_last_not_of(" \r\n\t");
        item = Lower(item.substr(begin, end - begin + 1));
        const auto slash = item.find_last_of("/\\");
        if (slash != std::string::npos) item.erase(0, slash + 1);
        if (!item.empty() && item.size() <= 1024) result.push_back(std::move(item));
        if (result.size() == 256) break;
    }
    return result;
}
unsigned ButtonBit(unsigned button) {
    switch (button) { case 1: return 1; case 3: return 2; case 2: return 4; case 8: return 8; case 9: return 16; default: return 0; }
}
bool SameVisuals(const Settings& a, const Settings& b) {
    return a.highlightEnabled == b.highlightEnabled && a.highlightStyle == b.highlightStyle &&
        a.highlightRadius == b.highlightRadius && a.highlightOpacity == b.highlightOpacity &&
        a.highlightThickness == b.highlightThickness && a.highlightColor == b.highlightColor &&
        a.highlightBreathing == b.highlightBreathing && a.highlightIdleHideMs == b.highlightIdleHideMs &&
        a.spotlightDim == b.spotlightDim && a.spotlightRadius == b.spotlightRadius &&
        a.trailEnabled == b.trailEnabled && a.trailStyle == b.trailStyle &&
        a.trailRadius == b.trailRadius && a.trailOpacity == b.trailOpacity &&
        a.trailDurationMs == b.trailDurationMs && a.trailColor == b.trailColor &&
        a.clickEffectsEnabled == b.clickEffectsEnabled && a.leftClickEnabled == b.leftClickEnabled &&
        a.rightClickEnabled == b.rightClickEnabled && a.middleClickEnabled == b.middleClickEnabled &&
        a.clickStyle == b.clickStyle &&
        a.clickRadius == b.clickRadius && a.clickDurationMs == b.clickDurationMs &&
        a.clickOpacity == b.clickOpacity && a.clickThickness == b.clickThickness &&
        a.clickEasing == b.clickEasing && a.leftClickColor == b.leftClickColor &&
        a.rightClickColor == b.rightClickColor && a.middleClickColor == b.middleClickColor &&
        a.magnifierEnabled == b.magnifierEnabled && a.magnifierZoom == b.magnifierZoom &&
        a.magnifierSize == b.magnifierSize && a.magnifierShape == b.magnifierShape &&
        a.magnifierBorderColor == b.magnifierBorderColor && a.magnifierFrame == b.magnifierFrame &&
        a.keyDisplayEnabled == b.keyDisplayEnabled && a.keyDisplayMouse == b.keyDisplayMouse &&
        a.keyDisplayPosition == b.keyDisplayPosition && a.keyDisplaySize == b.keyDisplaySize &&
        a.keyDisplayDurationMs == b.keyDisplayDurationMs && a.keyDisplayOpacity == b.keyDisplayOpacity &&
        a.keyDisplayColor == b.keyDisplayColor;
}
void Color(cairo_t* cr, std::uint32_t packed, double opacity) {
    cairo_set_source_rgba(cr, (packed & 255) / 255.0, ((packed >> 8) & 255) / 255.0,
        ((packed >> 16) & 255) / 255.0, opacity);
}

class X11Engine final : public LinuxEngine {
public:
    X11Engine() : wake_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
    ~X11Engine() override { Stop(); if (wake_ >= 0) close(wake_); }
    bool Start(const Settings& settings, std::string& error) override;
    void Update(const Settings& settings) override;
    void Stop() override;
    EngineStatus Status() const override;
    void PreviewSound(unsigned buttonMask) override { audio_.Preview(buttonMask); }
    void PreviewKeyboardSound() override { audio_.PreviewKey(); }
    void Restore() override;
private:
    struct Overlay {
        Window window = None;
        cairo_surface_t* surface = nullptr;
        int width = 0, height = 0, x = 0, y = 0;
        bool mapped = false;
        bool painted = false; // Uniform panels are filled once per size.
    };
    struct Particle { Overlay overlay; bool active = false; int x = 0, y = 0; unsigned button = 0; TimePoint started{}; };
    struct Trail { Overlay overlay; bool active = false; int x = 0, y = 0; double direction = 0; TimePoint started{}; };
    struct CursorFrame { cairo_surface_t* normal = nullptr; cairo_surface_t* dark = nullptr; int hotX = 0, hotY = 0; };
    struct Monitor { int x = 0, y = 0, width = 0, height = 0; unsigned rate = 60; };
    void Wake() noexcept;
    void Run() noexcept;
    bool Initialize(std::string& error);
    void Cleanup() noexcept;
    void Publish();
    void Commands();
    void ProcessEvent(XEvent& event);
    bool SelectInput();
    void RawKey(const XIRawEvent* raw, bool pressed);
    unsigned CurrentModifiers();
    bool KeysAllowed() const noexcept;
    void ShowKey(KeySym symbol, unsigned modifiers, const std::string& typed = {});
    std::string TypedText(KeyCode code, unsigned modifiers);
    void PaintKeyDisplay(TimePoint now);
    void HideKeys() noexcept;
    void PaintLens(TimePoint now);
    void HideLens() noexcept;
    void ScheduleKeyFrame();
    void RawButton(int device, unsigned button, bool down);
    void ReconcileButtons();
    void UpdatePointerDevices();
    void UpdateForeground(bool force = false);
    void UpdateMonitors();
    void UpdatePointer();
    void CaptureCursor();
    void RebuildShakeCache();
    void ClearShakeCache() noexcept;
    void UpdateCursorEffect(double now);
    void ObserveShake();
    void ApplyPolicy();
    void RestorePointer() noexcept;
    void ClearEffects(bool holdSpotlightAndLens = false) noexcept;
    void ScheduleFrame(bool immediate);
    void Render(TimePoint now);
    void AddParticle(unsigned button);
    void AddTrail(TimePoint now);
    bool EnsureOverlay(Overlay&, int width, int height, const char* role, int limit = 2048);
    void Hide(Overlay&) noexcept;
    void Destroy(Overlay&) noexcept;
    void Place(Overlay&, int x, int y);
    void PaintHighlight(TimePoint now);
    void PaintSpotlight();
    void HideSpotlight() noexcept;
    void ResetSpotlight() noexcept;
    void PaintCursor(double now);
    void PaintParticle(Particle&, double progress);
    void PaintTrail(Trail&, double progress);
    bool OwnOverlay(Window) const noexcept;
    void RepairStacking();
    bool FullscreenPaused() const noexcept;
    void GraphicsError(const std::string& error);
    void CheckXError();
    unsigned MonitorRate() const;
    std::vector<unsigned long> Property(Window window, Atom property, Atom type, unsigned limit = 256);
    void SubscribeLock();
    void UnsubscribeLock();
    static void LockSignal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer);

    int wake_ = -1;
    std::thread thread_;
    std::atomic<bool> stopping_{false}, settingsPending_{false}, restorePending_{false}, locked_{false};
    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    Settings pending_, settings_;
    EngineStatus status_;
    bool startFinished_ = false, startSucceeded_ = false;
    std::string startError_;
    LinuxAudio audio_;
    GDBusConnection* sessionBus_ = nullptr;
    std::array<guint, 2> lockSubscriptions_{};

    Display* display_ = nullptr;
    Window root_ = None, foreground_ = None;
    int screen_ = 0, xiOpcode_ = 0, fixesEvent_ = 0, randrEvent_ = 0, masterPointer_ = 0;
    bool randr_ = false, handlerInstalled_ = false, compositor_ = false, graphicsFailed_ = false, multiplePointers_ = false;
    Visual* visual_ = nullptr;
    Colormap colormap_ = None;
    Atom activeAtom_ = None, wmStateAtom_ = None, fullscreenAtom_ = None, typeAtom_ = None,
        desktopAtom_ = None, dockAtom_ = None, pidAtom_ = None, compositorAtom_ = None,
        frameExtentsAtom_ = None, maximizedHorizontalAtom_ = None, maximizedVerticalAtom_ = None;
    bool foregroundFullscreen_ = false, foregroundExcluded_ = false, foregroundShell_ = false, foregroundAllowed_ = false;
    std::string foregroundName_, graphicsError_;
    std::vector<std::string> exclusions_, allowances_;
    std::vector<Monitor> monitors_;
    std::unordered_map<int, unsigned> devices_;
    unsigned held_ = 0;
    int pointerX_ = 0, pointerY_ = 0;
    double scale_ = 1.0;
    bool cursorVisible_ = true, allowed_ = false, ownHide_ = false, pointerDirty_ = true;
    // Paused only because the application shows a transparent cursor (editors
    // do that while you type): the spotlight and the lens keep their place.
    bool typingHold_ = false;
    bool cursorDirty_ = true, highlightDirty_ = true, pressed_ = false;
    bool lastLocked_ = false;
    bool pulse_ = false, frameScheduled_ = false;
    TimePoint pulseDeadline_{}, nextFrame_{}, nextSafety_{}, lastRuleCheck_{};
    TimePoint nextPointerFrame_{}, nextEffectsFrame_{}, lastEffectsFrame_{};
    AdaptiveFramePacer framePacer_;
    ShakeLocator shake_;
    bool shakeMotionPending_ = false;
    int pointerMonitor_ = -1;
    std::array<CursorFrame, kShakeFrames> shakeFrames_{};
    unsigned shakeFrameCount_ = 0;
    cairo_surface_t* paintedCursor_ = nullptr;
    std::string shakeError_;
    Overlay highlight_, cursor_;
    // Spotlight: one window over the pointer's monitor. A server-side pixmap
    // holds the whole shade; a move redraws the old and new circle there and
    // copies just that rectangle to the window in one request, so the
    // compositor never shows the dim area and the circle out of step.
    Overlay spotlight_;
    cairo_surface_t* spotPixmap_ = nullptr;
    int spotMonitorX_ = 0, spotMonitorY_ = 0, spotWidth_ = 0, spotHeight_ = 0, spotSize_ = 0;
    struct HoleRect { int x = 0, y = 0, size = 0; } spotHole_{};
    bool keyboardSelected_ = false, keyboardReleases_ = false;
    TimePoint lastMotion_{};
    // Key display: rows of keycaps (or typed text) that live in memory only
    // while shown. Each row fades on its own after the hold time.
    struct KeyStroke { std::vector<std::string> caps; std::string typed; unsigned repeat = 1; TimePoint updated{}; };
    Overlay keyPanel_;
    std::vector<KeyStroke> keyStrokes_;
    TimePoint keyDeadline_{};
    bool keyVisible_ = false, keyDirty_ = false;
    unsigned chordModifiers_ = 0;
    bool modifierChordUsed_ = false;
    // Magnifier lens beside the pointer, refreshed from the screen.
    Overlay lens_;
    bool lensVisible_ = false;
    TimePoint nextLensFrame_{};
    std::string lensError_;
    std::array<Particle, kParticleCount> particles_{};
    std::size_t nextParticle_ = 0;
    std::array<Trail, kTrailCount> trails_{};
    std::size_t nextTrail_ = 0;
    bool trailMotionPending_ = false, trailAnchorSet_ = false, stackDirty_ = false, stackOrderDirty_ = false, foreignStackDirty_ = false;
    int lastTrailX_ = 0, lastTrailY_ = 0;
    TimePoint lastTrailSample_{};
    cairo_surface_t* darkImage_ = nullptr;
    cairo_surface_t* normalImage_ = nullptr;
    int cursorHotX_ = 0, cursorHotY_ = 0;
    unsigned long cursorSerial_ = 0;
    std::uint64_t inputEvents_ = 0, renderedFrames_ = 0, wakeups_ = 0;
    std::atomic<std::uint64_t> inputSnapshot_{0}, frameSnapshot_{0}, wakeSnapshot_{0};
};

void X11Engine::Wake() noexcept {
    const std::uint64_t one = 1;
    if (wake_ >= 0) { const auto ignored = write(wake_, &one, sizeof(one)); (void)ignored; }
}
bool X11Engine::Start(const Settings& settings, std::string& error) {
    Stop(); error.clear();
    if (wake_ < 0) { error = "Could not create the X11 worker wake event."; return false; }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = settings; NormalizeSettings(pending_);
        startFinished_ = startSucceeded_ = false; startError_.clear();
        status_ = {}; status_.backend = "X11";
    }
    stopping_.store(false); restorePending_.store(false); settingsPending_.store(false);
    locked_.store(false);
    SubscribeLock();
    try { thread_ = std::thread([this] { Run(); }); }
    catch (const std::exception& e) { UnsubscribeLock(); error = e.what(); return false; }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!readyCv_.wait_for(lock, std::chrono::seconds(5), [this] { return startFinished_; })) {
        error = "The X11 backend did not initialize within five seconds.";
        lock.unlock(); Stop(); return false;
    }
    error = startError_;
    return startSucceeded_;
}
void X11Engine::Update(const Settings& settings) {
    { std::lock_guard<std::mutex> lock(mutex_); pending_ = settings; NormalizeSettings(pending_); }
    settingsPending_.store(true, std::memory_order_release); Wake();
}
void X11Engine::Restore() { restorePending_.store(true, std::memory_order_release); Wake(); }
void X11Engine::Stop() {
    UnsubscribeLock();
    stopping_.store(true, std::memory_order_release); Wake();
    if (thread_.joinable()) thread_.join();
    audio_.Stop();
}
EngineStatus X11Engine::Status() const {
    EngineStatus copy;
    { std::lock_guard<std::mutex> lock(mutex_); copy = status_; }
    copy.inputEvents = inputSnapshot_.load(std::memory_order_relaxed);
    copy.renderedFrames = frameSnapshot_.load(std::memory_order_relaxed);
    copy.wakeups = wakeSnapshot_.load(std::memory_order_relaxed);
    const auto audioError = audio_.LastError();
    if (!audioError.empty()) copy.lastError += (copy.lastError.empty() ? "" : "\n") + audioError;
    return copy;
}
void X11Engine::SubscribeLock() {
    GError* error = nullptr;
    sessionBus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error) g_error_free(error);
    if (!sessionBus_) return;
    const std::array<const char*, 2> interfaces{{"org.freedesktop.ScreenSaver", "org.gnome.ScreenSaver"}};
    for (unsigned i = 0; i < interfaces.size(); ++i)
        lockSubscriptions_[i] = g_dbus_connection_signal_subscribe(sessionBus_, nullptr, interfaces[i], "ActiveChanged",
            nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, LockSignal, this, nullptr);
}
void X11Engine::UnsubscribeLock() {
    if (!sessionBus_) return;
    for (auto& id : lockSubscriptions_) { if (id) g_dbus_connection_signal_unsubscribe(sessionBus_, id); id = 0; }
    g_object_unref(sessionBus_); sessionBus_ = nullptr;
}
void X11Engine::LockSignal(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
    auto& self = *static_cast<X11Engine*>(data);
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) return;
    gboolean value = false; g_variant_get(parameters, "(b)", &value);
    self.locked_.store(value, std::memory_order_release); self.Wake();
}
std::vector<unsigned long> X11Engine::Property(Window window, Atom property, Atom type, unsigned limit) {
    Atom actual = None; int format = 0; unsigned long count = 0, after = 0; unsigned char* data = nullptr;
    std::vector<unsigned long> result;
    if (window && XGetWindowProperty(display_, window, property, 0, limit, False, type,
        &actual, &format, &count, &after, &data) == Success && actual == type && format == 32 && data)
        result.assign(reinterpret_cast<unsigned long*>(data), reinterpret_cast<unsigned long*>(data) + count);
    if (data) XFree(data);
    return result;
}
bool X11Engine::Initialize(std::string& error) {
    { std::lock_guard<std::mutex> lock(mutex_); settings_ = pending_; }
    held_ = 0; devices_.clear(); foreground_ = None; foregroundName_.clear(); masterPointer_ = 0;
    allowed_ = pressed_ = ownHide_ = pulse_ = frameScheduled_ = typingHold_ = false;
    trailMotionPending_ = trailAnchorSet_ = stackDirty_ = stackOrderDirty_ = foreignStackDirty_ = false;
    nextParticle_ = nextTrail_ = 0;
    pointerDirty_ = cursorDirty_ = highlightDirty_ = true;
    graphicsFailed_ = false; graphicsError_.clear(); visual_ = nullptr; cursorSerial_ = 0; scale_ = 1.0;
    lastLocked_ = locked_.load(std::memory_order_acquire);
    inputEvents_ = renderedFrames_ = wakeups_ = 0;
    inputSnapshot_.store(0); frameSnapshot_.store(0); wakeSnapshot_.store(0);
    nextFrame_ = {}; nextSafety_ = {}; lastRuleCheck_ = {};
    nextPointerFrame_ = {}; nextEffectsFrame_ = {}; lastEffectsFrame_ = {};
    framePacer_ = {};
    shake_.Configure(settings_.shakeStyle, settings_.shakeSensitivity, settings_.shakeMaximumScale);
    shakeMotionPending_ = false; pointerMonitor_ = -1; paintedCursor_ = nullptr; shakeError_.clear();
    keyboardSelected_ = keyboardReleases_ = false; lastMotion_ = Clock::now();
    keyStrokes_.clear(); keyVisible_ = keyDirty_ = false; keyDeadline_ = {}; chordModifiers_ = 0; modifierChordUsed_ = false;
    spotHole_ = {}; spotSize_ = 0;
    lensVisible_ = false; nextLensFrame_ = {}; lensError_.clear();
    exclusions_ = ParseExclusions(settings_.excludedProcesses);
    allowances_ = ParseExclusions(settings_.allowedProcesses);
    display_ = XOpenDisplay(nullptr);
    if (!display_) { error = "Could not open a native X11 display. Wayland needs the supported compositor extension."; return false; }
    Display* expected = nullptr;
    if (!gEngineDisplay.compare_exchange_strong(expected, display_)) {
        error = "Only one X11 ClickShade backend may run in this process."; return false;
    }
    {
        std::lock_guard<std::mutex> lifecycle(XErrorHandlerLifecycleMutex());
        gEngineError.store(0);
        gPreviousErrorHandler.store(XSetErrorHandler(EngineXError)); handlerInstalled_ = true;
    }
    screen_ = DefaultScreen(display_); root_ = RootWindow(display_, screen_);
    int ignored = 0;
    if (!XQueryExtension(display_, "XInputExtension", &xiOpcode_, &ignored, &ignored)) {
        error = "This X server does not provide XInput2 raw mouse events."; return false;
    }
    // XI 2.0 suppresses raw events during another client's implicit button
    // grab. XI 2.1+ delivers release/overlap events throughout an ordinary drag.
    int major = 2, minor = 1;
    if (XIQueryVersion(display_, &major, &minor) != Success || major < 2 || (major == 2 && minor < 1)) {
        error = "XInput2 version 2.1 or newer is required for reliable held-button input."; return false;
    }
    if (!XFixesQueryExtension(display_, &fixesEvent_, &ignored)) {
        error = "The XFixes extension is required for safe cursor restoration."; return false;
    }
    major = 5; minor = 0;
    if (!XFixesQueryVersion(display_, &major, &minor) || major < 4) {
        error = "XFixes version 4 or newer is required."; return false;
    }
    if (!SelectInput()) {
        error = "Could not subscribe to XInput2 raw mouse events."; return false;
    }
    XIGetClientPointer(display_, None, &masterPointer_);
    XSelectInput(display_, root_, PropertyChangeMask | StructureNotifyMask | SubstructureNotifyMask);
    XFixesSelectCursorInput(display_, root_, XFixesDisplayCursorNotifyMask);
    auto atom = [this](const char* name) { return XInternAtom(display_, name, False); };
    activeAtom_ = atom("_NET_ACTIVE_WINDOW"); wmStateAtom_ = atom("_NET_WM_STATE"); fullscreenAtom_ = atom("_NET_WM_STATE_FULLSCREEN");
    typeAtom_ = atom("_NET_WM_WINDOW_TYPE"); desktopAtom_ = atom("_NET_WM_WINDOW_TYPE_DESKTOP"); dockAtom_ = atom("_NET_WM_WINDOW_TYPE_DOCK");
    pidAtom_ = atom("_NET_WM_PID"); compositorAtom_ = atom(("_NET_WM_CM_S" + std::to_string(screen_)).c_str());
    frameExtentsAtom_ = atom("_NET_FRAME_EXTENTS");
    maximizedHorizontalAtom_ = atom("_NET_WM_STATE_MAXIMIZED_HORZ"); maximizedVerticalAtom_ = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    XFixesSelectSelectionInput(display_, root_, compositorAtom_, XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask);
    compositor_ = XGetSelectionOwner(display_, compositorAtom_) != None;
    randr_ = XRRQueryExtension(display_, &randrEvent_, &ignored);
    if (randr_) XRRSelectInput(display_, root_, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
    XVisualInfo wanted{}; wanted.screen = screen_; wanted.depth = 32; wanted.c_class = TrueColor;
    int count = 0; auto* visuals = XGetVisualInfo(display_, VisualScreenMask | VisualDepthMask | VisualClassMask, &wanted, &count);
    for (int i = 0; i < count; ++i) {
        auto* format = XRenderFindVisualFormat(display_, visuals[i].visual);
        if (format && format->type == PictTypeDirect && format->direct.alphaMask) { visual_ = visuals[i].visual; break; }
    }
    if (visuals) XFree(visuals);
    if (visual_) colormap_ = XCreateColormap(display_, root_, visual_, AllocNone);
    else graphicsError_ = "This X server has no 32-bit ARGB visual; cursor and visual overlays are unavailable.";
    if (const char* dpi = XGetDefault(display_, "Xft", "dpi")) {
        char* end = nullptr; const double value = std::strtod(dpi, &end);
        if (end != dpi && std::isfinite(value)) scale_ = std::clamp(value / 96.0, 1.0, 3.0);
    }
    // Validate required subscriptions/resources before querying another
    // application's window. Its XID can disappear while GTK/WM windows are
    // being created; that ordinary BadWindow race is not a setup failure.
    XSync(display_, False);
    if (const int code = gEngineError.exchange(0, std::memory_order_acq_rel)) {
        char description[256]{}; XGetErrorText(display_, code, description, sizeof(description));
        error = std::string("The X server rejected backend initialization: ") + description;
        return false;
    }
    UpdateMonitors(); UpdatePointerDevices(); UpdateForeground(true); CaptureCursor();
    XSync(display_, False);
    CheckXError(); // Same bounded foreground-race handling as normal operation.
    std::string audioError;
    audio_.Start(settings_, audioError);
    ApplyPolicy(); ScheduleFrame(true);
    return true;
}
void X11Engine::UpdateMonitors() {
    shake_.Reset(); pointerMonitor_ = -1;
    monitors_.clear();
    if (randr_) if (auto* resources = XRRGetScreenResourcesCurrent(display_, root_)) {
        for (int i = 0; i < resources->ncrtc; ++i) if (auto* crtc = XRRGetCrtcInfo(display_, resources, resources->crtcs[i])) {
            if (crtc->width && crtc->height && crtc->mode) {
                unsigned rate = 60;
                for (int m = 0; m < resources->nmode; ++m) if (resources->modes[m].id == crtc->mode) {
                    const auto& mode = resources->modes[m];
                    if (mode.hTotal && mode.vTotal) {
                        double hz = double(mode.dotClock) / (double(mode.hTotal) * mode.vTotal);
                        if (mode.modeFlags & RR_Interlace) hz *= 2;
                        if (mode.modeFlags & RR_DoubleScan) hz /= 2;
                        if (std::isfinite(hz) && hz >= 30 && hz <= 1000) rate = static_cast<unsigned>(std::lround(hz));
                    }
                }
                monitors_.push_back({crtc->x, crtc->y, static_cast<int>(crtc->width), static_cast<int>(crtc->height), rate});
            }
            XRRFreeCrtcInfo(crtc);
        }
        XRRFreeScreenResources(resources);
    }
    if (monitors_.empty()) monitors_.push_back({0, 0, DisplayWidth(display_, screen_), DisplayHeight(display_, screen_), 60});
}
unsigned X11Engine::MonitorRate() const {
    for (const auto& monitor : monitors_) if (pointerX_ >= monitor.x && pointerY_ >= monitor.y &&
        pointerX_ < monitor.x + monitor.width && pointerY_ < monitor.y + monitor.height) return std::clamp(monitor.rate, 30u, 360u);
    return 60;
}
void X11Engine::UpdatePointer() {
    Window child, root; int x, y; unsigned buttons;
    if (XQueryPointer(display_, root_, &root, &child, &pointerX_, &pointerY_, &x, &y, &buttons)) pointerDirty_ = false;
    // A fresh client has no explicitly assigned ClientPointer until its first
    // pointer request. Resolve it after XQueryPointer chose the server default.
    if (!masterPointer_) XIGetClientPointer(display_, None, &masterPointer_);
    int monitorIndex = -1;
    for (std::size_t i = 0; i < monitors_.size(); ++i) {
        const auto& monitor = monitors_[i];
        if (pointerX_ >= monitor.x && pointerY_ >= monitor.y && pointerX_ < monitor.x + monitor.width && pointerY_ < monitor.y + monitor.height) {
            monitorIndex = static_cast<int>(i); break;
        }
    }
    if (monitorIndex != pointerMonitor_) { pointerMonitor_ = monitorIndex; shake_.Reset(); }
}
void X11Engine::UpdatePointerDevices() {
    shake_.Reset();
    RestorePointer(); pulse_ = false; held_ = 0; devices_.clear();
    masterPointer_ = 0; UpdatePointer();
    int count = 0, pointers = 0;
    if (auto* devices = XIQueryDevice(display_, XIAllMasterDevices, &count)) {
        for (int i = 0; i < count; ++i) if (devices[i].use == XIMasterPointer && devices[i].enabled) ++pointers;
        XIFreeDeviceInfo(devices);
    }
    // XFixes hiding is connection-owned but applies to every master sprite.
    // Never hide unrelated pointers to replace just this client's pointer.
    multiplePointers_ = pointers != 1;
}
void X11Engine::UpdateForeground(bool force) {
    const auto active = Property(root_, activeAtom_, XA_WINDOW, 1);
    Window window = active.empty() ? None : active.front();
    if (!window) { int revert; XGetInputFocus(display_, &window, &revert); if (window == PointerRoot) window = root_; }
    const bool changed = window != foreground_;
    if (changed || force) {
        if (foreground_ && foreground_ != root_) XSelectInput(display_, foreground_, NoEventMask);
        foreground_ = window;
        if (foreground_ && foreground_ != root_) XSelectInput(display_, foreground_, StructureNotifyMask | PropertyChangeMask | FocusChangeMask);
        foregroundName_.clear(); foregroundExcluded_ = foregroundAllowed_ = false;
        std::vector<std::string> names;
        const auto pids = Property(foreground_, pidAtom_, XA_CARDINAL, 1);
        if (!pids.empty() && pids.front() > 0 && pids.front() <= std::numeric_limits<int>::max()) {
            const auto prefix = "/proc/" + std::to_string(pids.front());
            std::array<char, 4096> path{};
            const ssize_t length = readlink((prefix + "/exe").c_str(), path.data(), path.size() - 1);
            if (length > 0) {
                std::string name(path.data(), static_cast<std::size_t>(length));
                name = name.substr(name.find_last_of('/') + 1);
                foregroundName_ = name; names.push_back(Lower(name));
            }
            std::ifstream command(prefix + "/comm"); std::string name;
            if (std::getline(command, name)) names.push_back(Lower(name));
        }
        XClassHint hint{};
        if (foreground_ && XGetClassHint(display_, foreground_, &hint)) {
            if (hint.res_name) { names.push_back(Lower(hint.res_name)); if (foregroundName_.empty()) foregroundName_ = hint.res_name; XFree(hint.res_name); }
            if (hint.res_class) { names.push_back(Lower(hint.res_class)); XFree(hint.res_class); }
        }
        for (const auto& name : names) {
            if (std::find(exclusions_.begin(), exclusions_.end(), name) != exclusions_.end()) foregroundExcluded_ = true;
            if (std::find(allowances_.begin(), allowances_.end(), name) != allowances_.end()) foregroundAllowed_ = true;
        }
    }
    foregroundFullscreen_ = false;
    const auto types = Property(foreground_, typeAtom_, XA_ATOM);
    foregroundShell_ = !foreground_ || foreground_ == root_ ||
        std::find(types.begin(), types.end(), desktopAtom_) != types.end() || std::find(types.begin(), types.end(), dockAtom_) != types.end();
    if (!foregroundShell_) {
        const auto states = Property(foreground_, wmStateAtom_, XA_ATOM);
        foregroundFullscreen_ = std::find(states.begin(), states.end(), fullscreenAtom_) != states.end();
        const bool maximized = std::find(states.begin(), states.end(), maximizedHorizontalAtom_) != states.end() &&
            std::find(states.begin(), states.end(), maximizedVerticalAtom_) != states.end();
        const auto extents = Property(foreground_, frameExtentsAtom_, XA_CARDINAL, 4);
        const bool decorated = std::any_of(extents.begin(), extents.end(), [](unsigned long extent) { return extent != 0; });
        XWindowAttributes attributes{};
        // An explicit fullscreen hint is authoritative. Geometry is only a
        // fallback for borderless windows, never ordinary maximized apps.
        if (!foregroundFullscreen_ && !maximized && !decorated &&
            XGetWindowAttributes(display_, foreground_, &attributes) && attributes.map_state == IsViewable) {
            int x = 0, y = 0; Window child;
            if (XTranslateCoordinates(display_, foreground_, root_, 0, 0, &x, &y, &child)) {
                for (const auto& monitor : monitors_) if (std::abs(x - monitor.x) <= 2 && std::abs(y - monitor.y) <= 2 &&
                    std::abs(attributes.width - monitor.width) <= 2 && std::abs(attributes.height - monitor.height) <= 2) foregroundFullscreen_ = true;
            }
        }
    }
    lastRuleCheck_ = Clock::now();
    ApplyPolicy();
}
void X11Engine::CaptureCursor() {
    auto* image = XFixesGetCursorImage(display_);
    if (!image) return;
    if (image->cursor_serial == cursorSerial_ && darkImage_) { XFree(image); return; }
    cursorSerial_ = image->cursor_serial;
    bool visible = false;
    const bool valid = image->width && image->height && image->width <= 512 && image->height <= 512 &&
        image->xhot < image->width && image->yhot < image->height;
    cairo_surface_t* dark = nullptr;
    cairo_surface_t* normal = nullptr;
    if (valid) {
        dark = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, image->width, image->height);
        normal = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, image->width, image->height);
        if (cairo_surface_status(dark) == CAIRO_STATUS_SUCCESS && cairo_surface_status(normal) == CAIRO_STATUS_SUCCESS) {
            auto* pixels = cairo_image_surface_get_data(dark);
            const int stride = cairo_image_surface_get_stride(dark);
            for (unsigned y = 0; y < image->height; ++y) {
                auto* row = reinterpret_cast<std::uint32_t*>(pixels + y * stride);
                auto* original = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(normal) + y * cairo_image_surface_get_stride(normal));
                for (unsigned x = 0; x < image->width; ++x) {
                    const std::uint32_t p = image->pixels[std::size_t(y) * image->width + x];
                    original[x] = p;
                    const unsigned a = p >> 24;
                    visible = visible || a != 0;
                    // XFixes pixels are premultiplied: invert each color within
                    // alpha, preserving antialiasing and completely clear pixels.
                    row[x] = (a << 24) | ((a - std::min(a, (p >> 16) & 255u)) << 16) |
                        ((a - std::min(a, (p >> 8) & 255u)) << 8) | (a - std::min(a, p & 255u));
                }
            }
            cairo_surface_mark_dirty(dark); cairo_surface_mark_dirty(normal);
        } else { cairo_surface_destroy(dark); cairo_surface_destroy(normal); dark = normal = nullptr; }
    }
    ClearShakeCache();
    if (darkImage_) cairo_surface_destroy(darkImage_);
    if (normalImage_) cairo_surface_destroy(normalImage_);
    normalImage_ = normal; darkImage_ = dark; cursorHotX_ = image->xhot; cursorHotY_ = image->yhot;
    cursorVisible_ = visible || !valid;
    cursorDirty_ = true;
    XFree(image);
    RebuildShakeCache();
    if (!darkImage_) RestorePointer();
    ApplyPolicy();
    if (pressed_ || shake_.Active(Seconds(Clock::now()))) ScheduleFrame(true);
}
void X11Engine::ClearShakeCache() noexcept {
    for (auto& frame : shakeFrames_) {
        if (frame.normal) cairo_surface_destroy(frame.normal);
        if (frame.dark) cairo_surface_destroy(frame.dark);
        frame = {};
    }
    shakeFrameCount_ = 0; paintedCursor_ = nullptr;
}
void X11Engine::RebuildShakeCache() {
    ClearShakeCache(); shakeError_.clear();
    if (!settings_.shakeStyle || !normalImage_ || !darkImage_) return;
    const int width = cairo_image_surface_get_width(normalImage_), height = cairo_image_surface_get_height(normalImage_);
    const double maximum = settings_.shakeMaximumScale / 100.0;
    const auto required = [&](unsigned count) {
        std::uint64_t bytes = 0;
        for (unsigned i = 0; i < count; ++i) {
            const double scale = 1 + (maximum - 1) * i / (count - 1);
            const int w = static_cast<int>(std::lround(width * scale)), h = static_cast<int>(std::lround(height * scale));
            if (w > 2048 || h > 2048) return std::numeric_limits<std::uint64_t>::max();
            bytes += std::uint64_t(w) * h * 8; // Normal and inverted, premultiplied ARGB.
        }
        return bytes;
    };
    unsigned count = kShakeFrames;
    while (count > 6 && required(count) > kShakeCacheBytes) --count;
    if (required(count) > kShakeCacheBytes) {
        shake_.Reset();
        shakeError_ = "Shake enlargement is unavailable for this exceptionally large cursor; the normal pointer remains visible.";
        return;
    }
    // Shape changes and configuration rebuild this bounded image cache. Input
    // and animation only select existing pixels; no per-frame resampling.
    for (unsigned i = 0; i < count; ++i) {
        const double scale = 1 + (maximum - 1) * i / (count - 1);
        auto& frame = shakeFrames_[i];
        const int w = std::max(1, static_cast<int>(std::lround(width * scale)));
        const int h = std::max(1, static_cast<int>(std::lround(height * scale)));
        const auto resize = [&](cairo_surface_t* source) {
            auto* target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            if (cairo_surface_status(target) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(target); return static_cast<cairo_surface_t*>(nullptr); }
            auto* cr = cairo_create(target);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_scale(cr, double(w) / width, double(h) / height);
            cairo_set_source_surface(cr, source, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            const bool good = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
            cairo_destroy(cr); cairo_surface_flush(target);
            if (!good) { cairo_surface_destroy(target); return static_cast<cairo_surface_t*>(nullptr); }
            return target;
        };
        frame.normal = resize(normalImage_);
        if (frame.normal) {
            frame.dark = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            if (cairo_surface_status(frame.dark) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(frame.dark); frame.dark = nullptr; }
        }
        if (!frame.normal || !frame.dark) {
            ClearShakeCache(); shake_.Reset();
            shakeError_ = "Could not cache the enlarged pointer images. Other effects remain available.";
            return;
        }
        // Invert the already resampled premultiplied image so every cached
        // pair has identical alpha and exact complementary color channels.
        for (int y = 0; y < h; ++y) {
            const auto* source = reinterpret_cast<const std::uint32_t*>(cairo_image_surface_get_data(frame.normal) + y * cairo_image_surface_get_stride(frame.normal));
            auto* target = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(frame.dark) + y * cairo_image_surface_get_stride(frame.dark));
            for (int x = 0; x < w; ++x) {
                const auto pixel = source[x]; const unsigned alpha = pixel >> 24;
                target[x] = (alpha << 24) | ((alpha - std::min(alpha, (pixel >> 16) & 255u)) << 16) |
                    ((alpha - std::min(alpha, (pixel >> 8) & 255u)) << 8) | (alpha - std::min(alpha, pixel & 255u));
            }
        }
        cairo_surface_mark_dirty(frame.dark);
        frame.hotX = std::clamp(static_cast<int>(std::lround(cursorHotX_ * double(w) / width)), 0, w - 1);
        frame.hotY = std::clamp(static_cast<int>(std::lround(cursorHotY_ * double(h) / height)), 0, h - 1);
    }
    shakeFrameCount_ = count;
}
void X11Engine::ObserveShake() {
    if (!shakeMotionPending_) return;
    shakeMotionPending_ = false;
    if (!settings_.shakeStyle || !allowed_ || multiplePointers_ || !compositor_ || !shakeFrameCount_) { shake_.ClearTrace(); return; }
    UpdatePointer();
    const bool trigger = shake_.Observe(pointerX_ / scale_, pointerY_ / scale_, Seconds(Clock::now()), held_ == 0);
    if (trigger || shake_.Active(Seconds(Clock::now()))) ScheduleFrame(trigger);
}
void X11Engine::UpdateCursorEffect(double now) {
    const bool possible = allowed_ && compositor_ && visual_ && !graphicsFailed_ && !multiplePointers_ && darkImage_;
    const bool wantPressed = possible && settings_.darkCursorEnabled &&
        (settings_.cursorHold ? ((held_ & settings_.buttonMask) != 0) : pulse_);
    const bool enlarged = possible && shakeFrameCount_ && shake_.Active(now);
    if (!wantPressed && !enlarged) { RestorePointer(); return; }
    if (pressed_ != wantPressed) cursorDirty_ = true;
    pressed_ = wantPressed;
    PaintCursor(now);
    if (cursor_.mapped && !graphicsFailed_ && !ownHide_) {
        XFixesHideCursor(display_, root_); ownHide_ = true; XFlush(display_);
    }
}
void X11Engine::RestorePointer() noexcept {
    // Restore the real pointer before unmapping the replacement. XFixes also
    // automatically deletes this connection's hide request on process death.
    if (display_ && ownHide_) { XFixesShowCursor(display_, root_); XFlush(display_); ownHide_ = false; }
    pressed_ = false;
    Hide(cursor_);
}
void X11Engine::ClearEffects(bool holdSpotlightAndLens) noexcept {
    shake_.Reset(); shakeMotionPending_ = false;
    RestorePointer(); Hide(highlight_);
    if (!holdSpotlightAndLens) { HideSpotlight(); HideLens(); }
    for (auto& particle : particles_) { particle.active = false; Hide(particle.overlay); }
    for (auto& trail : trails_) { trail.active = false; Hide(trail.overlay); }
    trailMotionPending_ = trailAnchorSet_ = stackDirty_ = stackOrderDirty_ = foreignStackDirty_ = false;
    pulse_ = false; frameScheduled_ = false;
}
bool X11Engine::FullscreenPaused() const noexcept {
    return settings_.pauseFullscreen && foregroundFullscreen_ && !foregroundAllowed_;
}
void X11Engine::ApplyPolicy() {
    const bool otherRule = locked_.load(std::memory_order_acquire) || foregroundExcluded_ || FullscreenPaused();
    const bool rule = otherRule || !cursorVisible_;
    const bool permitted = settings_.enabled && !rule;
    if (!KeysAllowed()) HideKeys();
    typingHold_ = !permitted && settings_.enabled && !otherRule;
    if (!permitted) {
        ClearEffects(typingHold_);
        audio_.SetAllowed(false); allowed_ = false;
        if (locked_.load(std::memory_order_relaxed)) { held_ = 0; devices_.clear(); chordModifiers_ = 0; modifierChordUsed_ = false; }
        if (keyVisible_) ScheduleKeyFrame();
        Publish(); return;
    }
    const bool wasAllowed = allowed_;
    allowed_ = true; audio_.SetAllowed(true);
    if (pointerDirty_) UpdatePointer();
    UpdateCursorEffect(Seconds(Clock::now()));
    if (!wasAllowed && (settings_.highlightEnabled || settings_.magnifierEnabled)) { pointerDirty_ = true; ScheduleFrame(true); }
    Publish();
}
bool X11Engine::KeysAllowed() const noexcept {
    // The key display ignores the transparent-cursor rule: editors hide the
    // pointer while you type, which is exactly when the keys should show.
    return settings_.enabled && settings_.keyDisplayEnabled && !locked_.load(std::memory_order_relaxed) &&
        !foregroundExcluded_ && !FullscreenPaused() && compositor_ && visual_ && !graphicsFailed_;
}
void X11Engine::ScheduleKeyFrame() {
    if (!compositor_ || !visual_ || graphicsFailed_) return;
    const auto now = Clock::now();
    if (!frameScheduled_ || now < nextFrame_) nextFrame_ = now;
    frameScheduled_ = true;
}
unsigned X11Engine::CurrentModifiers() {
    XkbStateRec state{};
    if (XkbGetState(display_, XkbUseCoreKbd, &state) != Success) return 0;
    unsigned result = 0;
    if (state.mods & ControlMask) result |= kModControl;
    if (state.mods & ShiftMask) result |= kModShift;
    if (state.mods & Mod1Mask) result |= kModAlt;
    if (state.mods & Mod4Mask) result |= kModWin;
    if (state.mods & Mod5Mask) result |= kModAltGr;
    return result;
}
void X11Engine::ShowKey(KeySym symbol, unsigned modifiers, const std::string& typed) {
    if (!KeysAllowed() || !KeyDisplayWanted(settings_.keyDisplayMode, symbol, modifiers)) return;
    const auto now = Clock::now();
    const auto hold = std::chrono::milliseconds(settings_.keyDisplayDurationMs);
    if (!keyVisible_) keyStrokes_.clear();
    KeyStroke* last = keyStrokes_.empty() ? nullptr : &keyStrokes_.back();
    // A typed row is still being written while it has not started to fade.
    const bool typing = last && !last->typed.empty() && now - last->updated < hold;
    const bool plain = !(modifiers & (kModControl | kModAlt | kModWin | kModMouse));
    if (typing && !typed.empty()) {
        last->typed += typed;
        while (last->typed.size() > kTypedBytes) DropFirstCharacter(last->typed);
        last->updated = now;
    } else if (typing && plain && symbol == XK_BackSpace) {
        // Backspace edits the typed row, so it reads like the text itself.
        PopCharacter(last->typed);
        if (last->typed.empty()) keyStrokes_.pop_back(); else last->updated = now;
    } else if (!typed.empty() && typed != " ") {
        keyStrokes_.push_back({{}, typed, 1, now});
    } else {
        auto caps = ChordParts(symbol, modifiers);
        if (last && last->typed.empty() && last->caps == caps && now - last->updated < std::chrono::seconds(1)) {
            ++last->repeat;
            last->updated = now;
        } else {
            keyStrokes_.push_back({std::move(caps), {}, 1, now});
        }
    }
    while (keyStrokes_.size() > kKeyRowLimit) keyStrokes_.erase(keyStrokes_.begin());
    keyVisible_ = !keyStrokes_.empty(); keyDirty_ = true;
    if (!keyVisible_) HideKeys();
    ScheduleKeyFrame();
}
// The character a key types with the current Shift, Caps Lock and AltGr
// state, or nothing for shortcuts and keys that type nothing.
std::string X11Engine::TypedText(KeyCode code, unsigned modifiers) {
    if (modifiers & (kModControl | kModAlt | kModWin | kModMouse)) return {};
    XkbStateRec state{};
    if (XkbGetState(display_, XkbUseCoreKbd, &state) != Success) return {};
    const unsigned mods = state.mods & (ShiftMask | LockMask | Mod2Mask | Mod3Mask | Mod5Mask);
    unsigned consumed = 0;
    KeySym symbol = NoSymbol;
    if (!XkbLookupKeySym(display_, code, XkbBuildCoreState(mods, state.group), &consumed, &symbol)) return {};
    if (symbol >= 0xfe50 && symbol <= 0xfe8f) return {}; // Dead keys combine with the next key.
    return KeysymText(symbol);
}
void X11Engine::HideKeys() noexcept {
    Hide(keyPanel_);
    keyStrokes_.clear(); keyVisible_ = keyDirty_ = false; keyDeadline_ = {};
}
// The key display in the style of the Mac keystroke visualizers: each
// shortcut is a row of keycaps on a rounded translucent panel with a soft
// shadow; typed characters join into one row. Rows stack towards the screen
// edge and fade out one by one.
void X11Engine::PaintKeyDisplay(TimePoint now) {
    if (!keyVisible_) return;
    if (!KeysAllowed()) { HideKeys(); return; }
    const auto hold = std::chrono::milliseconds(settings_.keyDisplayDurationMs);
    keyStrokes_.erase(std::remove_if(keyStrokes_.begin(), keyStrokes_.end(),
        [&](const KeyStroke& stroke) { return now - stroke.updated >= hold + kKeyFade; }), keyStrokes_.end());
    if (keyStrokes_.empty()) { HideKeys(); return; }
    std::vector<double> fades;
    bool fading = false;
    TimePoint next{};
    for (const auto& stroke : keyStrokes_) {
        const auto age = now - stroke.updated;
        double fade = 1.0;
        TimePoint wake = stroke.updated + hold;
        if (age >= hold) {
            const double remaining = 1.0 - std::chrono::duration<double>(age - hold).count() / std::chrono::duration<double>(kKeyFade).count();
            fade = std::clamp(remaining * remaining, 0.0, 1.0);
            fading = true;
            wake = now + std::chrono::milliseconds(16);
        }
        fades.push_back(fade);
        if (next == TimePoint{} || wake < next) next = wake;
    }
    keyDeadline_ = next;
    if (!keyDirty_ && !fading) return;
    const Monitor* monitor = &monitors_.front();
    for (const auto& candidate : monitors_)
        if (pointerX_ >= candidate.x && pointerY_ >= candidate.y && pointerX_ < candidate.x + candidate.width && pointerY_ < candidate.y + candidate.height) { monitor = &candidate; break; }
    const double s = scale_ * settings_.keyDisplaySize / 100.0;
    const auto px = [&](double value, double minimum) { return std::max(minimum, std::round(value * s)); };
    const double capHeight = px(44, 16), capPadding = px(13, 4), capGap = px(7, 2), padX = px(11, 4), padY = px(11, 3);
    const double shadow = px(14, 3), shadowOffset = px(3, 1), rowGap = px(8, 2), depth = px(2, 1);
    const double corner = std::max(4.0, 16 * s), capCorner = std::max(3.0, 8 * s);
    const double capFont = px(22, 8), typeFont = px(28, 10), countFont = px(19, 7);
    const double margin = shadow + shadowOffset, bubbleHeight = capHeight + 2 * padY;
    cairo_surface_t* scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* measure = cairo_create(scratch);
    cairo_select_font_face(measure, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const auto advance = [&](const std::string& text, double size) {
        cairo_set_font_size(measure, size);
        cairo_text_extents_t extents{}; cairo_text_extents(measure, text.c_str(), &extents);
        return std::ceil(extents.x_advance);
    };
    struct RowLayout { std::vector<double> caps; std::string typed, count; double contentWidth = 0, countWidth = 0, width = 0; };
    std::vector<RowLayout> rows;
    const double maximumBubble = std::max(capHeight, monitor->width * 0.9 - 2 * margin);
    for (const auto& stroke : keyStrokes_) {
        RowLayout row;
        if (stroke.repeat > 1) { row.count = "\xc3\x97" + std::to_string(stroke.repeat); row.countWidth = advance(row.count, countFont) + 2 * capGap; }
        if (!stroke.caps.empty()) {
            for (const auto& cap : stroke.caps) {
                row.caps.push_back(std::max(capHeight, advance(cap, capFont) + 2 * capPadding));
                row.contentWidth += row.caps.back() + (row.caps.size() > 1 ? capGap : 0);
            }
        } else {
            // Keep the newest characters; an ellipsis marks the cut.
            const double available = std::max(capHeight, maximumBubble - 2 * padX - capPadding - row.countWidth);
            row.typed = stroke.typed;
            if (advance(row.typed, typeFont) > available) {
                while (row.typed.size() > 1 && advance("\xe2\x80\xa6" + row.typed, typeFont) > available) DropFirstCharacter(row.typed);
                row.typed = "\xe2\x80\xa6" + row.typed;
            }
            row.contentWidth = advance(row.typed, typeFont) + capPadding;
        }
        row.width = row.contentWidth + 2 * padX + row.countWidth;
        rows.push_back(std::move(row));
    }
    cairo_destroy(measure); cairo_surface_destroy(scratch);
    double widest = 0;
    for (const auto& row : rows) widest = std::max(widest, row.width);
    const int count = static_cast<int>(rows.size());
    const int width = static_cast<int>(std::ceil(widest + 2 * margin));
    const int height = static_cast<int>(std::ceil(2 * margin + count * bubbleHeight + (count - 1) * rowGap));
    if (!EnsureOverlay(keyPanel_, width, height, "key display", 16384)) return;
    const auto packed = settings_.keyDisplayColor;
    const unsigned red = packed & 255, green = (packed >> 8) & 255, blue = (packed >> 16) & 255;
    const bool dark = red * 299 + green * 587 + blue * 114 < 140000;
    const double ink = dark ? 0.965 : 0.094;
    cairo_t* cr = cairo_create(keyPanel_.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, 0, 0, 0, 0); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    const bool top = settings_.keyDisplayPosition == 1;
    for (int n = 0; n < count; ++n) {
        const auto& row = rows[static_cast<std::size_t>(n)];
        const auto& stroke = keyStrokes_[static_cast<std::size_t>(n)];
        // The newest row sits nearest the screen edge.
        const int slot = top ? count - 1 - n : n;
        const double x = settings_.keyDisplayPosition == 2 ? margin : settings_.keyDisplayPosition == 3 ? width - margin - row.width : std::round((width - row.width) / 2);
        const double y = margin + slot * (bubbleHeight + rowGap);
        cairo_push_group(cr);
        // Soft shadow: a few widening, fading layers below the panel.
        for (int layer = 5; layer >= 1; --layer) {
            const double spread = shadow * layer / 5.0;
            RoundedRect(cr, x - spread * 0.5, y + shadowOffset - spread * 0.25, row.width + spread, bubbleHeight + spread * 0.75, corner + spread * 0.5);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.075 * (1.0 - (layer - 1) / 5.0));
            cairo_fill(cr);
        }
        RoundedRect(cr, x, y, row.width, bubbleHeight, corner);
        Color(cr, packed, settings_.keyDisplayOpacity / 100.0);
        cairo_fill_preserve(cr);
        // Hairline rim, like a macOS HUD.
        cairo_set_line_width(cr, 1.0);
        if (dark) cairo_set_source_rgba(cr, 1, 1, 1, 0.14); else cairo_set_source_rgba(cr, 0, 0, 0, 0.10);
        cairo_stroke(cr);
        const double capTop = y + padY;
        if (!stroke.caps.empty()) {
            double cx = x + padX;
            cairo_set_font_size(cr, capFont);
            cairo_font_extents_t font{}; cairo_font_extents(cr, &font);
            for (std::size_t i = 0; i < stroke.caps.size(); ++i) {
                const double w = row.caps[i];
                // Key base (its lower edge shows as depth), then the face.
                RoundedRect(cr, cx, capTop, w, capHeight, capCorner);
                if (dark) cairo_set_source_rgba(cr, 1, 1, 1, 0.07); else cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
                cairo_fill(cr);
                RoundedRect(cr, cx, capTop, w, capHeight - depth, capCorner);
                if (dark) cairo_set_source_rgba(cr, 1, 1, 1, 0.13); else cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
                cairo_fill(cr);
                cairo_text_extents_t extents{}; cairo_text_extents(cr, stroke.caps[i].c_str(), &extents);
                cairo_set_source_rgba(cr, ink, ink, ink + 0.02, 1);
                cairo_move_to(cr, std::round(cx + (w - extents.x_advance) / 2),
                              std::round(capTop + (capHeight - depth) / 2 + (font.ascent - font.descent) / 2));
                cairo_show_text(cr, stroke.caps[i].c_str());
                cx += w + capGap;
            }
        } else {
            cairo_set_font_size(cr, typeFont);
            cairo_font_extents_t font{}; cairo_font_extents(cr, &font);
            cairo_set_source_rgba(cr, ink, ink, ink + 0.02, 1);
            cairo_move_to(cr, std::round(x + padX + capPadding / 2), std::round(capTop + capHeight / 2 + (font.ascent - font.descent) / 2));
            cairo_show_text(cr, row.typed.c_str());
        }
        if (!row.count.empty()) {
            cairo_set_font_size(cr, countFont);
            cairo_font_extents_t font{}; cairo_font_extents(cr, &font);
            cairo_set_source_rgba(cr, ink, ink, ink + 0.02, 0.62);
            cairo_move_to(cr, std::round(x + row.width - padX - row.countWidth + capGap), std::round(capTop + capHeight / 2 + (font.ascent - font.descent) / 2));
            cairo_show_text(cr, row.count.c_str());
        }
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, fades[static_cast<std::size_t>(n)]);
    }
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(keyPanel_.surface);
    if (!ok) { GraphicsError("Could not draw the key display."); return; }
    const int edge = static_cast<int>(48 * scale_), inset = static_cast<int>(margin);
    int left = monitor->x + (monitor->width - width) / 2, topEdge = monitor->y + monitor->height - edge - height + inset;
    switch (settings_.keyDisplayPosition) {
    case 1: topEdge = monitor->y + edge - inset; break;
    case 2: left = monitor->x + edge - inset; break;
    case 3: left = monitor->x + monitor->width - edge - width + inset; break;
    default: break;
    }
    Place(keyPanel_, left, topEdge);
    keyDirty_ = false;
}
void X11Engine::HideLens() noexcept {
    Hide(lens_);
    lensVisible_ = false;
}
void X11Engine::PaintLens(TimePoint now) {
    if (!settings_.magnifierEnabled || !(allowed_ || typingHold_) || !compositor_ || !visual_ || graphicsFailed_) { HideLens(); return; }
    const Monitor* monitor = &monitors_.front();
    for (const auto& candidate : monitors_)
        if (pointerX_ >= candidate.x && pointerY_ >= candidate.y && pointerX_ < candidate.x + candidate.width && pointerY_ < candidate.y + candidate.height) { monitor = &candidate; break; }
    const int size = std::clamp(static_cast<int>(std::lround(settings_.magnifierSize * scale_)), 40, 2400);
    const double zoom = settings_.magnifierZoom / 100.0;
    const int border = std::max(2, static_cast<int>(std::lround(3 * scale_)));
    const int sourceSize = std::max(8, static_cast<int>(std::lround((size - border) / zoom)));
    const int rootWidth = DisplayWidth(display_, screen_), rootHeight = DisplayHeight(display_, screen_);
    // Source square around the pointer, kept inside the monitor and the screen.
    const int sx = std::clamp(pointerX_ - sourceSize / 2, std::max(0, monitor->x), std::max(0, std::min(rootWidth, monitor->x + monitor->width) - sourceSize));
    const int sy = std::clamp(pointerY_ - sourceSize / 2, std::max(0, monitor->y), std::max(0, std::min(rootHeight, monitor->y + monitor->height) - sourceSize));
    if (sx + sourceSize > rootWidth || sy + sourceSize > rootHeight) { HideLens(); return; }
    // The lens sits beside the pointer and must never cover its own source:
    // the screen read below includes every window, this one too.
    int offset = size * 3 / 4, cx = 0, cy = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt, offset += size / 2) {
        cx = pointerX_ + offset; cy = pointerY_ - offset;
        if (cx + size / 2 > monitor->x + monitor->width) cx = pointerX_ - offset;
        if (cy - size / 2 < monitor->y) cy = pointerY_ + offset;
        const int lx = cx - size / 2, ly = cy - size / 2;
        if (lx >= sx + sourceSize || lx + size <= sx || ly >= sy + sourceSize || ly + size <= sy) break;
    }
    if (!EnsureOverlay(lens_, size, size, "magnifier", 2400)) return;
    XImage* image = XGetImage(display_, root_, sx, sy, sourceSize, sourceSize, AllPlanes, ZPixmap);
    if (!image) { lensError_ = "The X server did not provide screen pixels for the magnifier."; HideLens(); return; }
    if (image->bits_per_pixel != 32 || image->byte_order != LSBFirst) {
        lensError_ = "The magnifier needs a 24- or 32-bit little-endian X display.";
        XDestroyImage(image); HideLens(); return;
    }
    lensError_.clear();
    cairo_surface_t* capture = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(image->data), CAIRO_FORMAT_RGB24,
        sourceSize, sourceSize, image->bytes_per_line);
    cairo_t* cr = cairo_create(lens_.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, 0, 0, 0, 0); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const double inset = border / 2.0, half = size / 2.0, inner = half - inset;
    const auto shape = [&] {
        cairo_new_path(cr);
        if (settings_.magnifierShape == 0) cairo_arc(cr, half, half, inner, 0, 2 * kPi);
        else {
            const double c = std::max(4.0, size / 10.0), l = inset, t = inset, r = size - inset, b = size - inset;
            cairo_new_sub_path(cr);
            cairo_arc(cr, r - c, t + c, c, -kPi / 2, 0); cairo_arc(cr, r - c, b - c, c, 0, kPi / 2);
            cairo_arc(cr, l + c, b - c, c, kPi / 2, kPi); cairo_arc(cr, l + c, t + c, c, kPi, 3 * kPi / 2);
            cairo_close_path(cr);
        }
    };
    cairo_save(cr);
    shape(); cairo_clip(cr);
    cairo_translate(cr, inset, inset); cairo_scale(cr, zoom, zoom);
    cairo_set_source_surface(cr, capture, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    // The real pointer is not part of the screen image; draw its last captured
    // artwork where it belongs so the lens shows what the pointer points at.
    if (cairo_surface_t* pointer = pressed_ ? darkImage_ : normalImage_) {
        cairo_set_source_surface(cr, pointer, pointerX_ - sx - cursorHotX_, pointerY_ - sy - cursorHotY_);
        cairo_paint(cr);
    }
    cairo_restore(cr);
    if (settings_.magnifierFrame) {
        shape();
        Color(cr, settings_.magnifierBorderColor, 1.0);
        cairo_set_line_width(cr, border);
        cairo_stroke(cr);
    }
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr); cairo_surface_destroy(capture); XDestroyImage(image); cairo_surface_flush(lens_.surface);
    if (!ok) { GraphicsError("Could not draw the magnifier."); return; }
    Place(lens_, cx - size / 2, cy - size / 2);
    lensVisible_ = true;
    nextLensFrame_ = now + kLensInterval;
}
void X11Engine::RawButton(int device, unsigned button, bool down) {
    const unsigned bit = ButtonBit(button);
    if (!bit) return;
    if (down) UpdateForeground();
    auto found = devices_.find(device);
    const unsigned before = found == devices_.end() ? 0 : found->second;
    if (down) devices_[device] = before | bit;
    else if (found != devices_.end()) { found->second &= ~bit; if (!found->second) devices_.erase(found); }
    held_ = 0; for (const auto& pair : devices_) held_ |= pair.second;
    if (held_) { shake_.ClearTrace(); nextSafety_ = Clock::now() + kSafetyInterval; }
    if (down && !(before & bit) && allowed_) {
        if (!settings_.cursorHold && settings_.darkCursorEnabled && (bit & settings_.buttonMask)) {
            pulse_ = true; pulseDeadline_ = Clock::now() + std::chrono::milliseconds(settings_.cursorFlashMs);
        }
        audio_.Click(bit);
        const bool wanted = (bit == 1 && settings_.leftClickEnabled) || (bit == 2 && settings_.rightClickEnabled) ||
            (bit == 4 && settings_.middleClickEnabled);
        if (settings_.clickEffectsEnabled && wanted) AddParticle(bit);
    }
    if (down && !(before & bit) && settings_.keyDisplayEnabled && settings_.keyDisplayMouse) {
        const unsigned modifiers = CurrentModifiers();
        if (modifiers) modifierChordUsed_ = true;
        ShowKey(static_cast<KeySym>(bit), modifiers | kModMouse, {});
    }
    ApplyPolicy();
}
bool X11Engine::SelectInput() {
    std::array<unsigned char, XIMaskLen(XI_LASTEVENT)> bits{};
    XISetMask(bits.data(), XI_RawMotion); XISetMask(bits.data(), XI_RawButtonPress); XISetMask(bits.data(), XI_RawButtonRelease);
    // Key presses are only subscribed while keyboard sounds or the key display
    // are switched on. Sounds reduce each press to a coarse category; the key
    // display turns it into a label that exists only while it is on screen.
    const bool keys = settings_.keyboardSoundsEnabled || settings_.keyDisplayEnabled;
    if (keys) XISetMask(bits.data(), XI_RawKeyPress);
    if (settings_.keyDisplayEnabled) XISetMask(bits.data(), XI_RawKeyRelease);
    std::array<unsigned char, XIMaskLen(XI_LASTEVENT)> hierarchy{};
    XISetMask(hierarchy.data(), XI_HierarchyChanged);
    std::array<XIEventMask, 2> masks{{
        {XIAllMasterDevices, static_cast<int>(bits.size()), bits.data()},
        {XIAllDevices, static_cast<int>(hierarchy.size()), hierarchy.data()}}};
    keyboardSelected_ = keys;
    keyboardReleases_ = settings_.keyDisplayEnabled;
    return XISelectEvents(display_, root_, masks.data(), masks.size()) == Success;
}
void X11Engine::RawKey(const XIRawEvent* raw, bool pressed) {
    const bool repeat = (raw->flags & XIKeyRepeat) != 0;
    const KeySym symbol = XkbKeycodeToKeysym(display_, static_cast<KeyCode>(std::clamp(raw->detail, 0, 255)), 0, 0);
    if (pressed && !repeat && allowed_ && settings_.keyboardSoundsEnabled) {
        unsigned category = kKeyGeneric;
        if (symbol == XK_space) category = kKeySpace;
        else if (symbol == XK_Return || symbol == XK_KP_Enter) category = kKeyEnter;
        else if (symbol == XK_BackSpace) category = kKeyBackspace;
        audio_.Key(category);
    }
    if (!settings_.keyDisplayEnabled) return;
    const bool modifier = IsModifierKeysym(symbol);
    if (!pressed) {
        // A modifier tapped on its own is itself the label (Super opens the
        // overview); it shows once every modifier is up again.
        if (modifier && !CurrentModifiers()) {
            if (!modifierChordUsed_ && chordModifiers_) {
                if (chordModifiers_ & kModAltGr) ShowKey(XK_ISO_Level3_Shift, kModAltGr);
                else ShowKey(symbol, chordModifiers_ & ~ModifierBitOf(symbol));
            }
            modifierChordUsed_ = false; chordModifiers_ = 0;
        }
        return;
    }
    if (repeat) return;
    if (modifier) { chordModifiers_ |= ModifierBitOf(symbol); return; }
    const unsigned modifiers = CurrentModifiers();
    if (modifiers) modifierChordUsed_ = true;
    ShowKey(symbol, modifiers, TypedText(static_cast<KeyCode>(std::clamp(raw->detail, 0, 255)), modifiers));
}
void X11Engine::ReconcileButtons() {
    Window root, child; double rx, ry, wx, wy; XIButtonState buttons{}; XIModifierState modifiers{}; XIGroupState group{};
    unsigned current = 0;
    const bool ok = masterPointer_ && XIQueryPointer(display_, masterPointer_, root_, &root, &child, &rx, &ry, &wx, &wy, &buttons, &modifiers, &group);
    if (ok) for (unsigned button : {1u, 2u, 3u, 8u, 9u})
        if (buttons.mask && button < static_cast<unsigned>(buttons.mask_len * 8) && XIMaskIsSet(buttons.mask, button)) current |= ButtonBit(button);
    if (buttons.mask) XFree(buttons.mask);
    if (!ok) current = 0; // A failed reconciliation must not leave a dark pointer stuck.
    const unsigned previous = held_;
    for (auto it = devices_.begin(); it != devices_.end();) {
        it->second &= current;
        if (!it->second) it = devices_.erase(it); else ++it;
    }
    held_ = 0; for (const auto& pair : devices_) held_ |= pair.second;
    if (held_ != previous) ApplyPolicy();
    nextSafety_ = Clock::now() + kSafetyInterval;
}
void X11Engine::ScheduleFrame(bool immediate) {
    if (!allowed_ || !compositor_ || !visual_ || graphicsFailed_) return;
    const auto now = Clock::now();
    // Motion can move a cached halo/cursor sooner than the next expensive
    // animation pass. Auto adaptation must not deliberately slow that feedback.
    const auto requested = immediate ? now : std::max(nextPointerFrame_, now);
    if (!frameScheduled_ || requested < nextFrame_) nextFrame_ = requested;
    frameScheduled_ = true;
}
bool X11Engine::EnsureOverlay(Overlay& overlay, int width, int height, const char* role, int limit) {
    if (!visual_ || !compositor_ || graphicsFailed_ || width < 1 || height < 1 || width > limit || height > limit) return false;
    if (!overlay.window) {
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True; attributes.background_pixel = 0; attributes.border_pixel = 0;
        attributes.colormap = colormap_; attributes.event_mask = ExposureMask;
        overlay.window = XCreateWindow(display_, root_, 0, 0, width, height, 0, 32, InputOutput, visual_,
            CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attributes);
        if (!overlay.window) { GraphicsError("Could not create a transparent overlay window."); return false; }
        XStoreName(display_, overlay.window, (std::string("ClickShade ") + role).c_str());
        XClassHint classHint{const_cast<char*>("clickshade-overlay"), const_cast<char*>("ClickShadeOverlay")};
        XSetClassHint(display_, overlay.window, &classHint);
        XWMHints hints{}; hints.flags = InputHint; hints.input = False; XSetWMHints(display_, overlay.window, &hints);
        const auto empty = XFixesCreateRegion(display_, nullptr, 0);
        XFixesSetWindowShapeRegion(display_, overlay.window, ShapeInput, 0, 0, empty); XFixesDestroyRegion(display_, empty);
        const std::array<Atom, 3> states{{XInternAtom(display_, "_NET_WM_STATE_ABOVE", False), XInternAtom(display_, "_NET_WM_STATE_SKIP_TASKBAR", False), XInternAtom(display_, "_NET_WM_STATE_SKIP_PAGER", False)}};
        XChangeProperty(display_, overlay.window, wmStateAtom_, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(states.data()), states.size());
        overlay.surface = cairo_xlib_surface_create(display_, overlay.window, visual_, width, height);
        overlay.width = width; overlay.height = height;
    } else if (overlay.width != width || overlay.height != height) {
        XResizeWindow(display_, overlay.window, width, height);
        cairo_xlib_surface_set_size(overlay.surface, width, height);
        overlay.width = width; overlay.height = height; overlay.painted = false;
    }
    if (!overlay.surface || cairo_surface_status(overlay.surface) != CAIRO_STATUS_SUCCESS) { GraphicsError("Could not create a Cairo overlay surface."); return false; }
    return true;
}
void X11Engine::Hide(Overlay& overlay) noexcept {
    if (display_ && overlay.window && overlay.mapped) XUnmapWindow(display_, overlay.window);
    overlay.mapped = false;
}
void X11Engine::Destroy(Overlay& overlay) noexcept {
    if (overlay.surface) cairo_surface_destroy(overlay.surface);
    if (display_ && overlay.window) XDestroyWindow(display_, overlay.window);
    overlay = {};
}
void X11Engine::Place(Overlay& overlay, int x, int y) {
    if (!overlay.window) return;
    if (!overlay.mapped || overlay.x != x || overlay.y != y) XMoveWindow(display_, overlay.window, x, y);
    if (!overlay.mapped) {
        XMapRaised(display_, overlay.window); overlay.mapped = true;
        stackDirty_ = stackOrderDirty_ = true;
    }
    overlay.x = x; overlay.y = y;
}
void X11Engine::PaintHighlight(TimePoint now) {
    const int style = settings_.highlightStyle;
    const bool spotlight = style == kHighlightSpotlight;
    double radius = (spotlight ? settings_.spotlightRadius : settings_.highlightRadius) * scale_;
    double opacity = settings_.highlightOpacity / 100.0;
    if (settings_.highlightBreathing && !spotlight) {
        // A slow 2.6 s swell. The overlay is repainted every frame while breathing.
        const double phase = std::sin(Seconds(now) * 2 * kPi / 2.6);
        radius *= 0.92 + 0.08 * phase;
        opacity *= 0.76 + 0.24 * phase;
        highlightDirty_ = true;
    }
    if (spotlight) { Hide(highlight_); PaintSpotlight(); return; }
    const double extent = style == kHighlightCrosshair ? radius * 1.75 : style == kHighlightGlow ? radius * 1.4 : radius;
    const int size = static_cast<int>(std::ceil(extent * 2 + 8));
    if (!EnsureOverlay(highlight_, size, size, "highlight")) return;
    if (!highlight_.mapped) highlightDirty_ = true;
    Place(highlight_, pointerX_ - size / 2, pointerY_ - size / 2);
    if (highlightDirty_) {
        cairo_t* cr = cairo_create(highlight_.surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, 0, 0, 0, 0); cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        const double center = size / 2.0;
        const double thickness = std::min(radius * 2, settings_.highlightThickness * scale_);
        const auto packed = settings_.highlightColor;
        const double red = (packed & 255) / 255.0, green = ((packed >> 8) & 255) / 255.0, blue = ((packed >> 16) & 255) / 255.0;
        if (style == kHighlightGlow) {
            auto* glow = cairo_pattern_create_radial(center, center, 0, center, center, extent);
            cairo_pattern_add_color_stop_rgba(glow, 0, red, green, blue, opacity * 0.9);
            cairo_pattern_add_color_stop_rgba(glow, 0.55, red, green, blue, opacity * 0.45);
            cairo_pattern_add_color_stop_rgba(glow, 1, red, green, blue, 0);
            cairo_set_source(cr, glow);
            cairo_arc(cr, center, center, extent, 0, 2 * kPi); cairo_fill(cr);
            cairo_pattern_destroy(glow);
        } else if (style == kHighlightCrosshair) {
            cairo_set_source_rgba(cr, red, green, blue, opacity);
            const double gap = radius * 0.45;
            const std::array<std::pair<double, double>, 4> arms{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
            cairo_set_line_width(cr, std::max(1.0, thickness * 0.6));
            for (const auto& arm : arms) {
                cairo_move_to(cr, center + arm.first * gap, center + arm.second * gap);
                cairo_line_to(cr, center + arm.first * extent, center + arm.second * extent);
            }
            cairo_stroke(cr);
            cairo_set_line_width(cr, thickness);
            cairo_arc(cr, center, center, std::max(0.5, radius - thickness / 2), 0, 2 * kPi); cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, red, green, blue, opacity);
            const bool filled = style == kHighlightFilled;
            cairo_arc(cr, center, center, std::max(0.5, radius - (filled ? 0 : thickness / 2)), 0, 2 * kPi);
            if (filled) cairo_fill(cr); else { cairo_set_line_width(cr, thickness); cairo_stroke(cr); }
        }
        const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(highlight_.surface);
        if (!ok) { GraphicsError("Could not draw the pointer highlight."); return; }
        highlightDirty_ = false;
    }
    HideSpotlight();
}
void X11Engine::PaintSpotlight() {
    const Monitor* monitor = &monitors_.front();
    for (const auto& candidate : monitors_)
        if (pointerX_ >= candidate.x && pointerY_ >= candidate.y && pointerX_ < candidate.x + candidate.width && pointerY_ < candidate.y + candidate.height) { monitor = &candidate; break; }
    const double radius = std::max(4.0, settings_.spotlightRadius * scale_);
    const int size = static_cast<int>(std::ceil(radius * 2 + 8));
    if (!EnsureOverlay(spotlight_, monitor->width, monitor->height, "spotlight", 16384)) return;
    // Mapped before drawing: content drawn into an unmapped window is lost.
    Place(spotlight_, monitor->x, monitor->y);
    if (!spotPixmap_ || spotWidth_ != monitor->width || spotHeight_ != monitor->height) {
        if (spotPixmap_) cairo_surface_destroy(spotPixmap_);
        spotPixmap_ = cairo_surface_create_similar(spotlight_.surface, CAIRO_CONTENT_COLOR_ALPHA, monitor->width, monitor->height);
        spotWidth_ = monitor->width; spotHeight_ = monitor->height;
        spotlight_.painted = false;
    }
    if (!spotPixmap_ || cairo_surface_status(spotPixmap_) != CAIRO_STATUS_SUCCESS) { GraphicsError("Could not create the spotlight surface."); return; }
    const bool full = !spotlight_.painted || highlightDirty_ || size != spotSize_ ||
        spotMonitorX_ != monitor->x || spotMonitorY_ != monitor->y;
    const HoleRect next{pointerX_ - monitor->x - size / 2, pointerY_ - monitor->y - size / 2, size};
    if (!full && next.x == spotHole_.x && next.y == spotHole_.y) return;
    const double dim = settings_.spotlightDim / 100.0;
    cairo_t* cr = cairo_create(spotPixmap_);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, dim);
    if (full) cairo_paint(cr);
    else { cairo_rectangle(cr, spotHole_.x, spotHole_.y, spotHole_.size, spotHole_.size); cairo_fill(cr); }
    // The soft circle, then its colored rim.
    const double cx = next.x + size / 2.0, cy = next.y + size / 2.0;
    cairo_rectangle(cr, next.x, next.y, size, size); cairo_clip(cr);
    auto* hole = cairo_pattern_create_radial(cx, cy, 0, cx, cy, radius);
    cairo_pattern_add_color_stop_rgba(hole, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(hole, 0.78, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(hole, 1, 0, 0, 0, dim);
    cairo_pattern_set_extend(hole, CAIRO_EXTEND_PAD);
    cairo_set_source(cr, hole); cairo_paint(cr); cairo_pattern_destroy(hole);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const double thickness = std::min(radius * 2, settings_.highlightThickness * scale_);
    Color(cr, settings_.highlightColor, settings_.highlightOpacity / 100.0);
    cairo_set_line_width(cr, thickness);
    cairo_arc(cr, cx, cy, std::max(0.5, radius - thickness / 2), 0, 2 * kPi); cairo_stroke(cr);
    bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(spotPixmap_);
    // One copy into the window: the old circle filled and the new one drawn
    // arrive together.
    int left = 0, topEdge = 0, right = spotWidth_, bottom = spotHeight_;
    if (!full) {
        left = std::max(0, std::min(spotHole_.x, next.x)); topEdge = std::max(0, std::min(spotHole_.y, next.y));
        right = std::min(spotWidth_, std::max(spotHole_.x + spotHole_.size, next.x + size));
        bottom = std::min(spotHeight_, std::max(spotHole_.y + spotHole_.size, next.y + size));
    }
    if (ok && right > left && bottom > topEdge) {
        cairo_t* out = cairo_create(spotlight_.surface);
        cairo_set_operator(out, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(out, spotPixmap_, 0, 0);
        cairo_rectangle(out, left, topEdge, right - left, bottom - topEdge); cairo_fill(out);
        ok = cairo_status(out) == CAIRO_STATUS_SUCCESS; cairo_destroy(out); cairo_surface_flush(spotlight_.surface);
    }
    if (!ok) { GraphicsError("Could not draw the spotlight."); return; }
    spotlight_.painted = true; highlightDirty_ = false;
    spotHole_ = next; spotSize_ = size; spotMonitorX_ = monitor->x; spotMonitorY_ = monitor->y;
}
void X11Engine::HideSpotlight() noexcept { Hide(spotlight_); }
// Frees the monitor-sized window and pixmap whenever the spotlight is off.
void X11Engine::ResetSpotlight() noexcept {
    if (spotPixmap_) cairo_surface_destroy(spotPixmap_);
    spotPixmap_ = nullptr; spotWidth_ = spotHeight_ = spotSize_ = 0; spotHole_ = {};
    Destroy(spotlight_);
}
void X11Engine::PaintCursor(double now) {
    cairo_surface_t* image = pressed_ ? darkImage_ : normalImage_;
    if (!image) return;
    int hotX = cursorHotX_, hotY = cursorHotY_;
    const double scale = shake_.Scale(now);
    if (scale > 1 && shakeFrameCount_) {
        const double maximum = settings_.shakeMaximumScale / 100.0;
        const unsigned index = std::min(shakeFrameCount_ - 1, static_cast<unsigned>(std::lround(
            std::clamp((scale - 1) / (maximum - 1), 0.0, 1.0) * (shakeFrameCount_ - 1))));
        const auto& frame = shakeFrames_[index];
        image = pressed_ ? frame.dark : frame.normal;
        hotX = frame.hotX; hotY = frame.hotY;
    }
    const int width = cairo_image_surface_get_width(image), height = cairo_image_surface_get_height(image);
    if (!EnsureOverlay(cursor_, width, height, "pressed cursor")) return;
    if (!cursor_.mapped || image != paintedCursor_) cursorDirty_ = true;
    Place(cursor_, pointerX_ - hotX, pointerY_ - hotY);
    if (cursorDirty_) {
        cairo_t* cr = cairo_create(cursor_.surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_surface(cr, image, 0, 0); cairo_paint(cr);
        const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(cursor_.surface);
        if (!ok) { GraphicsError("Could not draw the pointer effect."); return; }
        cursorDirty_ = false; paintedCursor_ = image;
    }
}
void X11Engine::AddParticle(unsigned button) {
    if (!compositor_ || !visual_ || graphicsFailed_) return;
    UpdatePointer();
    auto& particle = particles_[nextParticle_++ % particles_.size()];
    particle.x = pointerX_; particle.y = pointerY_; particle.button = button; particle.started = Clock::now(); particle.active = true;
    PaintParticle(particle, 0);
    ScheduleFrame(true);
}
void X11Engine::PaintParticle(Particle& particle, double progress) {
    double eased = progress;
    if (settings_.clickEasing == 1) eased = 1 - std::pow(1 - progress, 3);
    else if (settings_.clickEasing == 2) eased = progress * progress * (3 - 2 * progress);
    const double maximum = settings_.clickRadius * scale_;
    const double radius = maximum * (0.28 + 0.72 * eased);
    const int size = static_cast<int>(std::ceil(maximum * 2 + 8));
    if (!EnsureOverlay(particle.overlay, size, size, "click")) return;
    Place(particle.overlay, particle.x - size / 2, particle.y - size / 2);
    cairo_t* cr = cairo_create(particle.overlay.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, 0, 0, 0, 0); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const auto color = particle.button == 1 ? settings_.leftClickColor : particle.button == 2 ? settings_.rightClickColor : settings_.middleClickColor;
    const double alpha = settings_.clickOpacity / 100.0 * (1 - progress);
    const double thickness = std::min(radius * 2, settings_.clickThickness * scale_);
    const double center = size / 2.0;
    const auto ring = [&](double r, double width, double a) {
        Color(cr, color, a);
        cairo_set_line_width(cr, width);
        cairo_arc(cr, center, center, std::max(0.5, r - width / 2), 0, 2 * kPi); cairo_stroke(cr);
    };
    switch (settings_.clickStyle) {
    case kClickPulse:
        Color(cr, color, alpha);
        cairo_arc(cr, center, center, std::max(0.5, radius), 0, 2 * kPi); cairo_fill(cr);
        break;
    case kClickRipple:
        ring(radius, thickness, alpha);
        ring(radius * 0.62, thickness, alpha * 0.55);
        break;
    case kClickBurst: {
        // Eight sparks fly outward and shrink; nothing is drawn at the center.
        const double spark = std::max(1.5, thickness * 0.9 * (1 - progress * 0.5));
        Color(cr, color, alpha);
        for (unsigned point = 0; point < 8; ++point) {
            const double angle = point * kPi / 4 + kPi / 8;
            cairo_arc(cr, center + std::cos(angle) * radius, center + std::sin(angle) * radius, spark, 0, 2 * kPi);
            cairo_fill(cr);
        }
        break;
    }
    case kClickNeon:
        ring(radius, thickness * 3.2, alpha * 0.28);
        ring(radius, thickness, alpha);
        break;
    case kClickTarget: {
        // A scope closing in on the click: the ring contracts to half the
        // radius and four inward ticks mark the axes; the center stays clear.
        const double scope = std::max(2.0, maximum * (1 - 0.5 * eased));
        ring(scope, thickness, alpha);
        Color(cr, color, alpha);
        cairo_set_line_width(cr, thickness);
        for (unsigned tick = 0; tick < 4; ++tick) {
            const double angle = tick * kPi / 2;
            cairo_move_to(cr, center + std::cos(angle) * scope * 0.55, center + std::sin(angle) * scope * 0.55);
            cairo_line_to(cr, center + std::cos(angle) * (scope - thickness / 2), center + std::sin(angle) * (scope - thickness / 2));
        }
        cairo_stroke(cr);
        break;
    }
    default:
        ring(radius, thickness, alpha);
        break;
    }
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(particle.overlay.surface);
    if (!ok) { GraphicsError("Could not draw a click effect."); return; }
}
void X11Engine::AddTrail(TimePoint now) {
    if (!settings_.trailEnabled) return;
    if (trailAnchorSet_ && now - lastTrailSample_ >= std::chrono::milliseconds(settings_.trailDurationMs)) trailAnchorSet_ = false;
    const double spacing = std::max(2.0, settings_.trailRadius * scale_ * 0.7);
    const double dx = pointerX_ - lastTrailX_, dy = pointerY_ - lastTrailY_;
    if (trailAnchorSet_ && dx * dx + dy * dy < spacing * spacing) return;
    if (settings_.trailStyle == 3 && !trailAnchorSet_) {
        lastTrailX_ = pointerX_; lastTrailY_ = pointerY_; trailAnchorSet_ = true; lastTrailSample_ = now; return;
    }
    auto& trail = trails_[nextTrail_++ % trails_.size()];
    trail.x = settings_.trailStyle == 3 ? lastTrailX_ : pointerX_;
    trail.y = settings_.trailStyle == 3 ? lastTrailY_ : pointerY_;
    trail.direction = std::atan2(dy, dx);
    trail.started = now; trail.active = true;
    lastTrailX_ = pointerX_; lastTrailY_ = pointerY_; trailAnchorSet_ = true; lastTrailSample_ = now;
}
void X11Engine::PaintTrail(Trail& trail, double progress) {
    const double maximum = settings_.trailRadius * scale_;
    const double radius = maximum * (1 - 0.35 * progress);
    const int size = static_cast<int>(std::ceil(maximum * (settings_.trailStyle == 3 ? 4 : 2) + 8));
    if (!EnsureOverlay(trail.overlay, size, size, "trail")) return;
    Place(trail.overlay, trail.x - size / 2, trail.y - size / 2);
    cairo_t* cr = cairo_create(trail.overlay.surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE); cairo_set_source_rgba(cr, 0, 0, 0, 0); cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const double remaining = 1 - progress;
    Color(cr, settings_.trailColor, settings_.trailOpacity / 100.0 * remaining * remaining);
    const double center = size / 2.0;
    if (settings_.trailStyle == 3) {
        cairo_translate(cr, center, center); cairo_rotate(cr, trail.direction);
        const double head = radius * .5;
        cairo_move_to(cr, -2 * radius, 0);
        cairo_line_to(cr, 0, -head);
        cairo_arc(cr, 0, 0, head, -kPi / 2, kPi / 2);
        cairo_close_path(cr); cairo_fill(cr);
    } else if (settings_.trailStyle == 2 || settings_.trailStyle == 4) {
        for (unsigned point = 0; point < 8; ++point) {
            const double angle = point * kPi / 4 - kPi / 2;
            const double length = radius * (point % 2 ? (settings_.trailStyle == 4 ? 0.12 : 0.24) : 1.0);
            const double x = center + std::cos(angle) * length, y = center + std::sin(angle) * length;
            if (point) cairo_line_to(cr, x, y); else cairo_move_to(cr, x, y);
        }
        cairo_close_path(cr); cairo_fill(cr);
    } else {
        const double thickness = std::max(1.0, scale_ * 1.3);
        cairo_arc(cr, center, center, std::max(0.5, radius - (settings_.trailStyle == 1 ? thickness / 2 : 0)), 0, 2 * kPi);
        if (settings_.trailStyle == 1) { cairo_set_line_width(cr, thickness); cairo_stroke(cr); }
        else cairo_fill(cr);
    }
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS; cairo_destroy(cr); cairo_surface_flush(trail.overlay.surface);
    if (!ok) GraphicsError("Could not draw a pointer trail.");
}
bool X11Engine::OwnOverlay(Window window) const noexcept {
    if (!window) return false;
    if (window == highlight_.window || window == cursor_.window || window == lens_.window || window == keyPanel_.window ||
        window == spotlight_.window) return true;
    for (const auto& particle : particles_) if (window == particle.overlay.window) return true;
    for (const auto& trail : trails_) if (window == trail.overlay.window) return true;
    return false;
}
void X11Engine::RepairStacking() {
    if (!stackDirty_) return;
    stackDirty_ = false;
    const bool reorder = stackOrderDirty_, inspectForeign = foreignStackDirty_;
    stackOrderDirty_ = foreignStackDirty_ = false;
    bool repair = false;
    if ((!allowed_ && !keyVisible_) || !compositor_ || !visual_ || graphicsFailed_) return;
    // Collect a fixed, bottom-to-top ordering. The real cursor replacement is
    // always highest; no window takes focus or accepts pointer input.
    std::array<Overlay*, kTrailCount + kParticleCount + 8> overlays{};
    std::size_t count = 0;
    auto add = [&](Overlay& overlay) { if (overlay.mapped && overlay.window) overlays[count++] = &overlay; };
    add(spotlight_);
    for (std::size_t i = 0; i < trails_.size(); ++i) add(trails_[(nextTrail_ + i) % trails_.size()].overlay);
    const std::size_t trailCount = count;
    add(highlight_);
    for (std::size_t i = 0; i < particles_.size(); ++i) add(particles_[(nextParticle_ + i) % particles_.size()].overlay);
    add(lens_);
    add(keyPanel_);
    add(cursor_);
    if (!count) return;
    if (inspectForeign) {
        Window root, parent, *children = nullptr; unsigned childCount = 0;
        if (XQueryTree(display_, root_, &root, &parent, &children, &childCount)) {
            bool aboveOverlay = false;
            for (unsigned i = 0; i < childCount && !repair; ++i) {
                if (OwnOverlay(children[i])) {
                    for (std::size_t o = 0; o < count; ++o) if (overlays[o]->window == children[i]) aboveOverlay = true;
                } else if (aboveOverlay) {
                    XWindowAttributes attributes{};
                    if (XGetWindowAttributes(display_, children[i], &attributes) && attributes.map_state == IsViewable && attributes.c_class == InputOutput) {
                        for (std::size_t o = 0; o < count; ++o) {
                            const auto& overlay = *overlays[o];
                            if (overlay.x < attributes.x + attributes.width && overlay.x + overlay.width > attributes.x &&
                                overlay.y < attributes.y + attributes.height && overlay.y + overlay.height > attributes.y) {
                                repair = true; break;
                            }
                        }
                    }
                }
            }
        }
        if (children) XFree(children);
    }
    // This runs only after a foreign map/restack event or one of our newly
    // mapped overlays. Our own ConfigureNotify events are explicitly ignored,
    // so a stationary highlight cannot cause a self-sustaining raise loop.
    if (repair || reorder) {
        // Mapping a new trail already puts it above the older trails. Only
        // restore the few highlight/click/cursor layers above it in that case.
        // A foreign menu occluding an older stamp requires the whole group.
        for (std::size_t i = repair ? 0 : trailCount; i < count; ++i) XRaiseWindow(display_, overlays[i]->window);
    }
}
void X11Engine::Render(TimePoint now) {
    frameScheduled_ = false;
    if (!compositor_ || !visual_ || graphicsFailed_) return;
    now = Clock::now();
    // The key display runs on its own rules so it survives a hidden pointer.
    PaintKeyDisplay(now);
    const auto scheduleKeys = [&] {
        if (!keyVisible_ || keyDeadline_ == TimePoint{}) return;
        if (!frameScheduled_ || keyDeadline_ < nextFrame_) nextFrame_ = keyDeadline_;
        frameScheduled_ = true;
    };
    if (!allowed_) {
        // A held lens keeps showing live content at its resting rate.
        if (typingHold_ && settings_.magnifierEnabled && lensVisible_) {
            if (now >= nextLensFrame_) PaintLens(now);
            if (lensVisible_ && (!frameScheduled_ || nextLensFrame_ < nextFrame_)) { nextFrame_ = nextLensFrame_; frameScheduled_ = true; }
        } else HideLens();
        scheduleKeys(); XFlush(display_); return;
    }
    if (pointerDirty_) UpdatePointer();
    const bool pacingChanged = framePacer_.Configure(MonitorRate(), static_cast<unsigned>(settings_.visualRefreshRate));
    const bool effectsDue = pacingChanged || now >= nextEffectsFrame_;
    const bool effectsWork = trailMotionPending_ ||
        std::any_of(trails_.begin(), trails_.end(), [](const Trail& trail) { return trail.active; }) ||
        std::any_of(particles_.begin(), particles_.end(), [](const Particle& particle) { return particle.active; });
    if (effectsDue && trailMotionPending_) { trailMotionPending_ = false; AddTrail(now); }
    const bool idleHidden = settings_.highlightIdleHideMs > 0 &&
        now - lastMotion_ >= std::chrono::milliseconds(settings_.highlightIdleHideMs);
    const bool highlightShown = settings_.highlightEnabled && !idleHidden;
    if (highlightShown) PaintHighlight(now); else { Hide(highlight_); HideSpotlight(); }
    if (settings_.magnifierEnabled) PaintLens(now); else HideLens();
    UpdateCursorEffect(Seconds(now));
    bool animating = highlightShown && highlight_.mapped && settings_.highlightBreathing && settings_.highlightStyle != kHighlightSpotlight;
    for (auto& trail : trails_) if (trail.active) {
        const double elapsed = std::chrono::duration<double, std::milli>(now - trail.started).count();
        const double progress = std::clamp(elapsed / settings_.trailDurationMs, 0.0, 1.0);
        if (progress >= 1) { trail.active = false; Hide(trail.overlay); }
        else { if (effectsDue) PaintTrail(trail, progress); animating = true; }
    }
    for (auto& particle : particles_) if (particle.active) {
        const double elapsed = std::chrono::duration<double, std::milli>(now - particle.started).count();
        const double progress = std::clamp(elapsed / settings_.clickDurationMs, 0.0, 1.0);
        if (progress >= 1) { particle.active = false; Hide(particle.overlay); }
        else { if (effectsDue) PaintParticle(particle, progress); animating = true; }
    }
    RepairStacking();
    XFlush(display_); ++renderedFrames_;
    frameSnapshot_.store(renderedFrames_, std::memory_order_relaxed);
    const auto finished = Clock::now();
    if (effectsDue) {
        if (effectsWork) {
            const double elapsed = lastEffectsFrame_ == TimePoint{} ? 1.0 / framePacer_.Rate()
                : std::chrono::duration<double>(now - lastEffectsFrame_).count();
            framePacer_.Observe(std::chrono::duration<double>(finished - now).count(), elapsed);
            lastEffectsFrame_ = now;
        }
        const auto interval = std::chrono::nanoseconds(1000000000ull / framePacer_.Rate());
        nextEffectsFrame_ = now + interval;
        if (nextEffectsFrame_ <= finished) nextEffectsFrame_ = finished + interval;
    }
    const auto pointerRate = framePacer_.Automatic() ? framePacer_.MonitorCap() : framePacer_.Rate();
    nextPointerFrame_ = now + std::chrono::nanoseconds(1000000000ull / pointerRate);
    frameScheduled_ = !graphicsFailed_ && (animating || trailMotionPending_);
    if (frameScheduled_) nextFrame_ = nextEffectsFrame_;
    if (highlightShown && settings_.highlightIdleHideMs > 0) {
        // One extra pass at the idle deadline hides the halo; motion reschedules it.
        const auto hideAt = lastMotion_ + std::chrono::milliseconds(settings_.highlightIdleHideMs);
        if (!frameScheduled_ || hideAt < nextFrame_) nextFrame_ = hideAt;
        frameScheduled_ = true;
    }
    // Use the phase actually painted, not completion time. If drawing crosses
    // the final shrink boundary, the finite due deadline guarantees a final
    // restoration pass instead of leaving the native sprite hidden at idle.
    const double shakeWake = shake_.NextWake(Seconds(now), 1.0 / pointerRate);
    if (shakeFrameCount_ && std::isfinite(shakeWake)) {
        const auto deadline = TimePoint(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(shakeWake)));
        if (!frameScheduled_ || deadline < nextFrame_) nextFrame_ = deadline;
        frameScheduled_ = true;
    }
    if (lensVisible_) {
        // A resting lens keeps re-reading the screen so video and typing stay live.
        if (!frameScheduled_ || nextLensFrame_ < nextFrame_) nextFrame_ = nextLensFrame_;
        frameScheduled_ = true;
    }
    scheduleKeys();
}
void X11Engine::GraphicsError(const std::string& error) {
    RestorePointer(); graphicsFailed_ = true; graphicsError_ = error;
    ClearEffects(); Publish();
}
void X11Engine::CheckXError() {
    const int code = gEngineError.exchange(0, std::memory_order_acq_rel);
    if (!code) return;
    const auto resource = gErrorResource.load(std::memory_order_relaxed);
    if (code == BadWindow && !OwnOverlay(resource)) {
        // An ordinary application may disappear between an EWMH query and the
        // reply. That race must not terminate the process or disable rendering.
        if (resource == foreground_) foreground_ = None;
        return;
    }
    char description[256]{}; XGetErrorText(display_, code, description, sizeof(description));
    GraphicsError(std::string("X11 overlay error: ") + description);
}
void X11Engine::Commands() {
    if (restorePending_.exchange(false, std::memory_order_acquire)) {
        settings_.enabled = false;
        { std::lock_guard<std::mutex> lock(mutex_); pending_.enabled = false; }
        held_ = 0; devices_.clear(); ClearEffects(); audio_.SetAllowed(false); allowed_ = false; Publish();
    }
    if (settingsPending_.exchange(false, std::memory_order_acquire)) {
        Settings next; { std::lock_guard<std::mutex> lock(mutex_); next = pending_; }
        const bool visualsChanged = !SameVisuals(settings_, next);
        const bool pacingChanged = settings_.visualRefreshRate != next.visualRefreshRate;
        const bool shakeChanged = settings_.shakeStyle != next.shakeStyle || settings_.shakeSensitivity != next.shakeSensitivity ||
            settings_.shakeMaximumScale != next.shakeMaximumScale;
        const bool shakeCacheChanged = (settings_.shakeStyle == 0) != (next.shakeStyle == 0) ||
            settings_.shakeMaximumScale != next.shakeMaximumScale;
        const bool rulesChanged = settings_.excludedProcesses != next.excludedProcesses || settings_.allowedProcesses != next.allowedProcesses || settings_.pauseFullscreen != next.pauseFullscreen;
        const bool cursorChanged = settings_.darkCursorEnabled != next.darkCursorEnabled || settings_.cursorHold != next.cursorHold ||
            settings_.cursorFlashMs != next.cursorFlashMs || settings_.buttonMask != next.buttonMask;
        if (cursorChanged) { pulse_ = false; RestorePointer(); }
        if (visualsChanged) {
            for (auto& particle : particles_) { particle.active = false; Hide(particle.overlay); }
            for (auto& trail : trails_) { trail.active = false; Hide(trail.overlay); }
            spotlight_.painted = false;
            if (!(next.highlightEnabled && next.highlightStyle == kHighlightSpotlight)) ResetSpotlight();
            trailMotionPending_ = trailAnchorSet_ = false;
            highlightDirty_ = keyDirty_ = true; graphicsFailed_ = false; graphicsError_.clear(); lensError_.clear();
            if (!next.keyDisplayEnabled) HideKeys();
            if (!next.magnifierEnabled) HideLens();
        }
        settings_ = next; audio_.Update(settings_);
        const bool keysWanted = settings_.keyboardSoundsEnabled || settings_.keyDisplayEnabled;
        if (keyboardSelected_ != keysWanted || keyboardReleases_ != settings_.keyDisplayEnabled) {
            SelectInput(); XFlush(display_);
            chordModifiers_ = 0; modifierChordUsed_ = false;
        }
        if (visualsChanged && keyVisible_) ScheduleKeyFrame();
        if (shakeChanged) {
            shake_.Configure(settings_.shakeStyle, settings_.shakeSensitivity, settings_.shakeMaximumScale);
            shakeMotionPending_ = false;
            if (shakeCacheChanged) RebuildShakeCache();
        }
        if (rulesChanged) {
            exclusions_ = ParseExclusions(settings_.excludedProcesses); allowances_ = ParseExclusions(settings_.allowedProcesses);
            UpdateForeground(true);
        }
        ApplyPolicy();
        if (visualsChanged || pacingChanged || shakeChanged) { pointerDirty_ = true; ScheduleFrame(true); }
    }
    const bool locked = locked_.load(std::memory_order_acquire);
    if (locked != lastLocked_) { lastLocked_ = locked; ApplyPolicy(); }
}
void X11Engine::ProcessEvent(XEvent& event) {
    if (event.type == GenericEvent && event.xcookie.extension == xiOpcode_ && XGetEventData(display_, &event.xcookie)) {
        if (event.xcookie.evtype == XI_HierarchyChanged) { UpdatePointerDevices(); ApplyPolicy(); }
        else if (event.xcookie.evtype == XI_RawKeyPress || event.xcookie.evtype == XI_RawKeyRelease)
            RawKey(static_cast<const XIRawEvent*>(event.xcookie.data), event.xcookie.evtype == XI_RawKeyPress);
        else if (event.xcookie.evtype == XI_RawMotion || event.xcookie.evtype == XI_RawButtonPress || event.xcookie.evtype == XI_RawButtonRelease) {
            const auto* raw = static_cast<XIRawEvent*>(event.xcookie.data);
            if (!masterPointer_ || raw->deviceid == masterPointer_) {
                ++inputEvents_;
                if (event.xcookie.evtype == XI_RawMotion) {
                    pointerDirty_ = true; lastMotion_ = Clock::now();
                    if (settings_.shakeStyle) shakeMotionPending_ = true;
                    if (allowed_ && settings_.trailEnabled) trailMotionPending_ = true;
                    if (settings_.highlightEnabled || settings_.trailEnabled || settings_.magnifierEnabled || pressed_ || ownHide_) ScheduleFrame(false);
                }
                else RawButton(raw->sourceid, raw->detail, event.xcookie.evtype == XI_RawButtonPress);
            }
        }
        XFreeEventData(display_, &event.xcookie); return;
    }
    if (event.type == fixesEvent_ + XFixesCursorNotify) { CaptureCursor(); return; }
    if (event.type == fixesEvent_ + XFixesSelectionNotify) {
        const auto* selection = reinterpret_cast<XFixesSelectionNotifyEvent*>(&event);
        if (selection->selection == compositorAtom_) {
            const bool present = XGetSelectionOwner(display_, compositorAtom_) != None;
            if (compositor_ != present) { ClearEffects(); compositor_ = present; ApplyPolicy(); ScheduleFrame(true); }
        }
        return;
    }
    if (randr_ && (event.type == randrEvent_ + RRScreenChangeNotify || event.type == randrEvent_ + RRNotify)) {
        XRRUpdateConfiguration(&event); UpdateMonitors(); UpdateForeground(); ScheduleFrame(true); return;
    }
    Window stacked = None;
    if (event.type == MapNotify && event.xmap.event == root_) stacked = event.xmap.window;
    else if (event.type == ConfigureNotify && event.xconfigure.event == root_) stacked = event.xconfigure.window;
    else if (event.type == CirculateNotify && event.xcirculate.event == root_) stacked = event.xcirculate.window;
    if (stacked && !OwnOverlay(stacked) && allowed_) stackDirty_ = foreignStackDirty_ = true;
    if (event.type == PropertyNotify && (event.xproperty.window == root_ || event.xproperty.window == foreground_)) {
        if (event.xproperty.atom == activeAtom_ || event.xproperty.atom == wmStateAtom_ || event.xproperty.atom == typeAtom_ || event.xproperty.atom == pidAtom_ || event.xproperty.atom == frameExtentsAtom_)
            UpdateForeground(event.xproperty.atom == pidAtom_);
    } else if ((event.type == ConfigureNotify || event.type == DestroyNotify || event.type == FocusOut || event.type == FocusIn) && event.xany.window == foreground_) {
        UpdateForeground();
    } else if (event.type == Expose) {
        if (event.xexpose.window == highlight_.window) highlightDirty_ = true;
        if (event.xexpose.window == cursor_.window) cursorDirty_ = true;
        if (event.xexpose.window == keyPanel_.window) { keyDirty_ = true; ScheduleKeyFrame(); }
        if (event.xexpose.window == spotlight_.window) spotlight_.painted = false;
        ScheduleFrame(true);
    }
}
void X11Engine::Publish() {
    EngineStatus next;
    next.running = display_ && !stopping_.load(std::memory_order_relaxed);
    next.globalSupported = display_ != nullptr;
    next.backend = "Native X11"; next.heldButtons = held_; next.pressed = pressed_;
    next.pausedByRule = locked_.load(std::memory_order_relaxed) || foregroundExcluded_ ||
        FullscreenPaused() || !cursorVisible_;
    next.inputEvents = inputEvents_; next.renderedFrames = renderedFrames_; next.wakeups = wakeups_;
    if (!settings_.enabled) next.stateText = "Paused — your normal cursor is restored.";
    else if (locked_.load(std::memory_order_relaxed)) next.stateText = "Paused while the session is locked.";
    else if (foregroundExcluded_) next.stateText = "Paused in excluded application " + foregroundName_ + ".";
    else if (FullscreenPaused()) next.stateText = "Paused in the fullscreen application.";
    else if (!cursorVisible_) next.stateText = "Paused for a fully transparent application cursor.";
    else if (!compositor_) next.stateText = "Listening — visual effects require an active X11 compositing manager.";
    else if (graphicsFailed_) next.stateText = "Visual effects paused after a graphics error; sound input remains active.";
    else if (pressed_) next.stateText = settings_.cursorHold ? "Pressed cursor is active while the selected button is held." : "Pressed cursor pulse is active.";
    else next.stateText = "Ready — X11 cursor inversion, highlights, trails, click effects, magnifier and key display.";
    next.capabilities = "XInput2 global mouse events; current-cursor inversion through a small software overlay; hold or timed pulse; optional Plasma-style/macOS-inspired shake enlargement of cached native cursor pixels; filled/ring/glow/crosshair/spotlight highlights with optional breathing and idle hide; optional bounded dot/ring/sparkle/comet/fine-sparkle trails; ring/pulse/ripple/burst/neon/target left, right and middle click effects; a magnifier lens beside the pointer refreshed from the screen about 30 times a second; an on-screen key display; adaptive/manual frame pacing; fullscreen pause with executable/WM_CLASS allowlist and exclusions; PCM/float WAV click and keyboard sounds through PulseAudio or PipeWire. "
        "Keyboard input is subscribed only while keyboard sounds or the key display are switched on. Sounds reduce each press to a coarse category (key, space, enter, backspace); the key display keeps a label only while it is on screen. Nothing is recorded. "
        "Cursor overlays require a compositing manager and follow compositor timing, not hardware-cursor timing. The spotlight dims the pointer's monitor only; the lens always sits beside the pointer because a centered lens would capture itself. Alternate profiles, reverse, custom cursor files and cursor-size changes are not supported on X11. General pointer confinement and XFixes-hidden cursor state cannot be queried; use fullscreen pause and explicit game exclusions. Common ScreenSaver D-Bus lock signals are honored. No Xwayland global-input fallback.";
    next.lastError = graphicsError_;
    if (!shakeError_.empty()) next.lastError += (next.lastError.empty() ? "" : "\n") + shakeError_;
    if (!lensError_.empty()) next.lastError += (next.lastError.empty() ? "" : "\n") + lensError_;
    if (multiplePointers_ && (settings_.darkCursorEnabled || settings_.shakeStyle))
        next.lastError += (next.lastError.empty() ? "" : "\n") + std::string("Cursor inversion and shake enlargement require one master pointer; unrelated pointer sprites are left untouched.");
    if (settings_.cursorStyle || settings_.cursorReverse || settings_.cursorSize || !settings_.releasedCursorPath.empty() || !settings_.pressedCursorPath.empty())
        next.lastError += (next.lastError.empty() ? "" : "\n") + std::string("Saved Windows cursor profile options are unavailable on X11; the current Linux cursor is used.");
    std::lock_guard<std::mutex> lock(mutex_); status_ = std::move(next);
}
void X11Engine::Cleanup() noexcept {
    if (display_) {
        ClearEffects(); HideKeys(); audio_.SetAllowed(false);
        Destroy(cursor_); Destroy(highlight_); Destroy(lens_); Destroy(keyPanel_);
        for (auto& particle : particles_) Destroy(particle.overlay);
        ResetSpotlight();
        for (auto& trail : trails_) Destroy(trail.overlay);
        ClearShakeCache();
        if (darkImage_) { cairo_surface_destroy(darkImage_); darkImage_ = nullptr; }
        if (normalImage_) { cairo_surface_destroy(normalImage_); normalImage_ = nullptr; }
        if (colormap_) { XFreeColormap(display_, colormap_); colormap_ = None; }
        {
            std::lock_guard<std::mutex> lifecycle(XErrorHandlerLifecycleMutex());
            XSync(display_, False);
            XCloseDisplay(display_); display_ = nullptr;
            if (handlerInstalled_) {
                const auto previous = gPreviousErrorHandler.load();
                const auto current = XSetErrorHandler(previous);
                if (current != EngineXError) XSetErrorHandler(current);
                gEngineDisplay.store(nullptr); handlerInstalled_ = false;
            }
        }
    }
    held_ = 0; devices_.clear(); allowed_ = false; visual_ = nullptr;
    audio_.Stop();
    std::lock_guard<std::mutex> lock(mutex_); status_.running = false; status_.pressed = false; status_.heldButtons = 0;
}
void X11Engine::Run() noexcept {
    try {
        std::string error;
        const bool success = Initialize(error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            startFinished_ = true; startSucceeded_ = success; startError_ = error;
            if (!success) { status_.lastError = error; status_.stateText = "The X11 backend is unavailable."; status_.backend = "X11"; }
        }
        readyCv_.notify_all();
        if (!success) { Cleanup(); return; }
        while (!stopping_.load(std::memory_order_acquire)) {
            Commands();
            // Bound one drain to preserve rendering/control responsiveness while
            // keeping every button transition. Movement merely sets one flag.
            unsigned count = 0;
            while (count++ < 256 && XPending(display_)) { XEvent event; XNextEvent(display_, &event); ProcessEvent(event); }
            inputSnapshot_.store(inputEvents_, std::memory_order_relaxed);
            CheckXError();
            ObserveShake();
            const auto now = Clock::now();
            if (pointerDirty_ && now - lastRuleCheck_ >= kSafetyInterval) UpdateForeground();
            if (pulse_ && now >= pulseDeadline_) { pulse_ = false; ApplyPolicy(); }
            if (held_ && now >= nextSafety_) ReconcileButtons();
            if (frameScheduled_ && now >= nextFrame_) Render(now);
            RepairStacking();
            if (stopping_.load(std::memory_order_acquire)) break;
            if (XPending(display_)) continue;
            XFlush(display_);
            std::int64_t timeout = -1;
            auto before = [&](TimePoint deadline) {
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
                const auto remaining = std::max<std::int64_t>(0, ns);
                timeout = timeout < 0 ? remaining : std::min(timeout, remaining);
            };
            if (frameScheduled_) before(nextFrame_);
            if (held_) before(nextSafety_);
            if (pulse_) before(pulseDeadline_);
            std::array<pollfd, 2> watched{{{ConnectionNumber(display_), POLLIN, 0}, {wake_, POLLIN, 0}}};
            timespec delay{};
            if (timeout >= 0) { delay.tv_sec = timeout / 1000000000; delay.tv_nsec = timeout % 1000000000; }
            const int result = ppoll(watched.data(), watched.size(), timeout < 0 ? nullptr : &delay, nullptr);
            ++wakeups_;
            wakeSnapshot_.store(wakeups_, std::memory_order_relaxed);
            if (result < 0 && errno != EINTR) { GraphicsError("Could not wait for X11 input events."); break; }
            if (watched[0].revents & (POLLHUP | POLLERR | POLLNVAL)) { RestorePointer(); break; }
            if (watched[1].revents & POLLIN) { std::uint64_t value; while (read(wake_, &value, sizeof(value)) == sizeof(value)) {} }
        }
    } catch (const std::exception& error) {
        if (display_) RestorePointer();
        std::lock_guard<std::mutex> lock(mutex_); startFinished_ = true; startError_ = error.what(); status_.lastError = error.what(); readyCv_.notify_all();
    } catch (...) {
        if (display_) RestorePointer();
        std::lock_guard<std::mutex> lock(mutex_); startFinished_ = true; startError_ = "The X11 backend stopped unexpectedly."; status_.lastError = startError_; readyCv_.notify_all();
    }
    Cleanup();
}
} // namespace

std::unique_ptr<LinuxEngine> CreateX11Engine() { return std::make_unique<X11Engine>(); }
} // namespace clickshade::linuxapp
