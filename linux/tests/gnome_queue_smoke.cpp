// Headless transport/queue checks against a private mock D-Bus service. This
// exercises the native bridge, not GNOME Shell or compositor compatibility.
#include "engine.h"
#include <gio/gio.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

using namespace clickshade::linuxapp;
namespace {
using TestClock = std::chrono::steady_clock;
unsigned passed = 0, failed = 0;
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (value) ++passed; else ++failed;
}
bool Until(const std::function<bool()>& condition, unsigned milliseconds = 1500) {
    const auto deadline = TestClock::now() + std::chrono::milliseconds(milliseconds);
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (TestClock::now() < deadline);
    return condition();
}

class FakeShell {
public:
    std::atomic<unsigned> statusCalls{0}, reloadCalls{0}, deactivateCalls{0};
    std::atomic<bool> active{false};

    bool Start(const char* address) {
        address_ = address;
        worker_ = std::thread([this] { Run(); });
        std::unique_lock<std::mutex> lock(mutex_);
        if (!changed_.wait_for(lock, std::chrono::seconds(2), [&] { return ready_; })) return false;
        return context_ != nullptr;
    }
    void BlockNextStatus() {
        std::lock_guard<std::mutex> lock(mutex_);
        blockNext_ = true; release_ = false;
    }
    bool WaitBlocked() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(2), [&] { return blocked_; });
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex_); release_ = true; changed_.notify_all();
    }
    void Stop() {
        Release();
        GMainContext* context = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (context_) context = g_main_context_ref(context_);
        }
        if (context) {
            g_main_context_invoke_full(context, G_PRIORITY_HIGH, [](gpointer data) -> gboolean {
                auto* self = static_cast<FakeShell*>(data);
                g_main_loop_quit(self->loop_);
                return G_SOURCE_REMOVE;
            }, this, nullptr);
            g_main_context_unref(context);
        }
        if (worker_.joinable()) worker_.join();
    }
    ~FakeShell() { Stop(); }

private:
    std::string address_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_ = false, blockNext_ = false, blocked_ = false, release_ = false;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;

    static void Method(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                       const gchar* method, GVariant* arguments,
                       GDBusMethodInvocation* invocation, gpointer data) {
        auto& self = *static_cast<FakeShell*>(data);
        if (g_str_equal(method, "GetStatus")) {
            ++self.statusCalls;
            {
                std::unique_lock<std::mutex> lock(self.mutex_);
                if (self.blockNext_) {
                    self.blockNext_ = false; self.blocked_ = true; self.changed_.notify_all();
                    self.changed_.wait(lock, [&] { return self.release_; });
                    self.blocked_ = false;
                }
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(bbsss)",
                self.active.load(), TRUE, "Mock bridge ready", "", "Mock transport only"));
            return;
        }
        if (g_str_equal(method, "SetActive")) {
            gboolean value = FALSE; g_variant_get(arguments, "(b)", &value);
            self.active.store(value);
            if (!value) ++self.deactivateCalls;
        } else if (g_str_equal(method, "Restore")) {
            self.active.store(false); ++self.deactivateCalls;
        } else if (g_str_equal(method, "Reload")) ++self.reloadCalls;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
    void Run() {
        constexpr const char* xml = R"XML(<node><interface name="org.gnome.Shell.Extensions.ClickShade1">
          <method name="Reload"/><method name="Restore"/>
          <method name="SetActive"><arg type="b" direction="in"/></method>
          <method name="GetStatus"><arg type="b" direction="out"/><arg type="b" direction="out"/>
            <arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
        </interface></node>)XML";
        GMainContext* context = g_main_context_new();
        g_main_context_push_thread_default(context);
        GError* error = nullptr;
        GDBusConnection* connection = g_dbus_connection_new_for_address_sync(address_.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, &error);
        GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(xml, nullptr);
        guint registration = 0;
        if (connection) {
            GVariant* answer = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
                "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                g_variant_new("(su)", "org.gnome.Shell", 0u), G_VARIANT_TYPE("(u)"),
                G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
            if (answer) {
                g_variant_unref(answer);
                static const GDBusInterfaceVTable vtable{Method, nullptr, nullptr, {nullptr}};
                registration = g_dbus_connection_register_object(connection,
                    "/org/gnome/Shell/Extensions/ClickShade", info->interfaces[0], &vtable,
                    this, nullptr, &error);
            }
        }
        if (registration) {
            loop_ = g_main_loop_new(context, FALSE);
            { std::lock_guard<std::mutex> lock(mutex_); context_ = context; ready_ = true; changed_.notify_all(); }
            g_main_loop_run(loop_);
            { std::lock_guard<std::mutex> lock(mutex_); context_ = nullptr; }
            g_dbus_connection_unregister_object(connection, registration);
            g_main_loop_unref(loop_); loop_ = nullptr;
        } else {
            std::lock_guard<std::mutex> lock(mutex_); ready_ = true; changed_.notify_all();
        }
        g_clear_error(&error);
        if (connection) { g_dbus_connection_close_sync(connection, nullptr, nullptr); g_object_unref(connection); }
        g_dbus_node_info_unref(info);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
};
} // namespace

int main() {
    GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    FakeShell shell;
    const bool available = shell.Start(g_test_dbus_get_bus_address(bus));
    Check(available, "private mock Shell D-Bus service starts without a desktop session");
    if (!available) { shell.Stop(); g_test_dbus_down(bus); g_object_unref(bus); return 1; }

    auto engine = CreateGnomeWaylandEngine();
    Settings settings;
    settings.highlightEnabled = false; settings.clickEffectsEnabled = false;
    std::string error;
    Check(engine->Start(settings, error) && error.empty(), "native GNOME bridge starts against the private service");
    Check(Until([&] { return engine->Status().running && shell.active.load(); }),
        "initial activation and status handshake complete");

    shell.BlockNextStatus();
    engine->Update(settings);
    const bool held = shell.WaitBlocked();
    Check(held, "mock holds one in-flight status call while settings changes arrive");
    const unsigned before = shell.statusCalls.load();
    for (unsigned i = 0; i < 1000; ++i) { settings.uiAccent = i; engine->Update(settings); }
    shell.Release();
    Check(Until([&] { return shell.statusCalls.load() > before; }), "latest settings wake is delivered after the in-flight call");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine->Stop();
    Check(shell.statusCalls.load() - before <= 4, "one thousand edits do not queue one thousand D-Bus status requests");
    Check(Until([&] { return !shell.active.load(); }), "normal Stop deactivates the service before bridge teardown");

    // Stop while the *initial* Process is still blocked, before its main loop
    // can dispatch a wake. Cleanup must still send SetActive(false).
    shell.BlockNextStatus();
    Check(engine->Start(settings, error) && shell.WaitBlocked(), "same bridge instance can restart into an in-flight initial handshake");
    const unsigned deactivatedBefore = shell.deactivateCalls.load();
    std::thread stopping([&] { engine->Stop(); });
    const bool cancelled = Until([&] { return engine->Status().stateText == "GNOME extension is unavailable"; }, 500);
    shell.Release();
    stopping.join();
    Check(cancelled && Until([&] { return shell.deactivateCalls.load() > deactivatedBefore && !shell.active.load(); }),
        "Stop during initial status cancels transport and still deactivates the extension");

    Check(engine->Start(settings, error) && Until([&] { return engine->Status().running && shell.active.load(); }),
        "restart discards stale queued wake state and activates normally");
    engine->Restore();
    Check(Until([&] { return !shell.active.load(); }), "explicit Restore remains deliverable after restart");
    engine->Stop(); engine->Stop(); engine.reset();
    Check(!shell.active.load(), "repeated Stop leaves no active service state");
    shell.Stop();
    g_test_dbus_down(bus);
    g_object_unref(bus);
    std::printf("\nGNOME bridge queue/lifecycle: %u passed, %u failed.\n", passed, failed);
    return failed ? 1 : 0;
}
