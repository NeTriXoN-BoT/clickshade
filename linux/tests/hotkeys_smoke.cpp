#include "hotkeys.h"

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <glib.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace clickshade::linuxapp;
namespace {
unsigned passed = 0, failed = 0;
void Check(bool value, const char* description) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
    value ? ++passed : ++failed;
}
void Pump(int milliseconds = 80) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
}
int capturedError = 0;
int Error(Display*, XErrorEvent* event) { capturedError = event->error_code; return 0; }
bool CanGrab(Display* display, KeySym key, unsigned mods) {
    capturedError = 0;
    const auto old = XSetErrorHandler(Error);
    XGrabKey(display, XKeysymToKeycode(display, key), mods, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync);
    XSync(display, False);
    const bool result = !capturedError;
    XUngrabKey(display, XKeysymToKeycode(display, key), mods, DefaultRootWindow(display)); XSync(display, False);
    XSetErrorHandler(old);
    return result;
}
void Key(Display* display, KeySym key, bool down) {
    XTestFakeKeyEvent(display, XKeysymToKeycode(display, key), down, CurrentTime); XFlush(display);
}
void Chord(Display* display, KeySym key, bool shift = true) {
    Key(display, XK_Control_L, true); if (shift) Key(display, XK_Shift_L, true);
    Key(display, key, true); Pump(30); Key(display, key, false);
    if (shift) Key(display, XK_Shift_L, false);
    Key(display, XK_Control_L, false); Pump();
}
} // namespace

int main(int argc, char** argv) {
    const bool interactive = argc == 2 && std::string(argv[1]) == "--run";
    if (argc > 1 && !interactive && std::string(argv[1]) != "--headless") {
        std::puts("Usage: hotkeys_smoke [--headless|--run] (interactive mode needs a disposable X11 desktop)"); return 2;
    }
    HotkeyChord chord; std::string error;
    Check(ParseHotkey("", chord, error) && chord.canonical.empty(), "empty shortcut disables an action");
    Check(ParseHotkey(" ctrl + shift + f10 ", chord, error) && chord.canonical == "Ctrl+Shift+F10", "shortcut spelling and whitespace canonicalize");
    Check(ParseHotkey("Control+Alt+q", chord, error) && chord.canonical == "Ctrl+Alt+Q", "Control alias and letter case canonicalize");
    Check(ParseHotkey("Super+PageDown", chord, error) && chord.canonical == "Win+PageDown", "Super maps to the shared Win modifier name");
    Check(ParseHotkey("Alt+F24", chord, error), "last supported function key is accepted");
    Check(!ParseHotkey("Alt+F25", chord, error), "out-of-range function key is rejected");
    Check(!ParseHotkey("A", chord, error), "bare typing key is rejected");
    Check(!ParseHotkey("F10", chord, error), "bare function key is rejected");
    Check(!ParseHotkey("Shift+A", chord, error), "Shift alone cannot capture ordinary typing");
    Check(!ParseHotkey("Ctrl+Ctrl+A", chord, error), "duplicate modifiers are rejected");
    Check(!ParseHotkey("Ctrl+A+B", chord, error), "multiple primary keys are rejected");
    Check(!ParseHotkey("Ctrl+", chord, error), "incomplete shortcut is rejected");
    Check(!ParseHotkey("Ctrl+Alt", chord, error), "modifier-only shortcut is rejected");
    Check(!ParseHotkey("Ctrl+A+", chord, error), "trailing empty key is rejected");
    Check(!ParseHotkey("Ctrl+UnrecognizedKey", chord, error), "unsupported key name is rejected");
    Check(!ParseHotkey(std::string(200, 'x'), chord, error), "oversized shortcut is rejected before registration");
    Check(Settings{}.hotkeyRestore == "Ctrl+Shift+F10", "Linux restore default avoids the virtual-terminal chord");
    if (!interactive) {
        std::printf("\n%u passed, %u failed. Interactive grab checks require --run.\n", passed, failed);
        return failed ? 1 : 0;
    }
    if (const char* wayland = std::getenv("WAYLAND_DISPLAY"); wayland && *wayland) {
        std::puts("FAIL choose a native disposable X11 session; Xwayland is not a global-hotkey test."); return 1;
    }
    XInitThreads();
    Display* display = XOpenDisplay(nullptr);
    if (!display) { std::puts("FAIL X11 test display unavailable"); return 1; }
    int event = 0, major = 0, minor = 0, base = 0;
    if (!XTestQueryExtension(display, &event, &base, &major, &minor)) { XCloseDisplay(display); std::puts("FAIL XTest is required only by this developer test"); return 1; }
    auto window = XCreateSimpleWindow(display, DefaultRootWindow(display), 20, 20, 200, 120, 0, 0, 0);
    XStoreName(display, window, "ClickShade shortcut test host");
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask); XMapWindow(display, window); XSync(display, False);
    // A window manager handles MapRequest asynchronously. A server round-trip
    // alone does not make a managed client viewable before SetInputFocus.
    XWindowAttributes attributes{};
    for (unsigned wait = 0; wait < 100; ++wait) {
        if (XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable) break;
        Pump(10);
    }
    if (attributes.map_state != IsViewable) {
        XDestroyWindow(display, window); XCloseDisplay(display);
        std::puts("FAIL test host did not become viewable"); return 1;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime); XFlush(display); Pump();
    std::array<unsigned, kHotkeyActionCount> invoked{};
    Settings settings;
    settings.hotkeyToggleAll = "Ctrl+Shift+F6"; settings.hotkeyHighlight = "Ctrl+Shift+F7";
    settings.hotkeyClicks = "Ctrl+Shift+F8"; settings.hotkeySounds = "Ctrl+Shift+F9";
    settings.hotkeySettings = "Ctrl+Shift+F11"; settings.hotkeyKeyboardSounds = "Ctrl+Shift+F5";
    settings.hotkeyMagnifier = "Ctrl+Shift+F3"; settings.hotkeyKeyDisplay = "Ctrl+Shift+F4";
    GlobalHotkeys hotkeys;
    Check(hotkeys.Start(settings, [&](HotkeyAction action) { ++invoked[static_cast<unsigned>(action)]; }, error), "all nine shortcuts register with standard passive X11 grabs");
    for (const auto key : {XK_F6, XK_F7, XK_F8, XK_F9, XK_F11, XK_F10, XK_F5, XK_F3, XK_F4}) Chord(display, key);
    Check(invoked == std::array<unsigned, kHotkeyActionCount>{{1,1,1,1,1,1,1,1,1}}, "each shortcut dispatches its own action once");
    const auto beforeHold = invoked[0];
    Key(display, XK_Control_L, true); Key(display, XK_Shift_L, true); Key(display, XK_F6, true); Pump(850);
    Check(invoked[0] == beforeHold + 1, "holding a shortcut suppresses keyboard autorepeat toggles");
    Key(display, XK_F6, false); Key(display, XK_Shift_L, false); Key(display, XK_Control_L, false); Pump();
    Chord(display, XK_F6); Check(invoked[0] == beforeHold + 2, "physical release permits the next deliberate press");
    XkbStateRec originalState{}; XkbGetState(display, XkbUseCoreKbd, &originalState);
    Key(display, XK_Caps_Lock, true); Key(display, XK_Caps_Lock, false); Pump();
    const auto beforeLock = invoked[1]; Chord(display, XK_F7);
    Check(invoked[1] == beforeLock + 1, "Caps Lock does not disable a registered shortcut");
    const unsigned numMask = XkbKeysymToModifiers(display, XK_Num_Lock);
    XkbLockModifiers(display, XkbUseCoreKbd, numMask, numMask); XFlush(display); Pump();
    const auto beforeNum = invoked[1]; Chord(display, XK_F7);
    Check(numMask && invoked[1] == beforeNum + 1, "Caps Lock and Num Lock together do not disable a shortcut");
    XkbLockModifiers(display, XkbUseCoreKbd, numMask, originalState.locked_mods & numMask);
    XkbLockModifiers(display, XkbUseCoreKbd, LockMask, originalState.locked_mods & LockMask); XFlush(display); Pump();
    const auto beforePlain = invoked;
    while (XPending(display)) { XEvent ignored{}; XNextEvent(display, &ignored); }
    Key(display, XK_a, true); Key(display, XK_a, false); Pump();
    unsigned letters = 0; while (XPending(display)) { XEvent e{}; XNextEvent(display, &e); if (e.type == KeyPress && e.xkey.keycode == XKeysymToKeycode(display, XK_a)) ++letters; }
    Check(letters == 1 && invoked == beforePlain, "unassigned ordinary typing reaches the foreground application");
    Settings duplicate = settings; duplicate.hotkeyHighlight = duplicate.hotkeyToggleAll;
    Check(!hotkeys.Apply(duplicate, error) && !error.empty(), "duplicate action bindings are rejected");
    Settings reserved = settings; reserved.hotkeyRestore = "Ctrl+Alt+F10";
    Check(!hotkeys.Apply(reserved, error), "server-reserved virtual-terminal chord is rejected on X11");
    XGrabKey(display, XKeysymToKeycode(display, XK_F12), ControlMask | ShiftMask, DefaultRootWindow(display), False, GrabModeAsync, GrabModeAsync); XSync(display, False);
    Settings conflict = settings; conflict.hotkeyToggleAll = "Ctrl+Shift+F12";
    Check(!hotkeys.Apply(conflict, error) && error.find("another application") != std::string::npos, "another client's key grab is reported as a conflict");
    const auto beforeConflict = invoked[0]; Chord(display, XK_F6);
    Check(invoked[0] == beforeConflict + 1, "failed shortcut edit preserves the previous working bindings");
    XUngrabKey(display, XKeysymToKeycode(display, XK_F12), AnyModifier, DefaultRootWindow(display)); XSync(display, False);
    Check(hotkeys.Apply(conflict, error), "binding succeeds after the external conflict is removed");
    const auto beforeChanged = invoked[0]; Chord(display, XK_F12);
    Check(invoked[0] == beforeChanged + 1, "reassigned shortcut performs the intended action");
    Check(CanGrab(display, XK_F6, ControlMask | ShiftMask), "reassignment releases the old chord");
    conflict.hotkeyToggleAll.clear(); Check(hotkeys.Apply(conflict, error), "clearing a shortcut is accepted");
    Check(CanGrab(display, XK_F12, ControlMask | ShiftMask), "clearing an action releases its passive grab");
    hotkeys.Stop();
    Check(CanGrab(display, XK_F10, ControlMask | ShiftMask), "stopping releases the restore shortcut");
    XDestroyWindow(display, window); XCloseDisplay(display);
    std::printf("\n%u passed, %u failed. Physical keyboards and desktop-reserved chords still need target-system acceptance.\n", passed, failed);
    return failed ? 1 : 0;
}
