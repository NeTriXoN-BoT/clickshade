#include "hotkeys.h"
#include "x11_errors.h"

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <glib-unix.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

namespace clickshade::linuxapp {
namespace {
std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Upper(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::array<std::string, kHotkeyActionCount> Values(const Settings& s) {
    return {{s.hotkeyToggleAll, s.hotkeyHighlight, s.hotkeyClicks, s.hotkeySounds, s.hotkeySettings, s.hotkeyRestore,
             s.hotkeyKeyboardSounds, s.hotkeyMagnifier, s.hotkeyKeyDisplay}};
}

// Xlib's handler is process-wide. Limit this trap to our own connection and
// forward other displays to the prior handler, including GTK and the renderer.
std::atomic<Display*> trapDisplay{nullptr};
std::atomic<int> trapError{0};
std::atomic<XErrorHandler> priorHandler{nullptr};
int RegistrationError(Display* display, XErrorEvent* event) {
    if (display == trapDisplay.load(std::memory_order_acquire)) {
        trapError.store(event->error_code, std::memory_order_release); return 0;
    }
    const auto previous = priorHandler.load(std::memory_order_acquire);
    return previous && previous != RegistrationError ? previous(display, event) : 0;
}
struct ErrorTrap {
    explicit ErrorTrap(Display* target)
        : lifecycle(XErrorHandlerLifecycleMutex()), display(target) {
        XSync(display, False);
        trapError.store(0); trapDisplay.store(display);
        priorHandler.store(XSetErrorHandler(RegistrationError));
    }
    ~ErrorTrap() {
        XSync(display, False);
        const auto current = XSetErrorHandler(priorHandler.load());
        if (current != RegistrationError) XSetErrorHandler(current);
        trapDisplay.store(nullptr);
    }
    int Check() { XSync(display, False); return trapError.exchange(0); }
    std::unique_lock<std::mutex> lifecycle;
    Display* display;
};
KeySym Symbol(const std::string& key) {
    if (key == "Space") return XK_space;
    if (key == "Tab") return XK_Tab;
    if (key == "Enter") return XK_Return;
    if (key == "Esc") return XK_Escape;
    if (key == "Backspace") return XK_BackSpace;
    if (key == "PageUp") return XK_Page_Up;
    if (key == "PageDown") return XK_Page_Down;
    return XStringToKeysym(key.c_str());
}
} // namespace

bool ParseHotkey(const std::string& input, HotkeyChord& chord, std::string& error) {
    chord = {}; error.clear();
    const auto text = Trim(input);
    if (text.empty()) return true;
    if (text.size() > 80) { error = "Use a short shortcut such as Ctrl+Alt+F10."; return false; }
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find('+', begin);
        const auto token = Upper(Trim(text.substr(begin, end == std::string::npos ? end : end - begin)));
        unsigned modifier = 0;
        if (token == "CTRL" || token == "CONTROL") modifier = HotkeyChord::Control;
        else if (token == "ALT") modifier = HotkeyChord::Alt;
        else if (token == "SHIFT") modifier = HotkeyChord::Shift;
        else if (token == "WIN" || token == "SUPER") modifier = HotkeyChord::Win;
        if (modifier) {
            if ((chord.modifiers & modifier) || !chord.key.empty()) { error = "List each modifier once, followed by one key."; return false; }
            chord.modifiers |= modifier;
        } else {
            if (!chord.key.empty()) { error = "A shortcut can contain only one main key."; return false; }
            if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9'))) chord.key = token;
            else if (token.size() >= 2 && token.size() <= 3 && token[0] == 'F' &&
                     std::all_of(token.begin() + 1, token.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                const int n = std::stoi(token.substr(1));
                if (n >= 1 && n <= 24) chord.key = "F" + std::to_string(n);
            } else {
                const std::array<std::pair<const char*, const char*>, 17> names{{
                    {"SPACE", "Space"}, {"TAB", "Tab"}, {"ENTER", "Enter"}, {"RETURN", "Enter"},
                    {"ESC", "Esc"}, {"ESCAPE", "Esc"}, {"BACKSPACE", "Backspace"}, {"INSERT", "Insert"},
                    {"DELETE", "Delete"}, {"HOME", "Home"}, {"END", "End"}, {"PAGEUP", "PageUp"},
                    {"PAGEDOWN", "PageDown"}, {"LEFT", "Left"}, {"RIGHT", "Right"}, {"UP", "Up"}, {"DOWN", "Down"}}};
                for (const auto& name : names) if (token == name.first) { chord.key = name.second; break; }
            }
            if (chord.key.empty()) { error = "Use A–Z, 0–9, F1–F24, or a navigation key."; return false; }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (chord.key.empty() || !(chord.modifiers & (HotkeyChord::Control | HotkeyChord::Alt | HotkeyChord::Win))) {
        error = "Include Ctrl, Alt, or Win/Super so ordinary typing stays available."; return false;
    }
    if (chord.modifiers & HotkeyChord::Control) chord.canonical += "Ctrl+";
    if (chord.modifiers & HotkeyChord::Alt) chord.canonical += "Alt+";
    if (chord.modifiers & HotkeyChord::Shift) chord.canonical += "Shift+";
    if (chord.modifiers & HotkeyChord::Win) chord.canonical += "Win+";
    chord.canonical += chord.key;
    return true;
}

struct GlobalHotkeys::Impl {
    struct Binding { int code; unsigned modifiers; HotkeyAction action; std::string name; };
    Display* display = nullptr;
    Window root = 0;
    guint source = 0;
    unsigned numLock = 0;
    bool detectableRepeat = false;
    bool configured = false;
    std::array<std::string, kHotkeyActionCount> values{};
    std::vector<Binding> bindings;
    std::set<int> held;
    std::function<void(HotkeyAction)> action;
    std::string error;

    void Locks() {
        numLock = 0;
        const auto code = XKeysymToKeycode(display, XK_Num_Lock);
        auto* map = XGetModifierMapping(display);
        if (!map) return;
        for (int m = 0; m < 8; ++m)
            for (int i = 0; i < map->max_keypermod; ++i)
                if (code && map->modifiermap[m * map->max_keypermod + i] == code) numLock |= 1u << m;
        XFreeModifiermap(map);
    }
    bool Grab(const std::vector<Binding>& proposed, ErrorTrap& trap, std::string& message) {
        const std::set<unsigned> locks{0u, LockMask, numLock, LockMask | numLock};
        for (const auto& binding : proposed) {
            for (const auto extra : locks)
                XGrabKey(display, binding.code, binding.modifiers | extra, root, False, GrabModeAsync, GrabModeAsync);
            const int result = trap.Check();
            if (result) {
                message = result == BadAccess ? binding.name + " is already used by another application. Choose a different shortcut."
                    : binding.name + " could not be registered on this keyboard.";
                return false;
            }
        }
        return true;
    }
    bool Apply(const Settings& settings, std::string& message, bool force = false) {
        if (!display) { message = "Global shortcuts need a native X11 session."; error = message; return false; }
        const auto requested = Values(settings);
        if (!force && configured && requested == values) { error.clear(); message.clear(); return true; }
        std::vector<Binding> proposed;
        std::set<std::string> unique;
        for (std::size_t i = 0; i < requested.size(); ++i) {
            HotkeyChord parsed;
            if (!ParseHotkey(requested[i], parsed, message)) { error = message; return false; }
            if (parsed.canonical.empty()) continue;
            const bool controlAlt = (parsed.modifiers & (HotkeyChord::Control | HotkeyChord::Alt)) ==
                                    (HotkeyChord::Control | HotkeyChord::Alt);
            if (controlAlt && (parsed.key == "Backspace" ||
                (parsed.key.size() > 1 && parsed.key[0] == 'F' && std::stoi(parsed.key.substr(1)) <= 12))) {
                error = message = parsed.canonical + " can be reserved by the X server. Use a different combination, such as Ctrl+Shift+F10.";
                return false;
            }
            if (!unique.insert(parsed.canonical).second) { error = message = "Each action needs a different shortcut: " + parsed.canonical + "."; return false; }
            const int code = XKeysymToKeycode(display, Symbol(parsed.key));
            if (!code) { error = message = parsed.key + " is unavailable on this keyboard."; return false; }
            unsigned modifiers = 0;
            if (parsed.modifiers & HotkeyChord::Control) modifiers |= ControlMask;
            if (parsed.modifiers & HotkeyChord::Alt) modifiers |= Mod1Mask;
            if (parsed.modifiers & HotkeyChord::Shift) modifiers |= ShiftMask;
            if (parsed.modifiers & HotkeyChord::Win) modifiers |= Mod4Mask;
            proposed.push_back({code, modifiers, static_cast<HotkeyAction>(i), parsed.canonical});
        }
        ErrorTrap trap(display);
        XUngrabKey(display, AnyKey, AnyModifier, root);
        trap.Check();
        Locks();
        if (!Grab(proposed, trap, message)) {
            XUngrabKey(display, AnyKey, AnyModifier, root); trap.Check();
            std::string previousError;
            if (!Grab(bindings, trap, previousError)) message += " " + previousError;
            error = message; return false;
        }
        bindings = std::move(proposed); values = requested; held.clear();
        configured = true;
        error.clear(); message.clear();
        return true;
    }
    gboolean Dispatch(GIOCondition condition) {
        if (condition & (G_IO_HUP | G_IO_ERR)) {
            source = 0; error = "The global-shortcut connection closed. Reopen ClickShade to retry."; return G_SOURCE_REMOVE;
        }
        while (display && XPending(display)) {
            XEvent event{}; XNextEvent(display, &event);
            if (event.type == MappingNotify) {
                XRefreshKeyboardMapping(&event.xmapping);
                if (!configured) continue;
                Settings current;
                current.hotkeyToggleAll = values[0]; current.hotkeyHighlight = values[1]; current.hotkeyClicks = values[2];
                current.hotkeySounds = values[3]; current.hotkeySettings = values[4]; current.hotkeyRestore = values[5];
                current.hotkeyKeyboardSounds = values[6]; current.hotkeyMagnifier = values[7]; current.hotkeyKeyDisplay = values[8];
                std::string message; Apply(current, message, true);
            } else if (event.type == KeyRelease) {
                if (!detectableRepeat && XEventsQueued(display, QueuedAfterReading)) {
                    XEvent next{}; XPeekEvent(display, &next);
                    if (next.type == KeyPress && next.xkey.keycode == event.xkey.keycode && next.xkey.time == event.xkey.time) continue;
                }
                held.erase(event.xkey.keycode);
            } else if (event.type == KeyPress) {
                if (!held.insert(event.xkey.keycode).second) continue;
                const unsigned state = event.xkey.state & (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask) & ~numLock;
                for (const auto& binding : bindings) if (binding.code == static_cast<int>(event.xkey.keycode) && binding.modifiers == state) {
                    // The callback may reconfigure or stop this service. Copy the
                    // action before invoking it and do not retain vector refs.
                    const auto selected = binding.action;
                    if (action) action(selected);
                    break;
                }
            }
        }
        return G_SOURCE_CONTINUE;
    }
    void Stop() {
        if (source) { g_source_remove(source); source = 0; }
        if (display) {
            XUngrabKey(display, AnyKey, AnyModifier, root);
            XUngrabKeyboard(display, CurrentTime);
            XCloseDisplay(display); display = nullptr;
        }
        bindings.clear(); held.clear(); values = {}; configured = false; error.clear(); action = {};
    }
};

GlobalHotkeys::GlobalHotkeys() : impl_(std::make_unique<Impl>()) {}
GlobalHotkeys::~GlobalHotkeys() { Stop(); }
bool GlobalHotkeys::Start(const Settings& settings, std::function<void(HotkeyAction)> action, std::string& error) {
    Stop();
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) { error = "The X11 global-shortcut connection could not be opened."; impl_->error = error; return false; }
    impl_->root = DefaultRootWindow(impl_->display);
    impl_->action = std::move(action);
    Bool supported = False;
    XkbSetDetectableAutoRepeat(impl_->display, True, &supported);
    impl_->detectableRepeat = supported;
    impl_->source = g_unix_fd_add(ConnectionNumber(impl_->display), static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
        +[](gint, GIOCondition condition, gpointer data) -> gboolean { return static_cast<Impl*>(data)->Dispatch(condition); }, impl_.get());
    return impl_->Apply(settings, error, true);
}
bool GlobalHotkeys::Apply(const Settings& settings, std::string& error) { return impl_->Apply(settings, error); }
void GlobalHotkeys::Stop() { impl_->Stop(); }
std::string GlobalHotkeys::LastError() const { return impl_->error; }

} // namespace clickshade::linuxapp
