#include "engine.h"
#include "audio.h"

#include <gio/gio.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace clickshade::linuxapp {
namespace {
constexpr const char* kBus = "org.gnome.Shell";
constexpr const char* kPath = "/org/gnome/Shell/Extensions/ClickShade";
constexpr const char* kInterface = "org.gnome.Shell.Extensions.ClickShade1";
constexpr const char* kCapabilities =
    "GNOME Wayland requires the bundled ClickShade Shell extension. Native Shell highlight and left/right/middle click effects "
    "in every style (the spotlight falls back to a glow); sampled button state can miss presses shorter than one sampling interval. "
    "Click and keyboard sounds play through this app from the extension's coarse notifications. Cursor replacement, custom cursor "
    "files, reverse/hold cursor modes, shake enlargement, trails, global shortcuts and confined-pointer detection are unavailable. "
    "This experimental compositor integration has not been runtime-validated on GNOME.";

class GnomeEngine final : public LinuxEngine {
public:
    GnomeEngine() {
        status_.backend = "GNOME Wayland · Shell extension";
        status_.stateText = "Waiting for the GNOME Shell extension";
        status_.capabilities = kCapabilities;
    }
    ~GnomeEngine() override { Stop(); }

    bool Start(const Settings& settings, std::string& error) override {
        Stop();
        error.clear();
        wakePending_.store(false);
        stopping_.store(false);
        reload_.store(true);
        restore_.store(false);
        desiredActive_.store(true);
        masterEnabled_.store(settings.enabled);
        extensionAllowed_.store(false);
        cancelled_ = g_cancellable_new();
        std::string audioError;
        audio_.Start(settings, audioError);
        audio_.SetAllowed(false);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.stateText = "Connecting to the GNOME Shell extension";
            status_.lastError.clear();
        }
        try { worker_ = std::thread([this] { Run(); }); }
        catch (const std::exception& exception) {
            error = std::string("Could not start the GNOME bridge: ") + exception.what();
            audio_.Stop();
            g_clear_object(&cancelled_);
            return false;
        }
        return true;
    }

    void Update(const Settings& settings) override {
        audio_.Update(settings);
        masterEnabled_.store(settings.enabled);
        reload_.store(true);
        restore_.store(false);
        desiredActive_.store(true);
        audio_.SetAllowed(settings.enabled && extensionAllowed_.load());
        Wake();
    }

    void Stop() override {
        stopping_.store(true);
        desiredActive_.store(false);
        extensionAllowed_.store(false);
        audio_.SetAllowed(false);
        if (cancelled_) g_cancellable_cancel(cancelled_);
        Wake();
        if (worker_.joinable()) worker_.join();
        g_clear_object(&cancelled_);
        audio_.Stop();
        std::lock_guard<std::mutex> guard(mutex_);
        status_.running = false;
        status_.stateText = "Stopped";
    }

    EngineStatus Status() const override {
        EngineStatus result;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            result = status_;
        }
        const auto audioError = audio_.LastError();
        if (!audioError.empty()) {
            if (!result.lastError.empty()) result.lastError += " ";
            result.lastError += "Sound: " + audioError;
        }
        return result;
    }

    void PreviewSound(unsigned buttons) override { audio_.Preview(buttons & 7u); }
    void PreviewKeyboardSound() override { audio_.PreviewKey(); }

    void Restore() override {
        restore_.store(true);
        reload_.store(false);
        desiredActive_.store(false);
        extensionAllowed_.store(false);
        audio_.SetAllowed(false);
        Wake();
    }

private:
    mutable std::mutex mutex_;
    EngineStatus status_;
    LinuxAudio audio_;
    std::thread worker_;
    std::atomic<bool> stopping_{true}, reload_{false}, restore_{false}, desiredActive_{true};
    std::atomic<bool> wakePending_{false};
    std::atomic<bool> masterEnabled_{false}, extensionAllowed_{false};
    GMainContext* context_ = nullptr; // Protected by mutex_; owned by worker.
    GMainLoop* loop_ = nullptr;
    GCancellable* cancelled_ = nullptr; // Start/Stop only; worker borrows.
    GDBusConnection* connection_ = nullptr;
    guint subscription_ = 0, keySubscription_ = 0, allowedSubscription_ = 0;
    bool activated_ = false;

    void Wake() {
        // A slider can change many times while a synchronous D-Bus call is in
        // flight. Work flags carry the latest state; only one source needs to
        // wait behind the current Process, including a pending Stop request.
        if (wakePending_.exchange(true, std::memory_order_acq_rel)) return;
        GMainContext* context = nullptr;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (context_) context = g_main_context_ref(context_);
        }
        if (!context) {
            wakePending_.store(false, std::memory_order_release);
            return;
        }
        g_main_context_invoke_full(context, G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            auto* self = static_cast<GnomeEngine*>(data);
            self->wakePending_.store(false, std::memory_order_release);
            self->Process();
            return G_SOURCE_REMOVE;
        }, this, nullptr);
        g_main_context_unref(context);
    }

    GVariant* Call(const char* method, GVariant* parameters = nullptr,
                   const GVariantType* resultType = nullptr, GError** error = nullptr,
                   bool shutdown = false) {
        if (!connection_) return nullptr;
        return g_dbus_connection_call_sync(connection_, kBus, kPath, kInterface, method,
            parameters, resultType, G_DBUS_CALL_FLAGS_NO_AUTO_START, shutdown ? 150 : 250,
            shutdown ? nullptr : cancelled_, error);
    }

    void SetFailure(const char* detail) {
        activated_ = false;
        extensionAllowed_.store(false);
        audio_.SetAllowed(false);
        std::lock_guard<std::mutex> guard(mutex_);
        status_.running = false;
        status_.globalSupported = false;
        status_.stateText = "GNOME extension is unavailable";
        status_.capabilities = kCapabilities;
        status_.lastError = "Install and enable clickshade@local in GNOME Shell, then choose Retry. "
            "Other Wayland compositors are not supported by this backend.";
        if (detail && *detail) status_.lastError += std::string(" Details: ") + detail;
    }

    void Connect() {
        if (connection_ || stopping_.load()) return;
        GError* error = nullptr;
        connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, cancelled_, &error);
        if (!connection_) {
            SetFailure(error ? error->message : "The session D-Bus connection is unavailable.");
            g_clear_error(&error);
            return;
        }
        // Signal callbacks run on this worker's thread-default GMainContext.
        // They only enqueue a preloaded PCM sound, never create a process.
        subscription_ = g_dbus_connection_signal_subscribe(connection_, kBus, kInterface,
            "Click", kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer data) {
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)"))) return;
                guint32 buttons = 0;
                g_variant_get(parameters, "(u)", &buttons);
                auto* self = static_cast<GnomeEngine*>(data);
                if (!self->stopping_.load()) self->audio_.Click(buttons & 7u);
            }, this, nullptr);
        // The extension only sends a coarse key category, never a key identity.
        keySubscription_ = g_dbus_connection_signal_subscribe(connection_, kBus, kInterface,
            "Key", kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer data) {
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)"))) return;
                guint32 category = 0;
                g_variant_get(parameters, "(u)", &category);
                auto* self = static_cast<GnomeEngine*>(data);
                if (!self->stopping_.load()) self->audio_.Key(category);
            }, this, nullptr);
        allowedSubscription_ = g_dbus_connection_signal_subscribe(connection_, kBus, kInterface,
            "AllowedChanged", kPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
               GVariant* parameters, gpointer data) {
                if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) return;
                gboolean allowed = FALSE;
                g_variant_get(parameters, "(b)", &allowed);
                auto* self = static_cast<GnomeEngine*>(data);
                self->extensionAllowed_.store(allowed);
                self->audio_.SetAllowed(allowed && self->masterEnabled_.load() &&
                    self->desiredActive_.load() && !self->stopping_.load());
            }, this, nullptr);
    }

    void Process() {
        if (stopping_.load()) {
            if (loop_) g_main_loop_quit(loop_);
            return;
        }
        Connect();
        if (!connection_) return;
        if (activated_ && restore_.exchange(false)) {
            GVariant* answer = Call("Restore");
            if (answer) g_variant_unref(answer);
        }
        const bool apply = reload_.exchange(false);
        if (!activated_ || apply) {
            GError* error = nullptr;
            GVariant* answer = Call("Reload", nullptr, nullptr, &error);
            if (!answer) {
                SetFailure(error ? error->message : "ClickShade is not exported by GNOME Shell.");
                g_clear_error(&error);
                return;
            }
            g_variant_unref(answer);
            answer = Call("SetActive", g_variant_new("(b)", desiredActive_.load()), nullptr, &error);
            if (!answer) {
                SetFailure(error ? error->message : "The extension could not be activated.");
                g_clear_error(&error);
                return;
            }
            g_variant_unref(answer);
            activated_ = true;
        }
        GError* error = nullptr;
        GVariant* answer = Call("GetStatus", nullptr, G_VARIANT_TYPE("(bbsss)"), &error);
        if (!answer) {
            SetFailure(error ? error->message : "The extension did not provide status.");
            g_clear_error(&error);
            return;
        }
        gboolean running = FALSE, supported = FALSE;
        const gchar *state = nullptr, *message = nullptr, *capabilities = nullptr;
        g_variant_get(answer, "(bb&s&s&s)", &running, &supported, &state, &message, &capabilities);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            status_.running = running;
            status_.globalSupported = supported;
            status_.stateText = state;
            status_.lastError = message;
            status_.capabilities = capabilities;
        }
        g_variant_unref(answer);
    }

    void Run() {
        GMainContext* context = g_main_context_new();
        g_main_context_push_thread_default(context);
        loop_ = g_main_loop_new(context, FALSE);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            context_ = context;
        }
        GSource* timer = g_timeout_source_new(1000);
        g_source_set_callback(timer, [](gpointer data) -> gboolean {
            static_cast<GnomeEngine*>(data)->Process();
            return G_SOURCE_CONTINUE;
        }, this, nullptr);
        g_source_attach(timer, context);
        Process();
        if (!stopping_.load()) g_main_loop_run(loop_);
        // Stop can arrive during the initial Process, before the main loop
        // starts and before its queued wake can run. Always deactivate here,
        // after the last activation call and before releasing the connection.
        if (connection_) {
            GVariant* answer = Call("SetActive", g_variant_new("(b)", FALSE), nullptr, nullptr, true);
            if (answer) g_variant_unref(answer);
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            context_ = nullptr;
        }
        g_source_destroy(timer);
        g_source_unref(timer);
        if (subscription_) g_dbus_connection_signal_unsubscribe(connection_, subscription_);
        if (keySubscription_) g_dbus_connection_signal_unsubscribe(connection_, keySubscription_);
        if (allowedSubscription_) g_dbus_connection_signal_unsubscribe(connection_, allowedSubscription_);
        subscription_ = keySubscription_ = allowedSubscription_ = 0;
        g_clear_object(&connection_);
        activated_ = false;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }
};
} // namespace

std::unique_ptr<LinuxEngine> CreateGnomeWaylandEngine() { return std::make_unique<GnomeEngine>(); }

} // namespace clickshade::linuxapp
