#include "engine.h"
#include "audio.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#undef Status
#include <gio/gio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace clickshade::linuxapp;
namespace {
using Clock = std::chrono::steady_clock;
Display* display = nullptr;
Window window = None;
unsigned passed = 0, failed = 0, skipped = 0, legacyDown = 0, legacyUp = 0;
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (value) ++passed; else ++failed;
}
void Pump() {
    while (XPending(display)) {
        XEvent event; XNextEvent(display, &event);
        if (event.type == ButtonPress) ++legacyDown;
        if (event.type == ButtonRelease) ++legacyUp;
    }
    while (g_main_context_iteration(nullptr, false)) {}
}
void WaitMs(unsigned milliseconds) {
    const auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    do { Pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (Clock::now() < until);
    Pump();
}
bool Until(const std::function<bool()>& test, unsigned milliseconds = 1500) {
    const auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    do { Pump(); if (test()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(2)); } while (Clock::now() < until);
    Pump(); return test();
}
void Button(unsigned button, bool down) { XTestFakeButtonEvent(display, button, down ? True : False, CurrentTime); XFlush(display); }
std::vector<Window> Overlays(const char* title, bool mappedOnly = true) {
    Window root, parent, *children = nullptr; unsigned count = 0;
    std::vector<Window> result;
    if (!XQueryTree(display, DefaultRootWindow(display), &root, &parent, &children, &count)) return result;
    for (unsigned i = 0; i < count; ++i) {
        char* name = nullptr;
        if (XFetchName(display, children[i], &name) && name) {
            if (std::strcmp(name, title) == 0) {
                XWindowAttributes attributes{};
                if (XGetWindowAttributes(display, children[i], &attributes) && (!mappedOnly || attributes.map_state == IsViewable)) result.push_back(children[i]);
            }
            XFree(name);
        }
    }
    if (children) XFree(children);
    return result;
}
Window Overlay(const char* title, bool mappedOnly = true) {
    const auto windows = Overlays(title, mappedOnly);
    return windows.empty() ? None : windows.back();
}
bool StackedAbove(Window upper, Window lower) {
    Window root, parent, *children = nullptr; unsigned count = 0;
    if (!upper || !lower || !XQueryTree(display, DefaultRootWindow(display), &root, &parent, &children, &count)) return false;
    int upperIndex = -1, lowerIndex = -1;
    for (unsigned i = 0; i < count; ++i) {
        if (children[i] == upper) upperIndex = i;
        if (children[i] == lower) lowerIndex = i;
    }
    if (children) XFree(children);
    return upperIndex >= 0 && lowerIndex >= 0 && upperIndex > lowerIndex;
}
Window Menu(int x, int y, unsigned width = 140, unsigned height = 100) {
    XSetWindowAttributes attributes{}; attributes.override_redirect = True; attributes.background_pixel = 0x263e63;
    const Window menu = XCreateWindow(display, DefaultRootWindow(display), x, y, width, height, 0,
        CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &attributes);
    XStoreName(display, menu, "ClickShade test popup menu");
    XSetTransientForHint(display, menu, window);
    const Atom type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    XChangeProperty(display, menu, XInternAtom(display, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&type), 1);
    XMapRaised(display, menu); XFlush(display);
    return menu;
}
void Activate() {
    const Atom active = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    XEvent event{}; event.xclient.type = ClientMessage; event.xclient.display = display; event.xclient.window = window;
    event.xclient.message_type = active; event.xclient.format = 32; event.xclient.data.l[0] = 2; event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XSetInputFocus(display, window, RevertToParent, CurrentTime); XFlush(display);
    WaitMs(100);
}
void Move(int x, int y) { XTestFakeMotionEvent(display, DefaultScreen(display), x, y, CurrentTime); XFlush(display); }
bool NoCursorOverlay() { return Overlay("ClickShade pressed cursor") == None; }
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--run") != 0) {
        std::puts("Use --run in an isolated native X11 test desktop; this sends real pointer/button events."); return 2;
    }
    XInitThreads();
    display = XOpenDisplay(nullptr);
    if (!display) { std::puts("An X11 display is required."); return 2; }
    int event, error, major, minor;
    if (!XTestQueryExtension(display, &event, &error, &major, &minor)) { std::puts("XTest is required."); return 2; }
    const auto root = DefaultRootWindow(display);
    const std::string compositorName = "_NET_WM_CM_S" + std::to_string(DefaultScreen(display));
    const bool compositor = XGetSelectionOwner(display, XInternAtom(display, compositorName.c_str(), False)) != None;
    if (!compositor) { std::puts("A real X11 compositing manager is required for cursor/overlay checks."); return 2; }
    window = XCreateSimpleWindow(display, root, 100, 100, 800, 600, 0, 0, 0x273441);
    XStoreName(display, window, "ClickShade native X11 integration test");
    XClassHint classHint{const_cast<char*>("clickshade-test"), const_cast<char*>("ClickShadeTest")}; XSetClassHint(display, window, &classHint);
    const unsigned long pid = getpid();
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
    XSelectInput(display, window, ExposureMask | ButtonPressMask | ButtonReleaseMask);
    Cursor original = XCreateFontCursor(display, XC_left_ptr); XDefineCursor(display, window, original);
    XMapRaised(display, window); XFlush(display); WaitMs(200); Activate(); Move(420, 350); WaitMs(50);

    Settings settings; settings.clickEffectsEnabled = false; settings.clickSoundsEnabled = false;
    auto engine = CreateX11Engine();
    std::string message;
    // A WM can briefly retain an application XID after its window is gone,
    // particularly while a GTK window is being recreated for scale changes.
    // Exercise that startup race without masking genuine XI/XFixes failures.
    const Atom active = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    const Window stale = XCreateSimpleWindow(display, root, 1, 1, 10, 10, 0, 0, 0);
    XDestroyWindow(display, stale); XSync(display, False);
    XChangeProperty(display, root, active, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&stale), 1);
    XSync(display, False);
    const bool started = engine->Start(settings, message);
    XChangeProperty(display, root, active, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&window), 1); XFlush(display);
    Check(started, "native X11 backend initializes despite a stale foreground-window XID");
    if (!message.empty()) std::printf("Initialization: %s\n", message.c_str());
    if (!engine->Status().running) { engine->Stop(); XDestroyWindow(display, window); XFreeCursor(display, original); XCloseDisplay(display); return 1; }
    Check(Until([&] { return engine->Status().globalSupported && !engine->Status().pausedByRule; }), "ordinary foreground application is allowed");
    Check(Until([] { return Overlay("ClickShade highlight") != None; }), "small ARGB pointer highlight is visible");
    const Window highlight = Overlay("ClickShade highlight");
    if (highlight) {
        XWindowAttributes attributes{}; XGetWindowAttributes(display, highlight, &attributes);
        int count = -1, order = 0; auto* rectangles = XShapeGetRectangles(display, highlight, ShapeInput, &count, &order);
        if (rectangles) XFree(rectangles);
        Check(count == 0 && attributes.override_redirect && attributes.width < 300 && attributes.height < 300, "overlay is small, unmanaged and completely input-transparent");
        auto* hints = XGetWMHints(display, highlight);
        Check(hints && (hints->flags & InputHint) && !hints->input, "overlay does not request input focus");
        if (hints) XFree(hints);
    }
    const auto beforeMenu = engine->Status();
    const Window menu = Menu(430, 360);
    Check(Until([&] { return StackedAbove(highlight, menu); }), "stationary highlight rises above a newly mapped context-menu window");
    XRaiseWindow(display, menu); XFlush(display);
    Check(Until([&] { return StackedAbove(highlight, menu); }), "foreign menu restacking repairs overlay order without pointer movement");
    Window menuFocus; int menuRevert; XGetInputFocus(display, &menuFocus, &menuRevert);
    Check(menuFocus == window && engine->Status().inputEvents == beforeMenu.inputEvents, "stack repair neither changes focus nor synthesizes mouse input");
    XDestroyWindow(display, menu); XFlush(display); WaitMs(40);
    const unsigned initialDown = legacyDown, initialUp = legacyUp;
    Button(1, true);
    Check(Until([&] { const auto s = engine->Status(); return s.pressed && s.heldButtons == 1; }), "left press displays the inverted cursor");
    Check(Until([] { return Overlay("ClickShade pressed cursor") != None; }), "pressed software cursor overlay is mapped");
    auto* actual = XFixesGetCursorImage(display);
    const auto cursorWindow = Overlay("ClickShade pressed cursor");
    bool inverted = false;
    if (actual && cursorWindow) {
        auto* pixels = XGetImage(display, cursorWindow, 0, 0, actual->width, actual->height, AllPlanes, ZPixmap);
        if (pixels) {
            inverted = true; bool opaque = false;
            for (unsigned y = 0; y < actual->height && inverted; ++y) for (unsigned x = 0; x < actual->width; ++x) {
                const auto source = static_cast<std::uint32_t>(actual->pixels[std::size_t(y) * actual->width + x]);
                const unsigned alpha = source >> 24;
                if (!alpha) continue;
                opaque = true;
                const auto wanted = ((alpha - std::min(alpha, (source >> 16) & 255u)) << 16) |
                    ((alpha - std::min(alpha, (source >> 8) & 255u)) << 8) | (alpha - std::min(alpha, source & 255u));
                if ((XGetPixel(pixels, x, y) & 0xffffff) != wanted) { inverted = false; break; }
            }
            inverted = inverted && opaque; XDestroyImage(pixels);
        }
    }
    if (actual) XFree(actual);
    Check(inverted, "visible cursor pixels are inverted with premultiplied alpha preserved");
    WaitMs(260);
    Check(engine->Status().pressed, "hold mode remains pressed beyond the pulse duration");
    Button(3, true);
    Check(Until([&] { return engine->Status().heldButtons == 3 && engine->Status().pressed; }), "overlapping left and right presses are both tracked");
    Button(1, false);
    Check(Until([&] { return engine->Status().heldButtons == 2 && engine->Status().pressed; }), "releasing left keeps the right-held cursor active");
    Button(3, false);
    Check(Until([&] { return engine->Status().heldButtons == 0 && !engine->Status().pressed && NoCursorOverlay(); }), "final release restores the real cursor and hides the overlay");
    Check(Until([&] { return legacyDown >= initialDown + 2 && legacyUp >= initialUp + 2; }), "original application receives all physical button transitions");

    settings.buttonMask = 1; engine->Update(settings); WaitMs(40);
    Button(3, true);
    Check(Until([&] { return engine->Status().heldButtons == 2 && !engine->Status().pressed; }), "unselected right button does not invert the cursor");
    Button(3, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.buttonMask = 3; settings.cursorHold = false; settings.cursorFlashMs = 160; engine->Update(settings); WaitMs(40);
    Button(1, true); WaitMs(15); Button(1, false);
    Check(Until([&] { return engine->Status().pressed && engine->Status().heldButtons == 0; }, 80), "quick release lets a short cursor pulse finish");
    Check(Until([&] { return !engine->Status().pressed && NoCursorOverlay(); }, 500), "pulse expires and restores the real cursor");
    Button(1, true); Check(Until([&] { return engine->Status().pressed; }), "held physical press begins a pulse");
    Check(Until([&] { return !engine->Status().pressed && engine->Status().heldButtons == 1; }, 500), "pulse expires even while the physical button stays held");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.cursorHold = true; engine->Update(settings); WaitMs(40);
    Button(1, true); Until([&] { return engine->Status().pressed; });
    settings.enabled = false; engine->Update(settings);
    Check(Until([&] { return !engine->Status().pressed && NoCursorOverlay() && Overlay("ClickShade highlight") == None; }), "master pause restores pointer before hiding effects");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.enabled = true; engine->Update(settings); WaitMs(40);

    const auto stateAtom = XInternAtom(display, "_NET_WM_STATE", False);
    const auto fullAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(display, window, stateAtom, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&fullAtom), 1); XFlush(display);
    Check(Until([&] { return engine->Status().pausedByRule && engine->Status().stateText.find("fullscreen") != std::string::npos; }), "EWMH fullscreen state pauses all effects");
    Button(1, true);
    Check(Until([&] { return engine->Status().heldButtons == 1 && !engine->Status().pressed; }), "fullscreen press leaves the native cursor untouched");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.allowedProcesses = "  /tmp/cLiCkShAdEtEsT  "; engine->Update(settings);
    Check(Until([&] { return !engine->Status().pausedByRule && Overlay("ClickShade highlight") != None; }), "case-insensitive basename allowlist bypasses fullscreen pause");
    Button(1, true);
    Check(Until([&] { return engine->Status().pressed; }), "allowlisted fullscreen desktop app retains pressed-cursor behavior");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.excludedProcesses = "ClickShadeTest"; engine->Update(settings);
    Check(Until([&] { return engine->Status().pausedByRule && engine->Status().stateText.find("excluded") != std::string::npos; }), "explicit exclusion overrides the fullscreen allowlist");
    settings.excludedProcesses.clear(); settings.allowedProcesses.clear(); engine->Update(settings);
    Check(Until([&] { return engine->Status().pausedByRule && engine->Status().stateText.find("fullscreen") != std::string::npos; }), "removing allowlist immediately restores fullscreen protection");
    XDeleteProperty(display, window, stateAtom); XFlush(display);
    Check(Until([&] { return !engine->Status().pausedByRule; }), "leaving fullscreen resumes effects");
    settings.excludedProcesses = "  cLiCkShAdEtEsT  "; engine->Update(settings);
    Check(Until([&] { return engine->Status().pausedByRule && engine->Status().stateText.find("excluded") != std::string::npos; }), "WM_CLASS exclusions are trimmed and case-insensitive");
    settings.excludedProcesses.clear(); engine->Update(settings);
    Check(Until([&] { return !engine->Status().pausedByRule; }), "clearing exclusions resumes the application");

    // A borderless client can cover a monitor without requesting EWMH
    // fullscreen. Explicit decorations/maximization distinguish desktop apps.
    const Window geometryWindow = Menu(0, 0, DisplayWidth(display, DefaultScreen(display)), DisplayHeight(display, DefaultScreen(display)));
    XDeleteProperty(display, geometryWindow, XInternAtom(display, "_NET_WM_WINDOW_TYPE", False));
    XDefineCursor(display, geometryWindow, original);
    XChangeProperty(display, root, active, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&geometryWindow), 1); XFlush(display);
    Check(Until([&] { return engine->Status().pausedByRule; }), "borderless monitor-sized foreground window receives fullscreen protection");
    const Atom frameExtents = XInternAtom(display, "_NET_FRAME_EXTENTS", False);
    const unsigned long decorated[4] = {4, 4, 22, 4};
    XChangeProperty(display, geometryWindow, frameExtents, XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(decorated), 4); XFlush(display);
    Check(Until([&] { return !engine->Status().pausedByRule; }), "decorated desktop window is not paused solely for monitor-sized geometry");
    XDeleteProperty(display, geometryWindow, frameExtents);
    const Atom maximized[2] = {XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False), XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False)};
    XChangeProperty(display, geometryWindow, stateAtom, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(maximized), 2); XFlush(display);
    Check(Until([&] { return !engine->Status().pausedByRule; }), "ordinary maximized state does not trigger the borderless fullscreen heuristic");
    XChangeProperty(display, geometryWindow, stateAtom, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&fullAtom), 1); XFlush(display);
    Check(Until([&] { return engine->Status().pausedByRule; }), "explicit fullscreen state remains authoritative");
    XChangeProperty(display, root, active, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&window), 1);
    XDestroyWindow(display, geometryWindow); XFlush(display); Activate();
    Check(Until([&] { return !engine->Status().pausedByRule; }), "closing temporary fullscreen window resumes the desktop app");
    settings.allowedProcesses = "ClickShadeTest"; engine->Update(settings); WaitMs(40);

    const char transparentBits[1] = {0};
    Pixmap transparentMask = XCreateBitmapFromData(display, root, transparentBits, 1, 1);
    XColor zero{};
    Cursor transparent = XCreatePixmapCursor(display, transparentMask, transparentMask, &zero, &zero, 0, 0);
    XDefineCursor(display, window, transparent); XFlush(display);
    Check(Until([&] { return engine->Status().pausedByRule && engine->Status().stateText.find("transparent") != std::string::npos; }), "allowlist does not bypass transparent-cursor protection");
    XDefineCursor(display, window, original); XFlush(display);
    Check(Until([&] { return !engine->Status().pausedByRule; }), "restoring a visible application cursor resumes effects");
    XFreeCursor(display, transparent); XFreePixmap(display, transparentMask);

    GError* busError = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &busError);
    if (busError) g_error_free(busError);
    if (bus) {
        Button(1, true); Until([&] { return engine->Status().pressed; });
        g_dbus_connection_emit_signal(bus, nullptr, "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "ActiveChanged", g_variant_new("(b)", TRUE), nullptr);
        g_dbus_connection_flush_sync(bus, nullptr, nullptr);
        Check(Until([&] { return engine->Status().pausedByRule && !engine->Status().pressed && engine->Status().heldButtons == 0; }), "screen lock overrides allowlist, restores pointer and clears held state");
        g_dbus_connection_emit_signal(bus, nullptr, "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "ActiveChanged", g_variant_new("(b)", FALSE), nullptr);
        g_dbus_connection_flush_sync(bus, nullptr, nullptr);
        Check(Until([&] { return !engine->Status().pausedByRule && !engine->Status().pressed; }), "unlock resumes without a stale held cursor");
        Button(1, false); WaitMs(30); g_object_unref(bus);
    } else { std::puts("SKIP session-bus screen-lock signal checks: no private session bus."); ++skipped; }
    settings.allowedProcesses.clear(); engine->Update(settings);

    settings.clickSoundsEnabled = true; settings.leftSoundPath = "/tmp/clickshade-no-such-sound-file.wav"; engine->Update(settings);
    Check(Until([&] { return engine->Status().lastError.find("Could not open") != std::string::npos; }), "missing optional audio file is reported asynchronously");
    Button(1, true);
    Check(Until([&] { return engine->Status().pressed; }), "optional audio failure leaves cursor effects running");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.clickSoundsEnabled = false; settings.leftSoundPath.clear(); engine->Update(settings);
    Check(Until([&] { return engine->Status().lastError.empty(); }), "correcting audio settings clears the optional error");

    settings.darkCursorEnabled = false; settings.highlightEnabled = true; engine->Update(settings); WaitMs(40);
    Button(1, true);
    Check(Until([&] { return engine->Status().heldButtons == 1 && !engine->Status().pressed && Overlay("ClickShade highlight") != None; }), "cursor inversion can be disabled independently of the highlight");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.visualRefreshRate = 240; engine->Update(settings); WaitMs(80);
    const auto beforeMotion = engine->Status();
    const auto beginMotion = Clock::now();
    for (int i = 0; i < 1200; ++i) XTestFakeMotionEvent(display, DefaultScreen(display), 320 + i % 300, 260 + (i / 300) * 12, CurrentTime);
    Move(630, 420);
    Check(Until([&] { return engine->Status().inputEvents > beforeMotion.inputEvents + 100; }), "high-volume raw motion is received");
    Check(Until([&] {
        const Window overlay = Overlay("ClickShade highlight");
        XWindowAttributes attributes{};
        return overlay && XGetWindowAttributes(display, overlay, &attributes) && attributes.x + attributes.width / 2 == 630 && attributes.y + attributes.height / 2 == 420;
    }), "coalesced motion delivers the latest actual pointer position");
    const auto afterMotion = engine->Status();
    const auto motionSeconds = std::chrono::duration<double>(Clock::now() - beginMotion).count();
    Check(afterMotion.renderedFrames - beforeMotion.renderedFrames <= static_cast<std::uint64_t>(motionSeconds * 240 + 12), "motion frames are bounded by the requested cadence rather than packet count");
    WaitMs(250); const auto beforeIdle = engine->Status(); WaitMs(350); const auto afterIdle = engine->Status();
    Check(afterIdle.renderedFrames <= beforeIdle.renderedFrames + 1, "stationary pointer does not run a continuing render loop");
    Check(afterIdle.wakeups <= beforeIdle.wakeups + 2, "idle event worker does not poll continuously");
    Window focus; int revert; XGetInputFocus(display, &focus, &revert);
    Check(focus == window, "movement and effects preserve the foreground application's focus");

    Check(Overlays("ClickShade trail", false).empty(), "pointer trails allocate no windows while disabled by default");
    settings.highlightEnabled = false; settings.trailEnabled = true; settings.trailRadius = 8;
    settings.trailOpacity = 80; settings.trailDurationMs = 700; engine->Update(settings); WaitMs(40);
    bool styles = true, transparentTrail = false;
    for (int style = 0; style < 5; ++style) {
        settings.trailStyle = style; engine->Update(settings); WaitMs(30);
        Move(380 + style * 35, 390);
        if (style == 3) { WaitMs(30); Move(410 + style * 35, 390); }
        if (!Until([] { return Overlay("ClickShade trail") != None; })) { styles = false; continue; }
        const Window trail = Overlay("ClickShade trail");
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, trail, &attributes)) { styles = false; continue; }
        auto* image = XGetImage(display, trail, 0, 0, attributes.width, attributes.height, AllPlanes, ZPixmap);
        if (!image) { styles = false; continue; }
        const bool centerPainted = (XGetPixel(image, attributes.width / 2, attributes.height / 2) & 0xffffff) != 0;
        bool anyPainted = false;
        for (int y = 0; y < attributes.height; ++y) for (int x = 0; x < attributes.width; ++x)
            anyPainted = anyPainted || (XGetPixel(image, x, y) & 0xffffff) != 0;
        styles = styles && anyPainted && centerPainted == (style != 1) && (XGetPixel(image, 0, 0) & 0xffffff) == 0;
        XDestroyImage(image);
        int rectangles = -1, ordering = 0;
        auto* shape = XShapeGetRectangles(display, trail, ShapeInput, &rectangles, &ordering); if (shape) XFree(shape);
        transparentTrail = attributes.override_redirect && attributes.width < 100 && rectangles == 0;
    }
    Check(styles, "all five trail styles render transparent artwork with their expected filled or ring center");
    Check(transparentTrail, "trail windows remain small and input-transparent");
    Check(Overlay("ClickShade highlight") == None && !engine->Status().pressed, "trails work with highlight and dark cursor independently disabled");
    for (int i = 0; i < 48; ++i) { Move(350 + i * 5, 410 + (i % 2) * 6); WaitMs(9); }
    Check(Overlays("ClickShade trail", false).size() == 32 && Overlays("ClickShade trail").size() <= 32, "continuous trail motion reuses a bounded pool of 32 windows");
    Check(Until([&] {
        for (Window trail : Overlays("ClickShade trail")) {
            XWindowAttributes attributes{};
            if (XGetWindowAttributes(display, trail, &attributes) && attributes.x + attributes.width / 2 == 585 && attributes.y + attributes.height / 2 == 416) return true;
        }
        return false;
    }), "trail sampling includes the latest actual pointer position");
    const auto trailWindows = Overlays("ClickShade trail");
    const Window trailMenu = Menu(330, 390, 310, 80);
    Check(Until([&] {
        return !trailWindows.empty() && std::all_of(trailWindows.begin(), trailWindows.end(), [&](Window trail) { return StackedAbove(trail, trailMenu); });
    }, 300), "stationary trail stamps rise above newly opened menu windows");
    XDestroyWindow(display, trailMenu); XFlush(display);
    Check(Until([] { return Overlays("ClickShade trail").empty(); }, 1000), "all trail stamps expire after motion stops");
    WaitMs(80); const auto beforeTrailIdle = engine->Status(); WaitMs(300); const auto afterTrailIdle = engine->Status();
    Check(afterTrailIdle.renderedFrames <= beforeTrailIdle.renderedFrames + 1 && afterTrailIdle.wakeups <= beforeTrailIdle.wakeups + 2,
        "expired trails and overlay restacking leave no continuing idle loop");
    Move(500, 430); Until([] { return Overlay("ClickShade trail") != None; });
    XChangeProperty(display, window, stateAtom, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&fullAtom), 1); XFlush(display);
    Check(Until([&] { return engine->Status().pausedByRule && Overlays("ClickShade trail").empty(); }), "fullscreen pause immediately clears active trails");
    XDeleteProperty(display, window, stateAtom); XFlush(display); Until([&] { return !engine->Status().pausedByRule; });
    Move(520, 430); Until([] { return Overlay("ClickShade trail") != None; });
    settings.trailEnabled = false; settings.highlightEnabled = true; engine->Update(settings);
    Check(Until([] { return Overlays("ClickShade trail").empty() && Overlay("ClickShade highlight") != None; }), "disabling trails clears them without disabling other effects");

    settings.highlightEnabled = true; settings.visualRefreshRate = 30; engine->Update(settings); WaitMs(70);
    const auto beforeThirty = engine->Status();
    const auto thirtyStart = Clock::now();
    for (int i = 0; i < 80; ++i) { Move(400 + i, 430); WaitMs(3); }
    WaitMs(45);
    const auto thirtyElapsed = std::chrono::duration<double>(Clock::now() - thirtyStart).count();
    Check(engine->Status().renderedFrames - beforeThirty.renderedFrames <= static_cast<std::uint64_t>(thirtyElapsed * 30 + 3),
          "manual 30 FPS bounds cached-pointer work without changing actual mouse motion");
    settings.highlightEnabled = false; settings.darkCursorEnabled = false;
    settings.visualRefreshRate = 0; settings.shakeStyle = 1; settings.shakeMaximumScale = 400;
    engine->Update(settings); WaitMs(120);
    auto* baseline = XFixesGetCursorImage(display);
    const unsigned nativeWidth = baseline ? baseline->width : 0;
    const unsigned nativeHeight = baseline ? baseline->height : 0;
    const unsigned hotX = baseline ? baseline->xhot : 0, hotY = baseline ? baseline->yhot : 0;
    const unsigned long nativeSerial = baseline ? baseline->cursor_serial : 0;
    if (baseline) XFree(baseline);
    const auto shake = [&] {
        for (unsigned i = 0; i < 8; ++i) { Move(i % 2 ? 540 : 400, 430); WaitMs(30); }
    };
    for (int i = 0; i < 8; ++i) { Move(380 + i * 20, 430); WaitMs(20); }
    Check(NoCursorOverlay(), "ordinary one-direction movement does not trigger enlargement");
    shake(); WaitMs(240);
    const auto enlarged = Overlay("ClickShade pressed cursor");
    XWindowAttributes enlargedInfo{};
    Check(nativeWidth && enlarged && XGetWindowAttributes(display, enlarged, &enlargedInfo) &&
          enlargedInfo.width >= static_cast<int>(nativeWidth * 2.8) && enlargedInfo.height >= static_cast<int>(nativeHeight * 2.8),
          "Plasma-style deliberate shake enlarges the cached native cursor to approximately three times");
    Window pointerRoot, pointerChild; int rootX, rootY, localX, localY; unsigned pointerButtons;
    const bool pointerOk = XQueryPointer(display, root, &pointerRoot, &pointerChild, &rootX, &rootY, &localX, &localY, &pointerButtons);
    Check(pointerOk && rootX == 540 && rootY == 430, "shake enlargement never warps or changes the actual pointer position");
    const int scaledHotX = nativeWidth ? static_cast<int>(std::lround(hotX * double(enlargedInfo.width) / nativeWidth)) : 0;
    const int scaledHotY = nativeHeight ? static_cast<int>(std::lround(hotY * double(enlargedInfo.height) / nativeHeight)) : 0;
    Check(enlarged && std::abs(enlargedInfo.x + scaledHotX - 540) <= 1 && std::abs(enlargedInfo.y + scaledHotY - 430) <= 1,
          "enlarged cursor preserves its aspect ratio and scaled native hotspot");
    int shakeShapeCount = -1, shakeOrdering = 0;
    auto* shakeShape = enlarged ? XShapeGetRectangles(display, enlarged, ShapeInput, &shakeShapeCount, &shakeOrdering) : nullptr;
    if (shakeShape) XFree(shakeShape);
    XGetInputFocus(display, &focus, &revert);
    Check(shakeShapeCount == 0 && focus == window, "shake overlay is click-through and preserves application focus");
    auto* normalPixels = enlarged ? XGetImage(display, enlarged, 0, 0, enlargedInfo.width, enlargedInfo.height, AllPlanes, ZPixmap) : nullptr;
    settings.darkCursorEnabled = true; engine->Update(settings); WaitMs(40);
    Button(1, true); Until([&] { return engine->Status().pressed; }); WaitMs(30);
    auto* pressedPixels = enlarged ? XGetImage(display, enlarged, 0, 0, enlargedInfo.width, enlargedInfo.height, AllPlanes, ZPixmap) : nullptr;
    bool enlargedInverted = normalPixels && pressedPixels, opaqueFound = false;
    if (normalPixels && pressedPixels) for (int y = 0; y < enlargedInfo.height && enlargedInverted; ++y)
        for (int x = 0; x < enlargedInfo.width; ++x) {
            const auto normalPixel = XGetPixel(normalPixels, x, y), pressedPixel = XGetPixel(pressedPixels, x, y);
            if ((normalPixel >> 24) != 255) continue;
            opaqueFound = true;
            for (int channel : {0, 8, 16}) if (((normalPixel >> channel) & 255) + ((pressedPixel >> channel) & 255) != 255) enlargedInverted = false;
        }
    if (normalPixels) XDestroyImage(normalPixels);
    if (pressedPixels) XDestroyImage(pressedPixels);
    Check(enlargedInverted && opaqueFound, "click-time inversion still applies to the enlarged cursor's opaque pixels");
    Button(1, false); Until([&] { return !engine->Status().pressed; });
    Check(Overlay("ClickShade pressed cursor") != None, "release returns enlarged artwork to normal until the shake hold expires");
    Check(Until(NoCursorOverlay, 2700), "shake hold and return animation restore the real pointer automatically");
    auto* restoredCursor = XFixesGetCursorImage(display);
    Check(restoredCursor && restoredCursor->cursor_serial == nativeSerial && restoredCursor->width == nativeWidth && restoredCursor->height == nativeHeight,
          "shake never replaces or resizes the application's native X cursor resource");
    if (restoredCursor) XFree(restoredCursor);
    WaitMs(60); const auto beforeShakeIdle = engine->Status(); WaitMs(300); const auto afterShakeIdle = engine->Status();
    Check(afterShakeIdle.renderedFrames <= beforeShakeIdle.renderedFrames + 1 && afterShakeIdle.wakeups <= beforeShakeIdle.wakeups + 2,
          "enabled but idle shake locator leaves no render or polling timer");
    settings.darkCursorEnabled = false; settings.shakeStyle = 0; engine->Update(settings); WaitMs(40);
    settings.shakeStyle = 1; engine->Update(settings); WaitMs(100);
    Button(1, true); Until([&] { return engine->Status().heldButtons == 1; }); shake(); WaitMs(220);
    Check(NoCursorOverlay(), "dragging with a held mouse button cannot trigger shake enlargement");
    Button(1, false); Until([&] { return engine->Status().heldButtons == 0; });
    settings.shakeStyle = 2; engine->Update(settings); WaitMs(100); shake(); WaitMs(140);
    const Window macCursor = Overlay("ClickShade pressed cursor"); XWindowAttributes macInfo{};
    Check(macCursor && XGetWindowAttributes(display, macCursor, &macInfo) && macInfo.width > static_cast<int>(nativeWidth * 2.2),
          "macOS-inspired preset uses the same native cursor and its own enlargement timing");
    Check(Until(NoCursorOverlay, 1300), "macOS-inspired preset returns within its shorter documented interval");
    settings.shakeStyle = 1; engine->Update(settings); WaitMs(100); shake(); WaitMs(220);
    settings.enabled = false; engine->Update(settings);
    Check(Until(NoCursorOverlay, 500), "master pause immediately restores the real cursor during active enlargement");
    settings.enabled = true; settings.shakeStyle = 0; settings.highlightEnabled = true;
    engine->Update(settings); WaitMs(100);

    settings.darkCursorEnabled = true; engine->Update(settings); WaitMs(40);
    const auto beforeRapid = engine->Status().inputEvents;
    const unsigned downBefore = legacyDown, upBefore = legacyUp;
    for (unsigned i = 0; i < 32; ++i) { XTestFakeButtonEvent(display, 1, True, CurrentTime); XTestFakeButtonEvent(display, 1, False, CurrentTime); }
    XFlush(display);
    Check(Until([&] { return engine->Status().inputEvents >= beforeRapid + 64 && engine->Status().heldButtons == 0 && !engine->Status().pressed; }, 3000), "rapid press/release queue leaves no stale held state");
    Check(Until([&] { return legacyDown >= downBefore + 32 && legacyUp >= upBefore + 32; }), "rapid click burst preserves every application button event");

    engine->Restore();
    Check(Until([&] { return !engine->Status().pressed && engine->Status().stateText.find("Paused") != std::string::npos && NoCursorOverlay(); }), "restore command pauses safely");
    engine->Stop();
    Check(!engine->Status().running && Overlay("ClickShade highlight", false) == None && Overlay("ClickShade pressed cursor", false) == None && Overlays("ClickShade trail", false).empty(), "stop destroys all cursor, highlight and trail windows");
    settings.enabled = true;
    Check(engine->Start(settings, message), "same engine instance can restart cleanly");
    Button(1, true);
    Check(Until([&] { return engine->Status().pressed; }), "restarted backend can invert a press");
    engine->Stop(); Button(1, false); WaitMs(50);
    Check(NoCursorOverlay() && !engine->Status().running, "stop while held restores and cleans up");
    std::puts("SKIP compositor/hardware presentation latency and actual audio audibility require a physical Linux desktop."); ++skipped;
    std::puts("SKIP general pointer confinement and XFixes-hidden-state queries are not available in the X11 protocol."); ++skipped;
    XDestroyWindow(display, window); XFreeCursor(display, original); XSync(display, False); XCloseDisplay(display);
    std::printf("\n%u passed, %u failed, %u skipped.\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
