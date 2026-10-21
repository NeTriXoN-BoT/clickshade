#include "engine.h"
#include "settings.h"
#include "icon_data.h"
#include "hotkeys.h"

#include <gtk/gtk.h>
#include <X11/Xlib.h>
#undef Status
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace clickshade::linuxapp {
namespace {
constexpr double kPi = 3.14159265358979323846;
void Class(GtkWidget* widget, const char* name) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}
GtkWidget* Label(const char* text, const char* style = nullptr, bool wrap = false) {
    auto* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (wrap) {
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 68);
    }
    if (style) Class(label, style);
    return label;
}
void Pack(GtkWidget* box, GtkWidget* widget, bool expand = false) {
    gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0);
}
void Connect(gpointer widget, const char* signal, std::function<void()> function) {
    auto* callback = new std::function<void()>(std::move(function));
    g_signal_connect_data(widget, signal, G_CALLBACK(+[](gpointer, gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
    }), callback, +[](gpointer data, GClosure*) {
        delete static_cast<std::function<void()>*>(data);
    }, GConnectFlags(0));
}
std::string Hex(std::uint32_t color) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", color & 255u,
                  (color >> 8u) & 255u, (color >> 16u) & 255u);
    return text;
}
std::uint32_t Mix(std::uint32_t a, std::uint32_t b, double amount) {
    const auto channel = [&](int shift) {
        return static_cast<unsigned>(std::lround(((a >> shift) & 255u) * (1 - amount) +
                                                ((b >> shift) & 255u) * amount));
    };
    return Rgb(channel(0), channel(8), channel(16));
}
double Luminance(std::uint32_t color) {
    return .2126 * (color & 255u) + .7152 * ((color >> 8u) & 255u) +
           .0722 * ((color >> 16u) & 255u);
}
GdkRGBA Rgba(std::uint32_t value, double alpha = 1) {
    return { (value & 255u) / 255., ((value >> 8u) & 255u) / 255.,
             ((value >> 16u) & 255u) / 255., alpha };
}
void Source(cairo_t* cr, std::uint32_t color, double alpha = 1) {
    const auto c = Rgba(color, alpha);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha);
}
bool WaylandSession() {
    const char* display = std::getenv("WAYLAND_DISPLAY");
    const char* type = std::getenv("XDG_SESSION_TYPE");
    return (display && *display) || (type && std::string(type) == "wayland");
}
GdkRectangle WorkArea(GtkWidget* window = nullptr) {
    GdkRectangle area{0, 0, 1280, 960};
    auto* display = gdk_display_get_default();
    GdkMonitor* monitor = nullptr;
    if (window && gtk_widget_get_realized(window))
        monitor = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(window));
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0) monitor = gdk_display_get_monitor(display, 0);
    if (monitor) gdk_monitor_get_workarea(monitor, &area);
    return area;
}
} // namespace

class Application {
public:
    explicit Application(GtkApplication* application) : application_(application), settings_(LoadSettings()), wayland_(WaylandSession()) {}
    ~Application() {
        if (saveTimer_) g_source_remove(saveTimer_);
        if (statusTimer_) g_source_remove(statusTimer_);
        hotkeys_.Stop();
        if (engine_) engine_->Stop();
        if (tray_) g_object_unref(tray_);
        if (css_) g_object_unref(css_);
    }
    void Present(bool background = false) {
        const bool first = !window_;
        if (first) Build();
        gtk_widget_show_all(window_);
        gtk_window_present(GTK_WINDOW(window_));
        if (background || (first && settings_.startMinimized)) {
            if (tray_ && gtk_status_icon_is_embedded(tray_)) gtk_widget_hide(window_);
            else gtk_window_iconify(GTK_WINDOW(window_));
        }
        if (!statusTimer_) statusTimer_ = g_timeout_add(700, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            if (gtk_widget_get_visible(self->window_)) self->RefreshStatus();
            return G_SOURCE_CONTINUE;
        }, this);
        RefreshStatus();
    }
    void ShowExplicit() {
        const bool minimized = settings_.startMinimized;
        settings_.startMinimized = false;
        Present();
        settings_.startMinimized = minimized;
    }
    void Quit() {
        SaveNow();
        hotkeys_.Stop();
        if (engine_) engine_->Stop();
        g_application_quit(G_APPLICATION(application_));
    }
    void Restore() {
        settings_.enabled = false;
        if (engine_) engine_->Restore();
        RefreshControls();
        Changed();
    }
private:
    GtkApplication* application_{};
    GtkWidget* window_{};
    GtkWidget* stack_{};
    GtkWidget* statusText_{};
    GtkWidget* capabilityText_{};
    GtkWidget* errorText_{};
    GtkWidget* backendText_{};
    GtkWidget* savedText_{};
    GtkWidget* profileText_{};
    GtkWidget* preview_{};
    GtkStatusIcon* tray_{};
    GtkCssProvider* css_{};
    Settings settings_;
    bool wayland_ = false;
    bool refreshing_ = false;
    GdkRectangle lastWorkArea_{};
    double effectiveZoom_ = 1;
    guint saveTimer_ = 0, statusTimer_ = 0, previewTick_ = 0;
    std::string saveError_;
    std::string hotkeyEditError_;
    GlobalHotkeys hotkeys_;
    std::array<GtkWidget*, kHotkeyActionCount> hotkeyEditors_{};
    bool hotkeyDraft_ = false;
    std::unique_ptr<LinuxEngine> engine_;
    std::vector<std::function<void()>> refresh_;
    struct Pulse { double x, y; gint64 start; unsigned button; };
    std::vector<Pulse> pulses_;
    bool previewPressed_ = false;

    GtkWidget* Button(const char* text, std::function<void()> action, const char* style = nullptr) {
        auto* button = gtk_button_new_with_label(text);
        if (style) Class(button, style);
        Connect(button, "clicked", std::move(action));
        return button;
    }
    GtkWidget* Card(GtkWidget* page, const char* title, const char* description = nullptr) {
        auto* frame = gtk_frame_new(nullptr);
        gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
        Class(frame, "card");
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 13);
        gtk_container_set_border_width(GTK_CONTAINER(box), 20);
        gtk_container_add(GTK_CONTAINER(frame), box);
        Pack(box, Label(title, "card-title"));
        if (description && *description) Pack(box, Label(description, "muted", true));
        Pack(page, frame);
        return box;
    }
    GtkWidget* Row(GtkWidget* parent, const char* title, const char* description = nullptr) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        auto* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        Pack(labels, Label(title, "row-title"));
        if (description && *description) Pack(labels, Label(description, "muted small", true));
        gtk_widget_set_hexpand(labels, TRUE);
        Pack(row, labels, true);
        Pack(parent, row);
        return row;
    }
    GtkWidget* Toggle(GtkWidget* parent, const char* title, bool Settings::* field,
                      const char* description = nullptr, bool sensitive = true) {
        auto* row = Row(parent, title, description);
        auto* control = gtk_switch_new();
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_switch_set_active(GTK_SWITCH(control), settings_.*field);
        gtk_widget_set_sensitive(control, sensitive);
        Pack(row, control);
        struct Data { Application* self; bool Settings::* field; };
        auto* data = new Data{this, field};
        g_signal_connect_data(control, "notify::active", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer ptr) {
            auto* d = static_cast<Data*>(ptr);
            if (d->self->refreshing_) return;
            const bool value = gtk_switch_get_active(GTK_SWITCH(object));
            if (d->field == &Settings::startWithWindows) {
                std::string error;
                if (!ConfigureStartup(value, error)) {
                    d->self->saveError_ = error;
                    d->self->RefreshControls();
                    d->self->RefreshStatus();
                    return;
                }
            }
            d->self->settings_.*(d->field) = value;
            d->self->Changed();
        }), data, +[](gpointer ptr, GClosure*) { delete static_cast<Data*>(ptr); }, GConnectFlags(0));
        refresh_.push_back([this, control, field] { gtk_switch_set_active(GTK_SWITCH(control), settings_.*field); });
        return control;
    }
    GtkWidget* Slider(GtkWidget* parent, const char* title, int Settings::* field,
                      int minimum, int maximum, const char* unit, int step = 1) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        auto* label = Label(title);
        gtk_widget_set_size_request(label, 145, -1);
        Pack(row, label);
        auto* adjustment = gtk_adjustment_new(std::clamp(settings_.*field, minimum, maximum),
                                              minimum, maximum, step, step * 5, 0);
        auto* slider = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adjustment);
        gtk_scale_set_draw_value(GTK_SCALE(slider), FALSE);
        gtk_widget_set_hexpand(slider, TRUE);
        gtk_widget_set_size_request(slider, 120, -1);
        Pack(row, slider, true);
        auto* number = gtk_spin_button_new(adjustment, step, 0);
        gtk_entry_set_width_chars(GTK_ENTRY(number), 4);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(number), TRUE);
        Pack(row, number);
        auto* units = Label(unit, "muted small");
        gtk_widget_set_size_request(units, 25, -1);
        Pack(row, units);
        Connect(adjustment, "value-changed", [this, adjustment, field] {
            if (refreshing_) return;
            settings_.*field = static_cast<int>(std::lround(gtk_adjustment_get_value(adjustment)));
            Changed(field == &Settings::uiScalePercent);
        });
        refresh_.push_back([this, adjustment, field, minimum, maximum] {
            gtk_adjustment_set_value(adjustment, std::clamp(settings_.*field, minimum, maximum));
        });
        Pack(parent, row);
        return row;
    }
    GtkWidget* Combo(GtkWidget* parent, const char* title, int Settings::* field,
                     const std::vector<std::pair<std::string, int>>& choices) {
        auto* row = Row(parent, title);
        auto* combo = gtk_combo_box_text_new();
        for (const auto& [label, value] : choices)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), std::to_string(value).c_str(), label.c_str());
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), std::to_string(settings_.*field).c_str());
        Pack(row, combo);
        Connect(combo, "changed", [this, combo, field] {
            if (refreshing_) return;
            const char* value = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
            if (value) settings_.*field = std::atoi(value);
            Changed(field == &Settings::uiScalePercent);
        });
        refresh_.push_back([this, combo, field] {
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), std::to_string(settings_.*field).c_str());
        });
        return row;
    }
    void Color(GtkWidget* parent, const char* title, std::uint32_t Settings::* field) {
        auto* row = Row(parent, title);
        const auto color = Rgba(settings_.*field);
        auto* button = gtk_color_button_new_with_rgba(&color);
        gtk_color_button_set_title(GTK_COLOR_BUTTON(button), title);
        gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), FALSE);
        gtk_widget_set_size_request(button, 108, 36);
        Pack(row, button);
        Connect(button, "color-set", [this, button, field] {
            if (refreshing_) return;
            GdkRGBA value{};
            gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &value);
            settings_.*field = Rgb(static_cast<unsigned>(std::lround(value.red * 255)),
                                   static_cast<unsigned>(std::lround(value.green * 255)),
                                   static_cast<unsigned>(std::lround(value.blue * 255)));
            Changed(field == &Settings::uiBackground || field == &Settings::uiAccent);
        });
        refresh_.push_back([this, button, field] {
            const auto value = Rgba(settings_.*field);
            gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &value);
        });
    }
    GtkWidget* File(GtkWidget* parent, const char* title, std::string Settings::* field, bool cursor, unsigned previewMask = 0) {
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        Pack(box, Label(title, "row-title"));
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), cursor ? "No cursor file selected" : "No sound selected");
        gtk_entry_set_text(GTK_ENTRY(entry), (settings_.*field).c_str());
        gtk_widget_set_hexpand(entry, TRUE);
        Pack(row, entry, true);
        Connect(entry, "changed", [this, entry, field] {
            if (refreshing_) return;
            settings_.*field = gtk_entry_get_text(GTK_ENTRY(entry));
            Changed();
        });
        Pack(row, Button("Choose…", [this, field, cursor] {
            auto* dialog = gtk_file_chooser_dialog_new(cursor ? "Choose a static cursor" : "Choose a click sound",
                GTK_WINDOW(window_), GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
                "Choose", GTK_RESPONSE_ACCEPT, nullptr);
            auto* filter = gtk_file_filter_new();
            gtk_file_filter_set_name(filter, cursor ? "Static Windows cursor (*.cur)" : "PCM wave sound (*.wav)");
            gtk_file_filter_add_pattern(filter, cursor ? "*.cur" : "*.wav");
            gtk_file_filter_add_pattern(filter, cursor ? "*.CUR" : "*.WAV");
            gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
            if (cursor) {
                if (!(settings_.*field).empty()) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), (settings_.*field).c_str());
            } else {
                const auto folder = (std::filesystem::path(ExecutablePath()).parent_path() / "sounds").string();
                gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), folder.c_str());
            }
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) { settings_.*field = filename; g_free(filename); }
                RefreshControls();
                Changed();
            }
            gtk_widget_destroy(dialog);
        }));
        if (previewMask) Pack(row, Button("Test", [this, previewMask] {
            SaveNow();
            if (engine_) engine_->PreviewSound(previewMask);
        }));
        Pack(row, Button("Clear", [this, field] { (settings_.*field).clear(); RefreshControls(); Changed(); }));
        Pack(box, row);
        if (!cursor) SoundLibrary(box, field, entry);
        Pack(parent, box);
        refresh_.push_back([this, entry, field] { gtk_entry_set_text(GTK_ENTRY(entry), (settings_.*field).c_str()); });
        return box;
    }
    // A keyboard set is a folder (key*.wav plus optional space/enter/backspace)
    // or one WAV. The dropdown lists the set folders under sounds/keyboard.
    void KeyboardSet(GtkWidget* parent) {
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        Pack(box, Label("Keyboard sound set", "row-title"));
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "No keyboard set selected");
        gtk_entry_set_text(GTK_ENTRY(entry), settings_.keyboardSoundPath.c_str());
        gtk_widget_set_hexpand(entry, TRUE);
        Pack(row, entry, true);
        Connect(entry, "changed", [this, entry] {
            if (refreshing_) return;
            settings_.keyboardSoundPath = gtk_entry_get_text(GTK_ENTRY(entry));
            Changed();
        });
        const auto choose = [this](bool folder) {
            auto* dialog = gtk_file_chooser_dialog_new(folder ? "Choose a keyboard sound folder" : "Choose one keyboard sound",
                GTK_WINDOW(window_), folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
                "Cancel", GTK_RESPONSE_CANCEL, "Choose", GTK_RESPONSE_ACCEPT, nullptr);
            if (!folder) {
                auto* filter = gtk_file_filter_new();
                gtk_file_filter_set_name(filter, "PCM wave sound (*.wav)");
                gtk_file_filter_add_pattern(filter, "*.wav"); gtk_file_filter_add_pattern(filter, "*.WAV");
                gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
            }
            const auto start = (std::filesystem::path(ExecutablePath()).parent_path() / "sounds" / "keyboard").string();
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start.c_str());
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) { settings_.keyboardSoundPath = filename; g_free(filename); }
                RefreshControls(); Changed();
            }
            gtk_widget_destroy(dialog);
        };
        Pack(row, Button("Folder…", [choose] { choose(true); }));
        Pack(row, Button("File…", [choose] { choose(false); }));
        Pack(row, Button("Test", [this] { SaveNow(); if (engine_) engine_->PreviewKeyboardSound(); }));
        Pack(row, Button("Clear", [this] { settings_.keyboardSoundPath.clear(); RefreshControls(); Changed(); }));
        Pack(box, row);
        auto* library = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* combo = gtk_combo_box_text_new();
        gtk_widget_set_hexpand(combo, TRUE);
        auto choices = std::make_shared<std::vector<std::string>>();
        const auto select = [this, combo, choices] {
            const auto found = std::find(choices->begin(), choices->end(), settings_.keyboardSoundPath);
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), found == choices->end() ? 0 :
                static_cast<int>(std::distance(choices->begin(), found)) + 1);
        };
        const auto populate = [this, combo, choices, select] {
            const bool previous = refreshing_;
            refreshing_ = true;
            choices->clear();
            const auto folder = std::filesystem::path(ExecutablePath()).parent_path() / "sounds" / "keyboard";
            std::error_code error;
            unsigned inspected = 0;
            for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
                 !error && it != end && inspected < 512; it.increment(error), ++inspected) {
                std::error_code type;
                const auto path = it->path().string();
                if (!it->is_directory(type) || type || !g_utf8_validate(path.c_str(), -1, nullptr)) continue;
                choices->push_back(path);
                if (choices->size() == 64) break;
            }
            std::sort(choices->begin(), choices->end());
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Custom folder or file");
            for (const auto& path : *choices)
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), std::filesystem::path(path).filename().string().c_str());
            select();
            refreshing_ = previous;
        };
        populate();
        Connect(entry, "changed", [this, select] {
            if (refreshing_) return;
            refreshing_ = true; select(); refreshing_ = false;
        });
        Connect(combo, "changed", [this, combo, choices] {
            if (refreshing_) return;
            const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            if (index <= 0 || static_cast<std::size_t>(index) > choices->size()) return;
            settings_.keyboardSoundPath = (*choices)[static_cast<std::size_t>(index) - 1];
            RefreshControls(); Changed();
        });
        Pack(library, combo, true);
        Pack(library, Button("Refresh list", populate));
        Pack(box, library);
        Pack(parent, box);
        refresh_.push_back([this, entry] { gtk_entry_set_text(GTK_ENTRY(entry), settings_.keyboardSoundPath.c_str()); });
        refresh_.push_back(select);
    }
    void SoundLibrary(GtkWidget* parent, std::string Settings::* field, GtkWidget* pathEntry) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* combo = gtk_combo_box_text_new();
        gtk_widget_set_hexpand(combo, TRUE);
        auto* renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
        for (auto* item = renderers; item; item = item->next)
            if (GTK_IS_CELL_RENDERER_TEXT(item->data))
                g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, "width-chars", 26, nullptr);
        g_list_free(renderers);
        auto choices = std::make_shared<std::vector<std::string>>();
        const auto select = [this, combo, choices, field] {
            const auto found = std::find(choices->begin(), choices->end(), settings_.*field);
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), found == choices->end() ? 0 :
                static_cast<int>(std::distance(choices->begin(), found)) + 1);
        };
        const auto populate = [this, combo, choices, select] {
            const bool previous = refreshing_;
            refreshing_ = true;
            choices->clear();
            const auto folder = std::filesystem::path(ExecutablePath()).parent_path() / "sounds";
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator(folder,
                std::filesystem::directory_options::skip_permission_denied, error), end;
            unsigned inspected = 0;
            for (; !error && iterator != end && inspected < 2048; iterator.increment(error), ++inspected) {
                std::error_code typeError;
                if (iterator.depth() >= 2) iterator.disable_recursion_pending();
                if (!iterator->is_regular_file(typeError) || typeError) continue;
                std::string extension = iterator->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c);
                });
                const auto path = iterator->path().string();
                if (extension != ".wav" || !g_utf8_validate(path.c_str(), -1, nullptr)) continue;
                choices->push_back(path);
                if (choices->size() == 256) break;
            }
            std::sort(choices->begin(), choices->end());
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "Custom file / choose a sound");
            for (const auto& path : *choices)
                gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), std::filesystem::path(path).lexically_relative(folder).string().c_str());
            select();
            refreshing_ = previous;
        };
        populate();
        Connect(pathEntry, "changed", [this, select] {
            if (refreshing_) return;
            refreshing_ = true; select(); refreshing_ = false;
        });
        Connect(combo, "changed", [this, combo, choices, field] {
            if (refreshing_) return;
            const int index = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
            if (index <= 0 || static_cast<std::size_t>(index) > choices->size()) return;
            settings_.*field = (*choices)[static_cast<std::size_t>(index) - 1];
            RefreshControls();
            Changed();
        });
        Pack(row, combo, true);
        Pack(row, Button("Refresh list", populate));
        Pack(parent, row);
        refresh_.push_back(select);
    }
    GtkWidget* Page(const char* id, const char* title, const char* subtitle) {
        auto* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        // External policies preserve wheel, keyboard and focus scrolling while
        // keeping scrollbar chrome out of the designed interface. Horizontal
        // scrolling also lets exceptionally narrow work areas retain controls.
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
        auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(page), 24);
        Pack(page, Label(title, "page-title"));
        Pack(page, Label(subtitle, "muted", true));
        gtk_container_add(GTK_CONTAINER(scroller), page);
        gtk_stack_add_titled(GTK_STACK(stack_), scroller, id, title);
        return page;
    }
    void ApplicationList(GtkWidget* parent, const char* title, std::string Settings::* field) {
        Pack(parent, Label(title, "row-title"));
        auto* entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "application-name; another-app");
        gtk_entry_set_text(GTK_ENTRY(entry), (settings_.*field).c_str());
        Pack(parent, entry);
        Connect(entry, "changed", [this, entry, field] {
            if (refreshing_) return;
            settings_.*field = gtk_entry_get_text(GTK_ENTRY(entry)); Changed();
        });
        auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        Pack(actions, Button("Add app…", [this, field] {
            auto* dialog = gtk_file_chooser_dialog_new("Choose an application executable", GTK_WINDOW(window_),
                GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL, "Add app", GTK_RESPONSE_ACCEPT, nullptr);
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), "/usr/bin");
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename) {
                    const auto name = std::filesystem::path(filename).filename().string();
                    g_free(filename);
                    if (!name.empty() && name.find(';') == std::string::npos) {
                        if (!(settings_.*field).empty()) settings_.*field += "; ";
                        settings_.*field += name;
                        RefreshControls(); Changed();
                    }
                }
            }
            gtk_widget_destroy(dialog);
        }));
        Pack(actions, Button("Clear", [this, field] { (settings_.*field).clear(); RefreshControls(); Changed(); }));
        Pack(parent, actions);
        refresh_.push_back([this, entry, field] { gtk_entry_set_text(GTK_ENTRY(entry), (settings_.*field).c_str()); });
    }
    void BuildCursor() {
        auto* page = Page("cursor", "Cursor and rules", "Press, release, and keep effects out of the applications that need your full attention.");
        auto* cursor = Card(page, "Click behavior", wayland_
            ? "GNOME Wayland uses the companion extension for highlights and click effects. Global cursor replacement is not available."
            : "X11 can invert the current pointer while pressed using a temporary software cursor. A compositing desktop is required.");
        Toggle(cursor, "Darken the pointer on click", &Settings::darkCursorEnabled, nullptr, !wayland_);
        Toggle(cursor, "Stay dark while held", &Settings::cursorHold, "Turn off for a brief flash after each press.", !wayland_);
        auto* flash = Slider(cursor, "Flash duration", &Settings::cursorFlashMs, 50, 500, "ms", 10);
        gtk_widget_set_sensitive(flash, !wayland_);
        auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        Pack(buttons, Label("Trigger buttons", "muted"));
        for (const auto& [name, mask] : std::vector<std::pair<const char*, unsigned>>{{"Left", 1}, {"Right", 2}, {"Middle", 4}, {"Side 1", 8}, {"Side 2", 16}}) {
            auto* check = gtk_check_button_new_with_label(name);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), (settings_.buttonMask & mask) != 0);
            Pack(buttons, check);
            Connect(check, "toggled", [this, check, mask] {
                if (refreshing_) return;
                if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) settings_.buttonMask |= mask;
                else settings_.buttonMask &= ~mask;
                Changed();
            });
            refresh_.push_back([this, check, mask] { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), (settings_.buttonMask & mask) != 0); });
        }
        gtk_widget_set_sensitive(buttons, !wayland_);
        Pack(cursor, buttons);

        auto* shake = Card(page, "Shake to find the pointer", wayland_
            ? "Shake magnification is unavailable in this GNOME companion. Use your compositor's own locator if available; these preferences are retained."
            : "Quick back-and-forth motion briefly enlarges the pointer. Plasma-style follows published behavior with a bounded size; macOS-inspired is an approximation. Dragging does not trigger it.");
        auto* shakeControls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        Pack(shake, shakeControls);
        Combo(shakeControls, "Behavior", &Settings::shakeStyle,
            {{"Off", 0}, {"Plasma-style", 1}, {"macOS-inspired", 2}});
        Slider(shakeControls, "Sensitivity", &Settings::shakeSensitivity, 50, 200, "%");
        Slider(shakeControls, "Maximum size", &Settings::shakeMaximumScale, 150, 400, "%", 10);
        gtk_widget_set_sensitive(shakeControls, !wayland_);

        auto* rules = Card(page, "Automatic pause", "The master switch pauses every effect. Fullscreen and excluded applications also pause highlights and sounds.");
        Toggle(rules, "Pause for fullscreen applications", &Settings::pauseFullscreen);
        Toggle(rules, "Pause when the pointer is confined", &Settings::pauseConfinedCursor,
               "Saved preference. Reliable confinement detection is unavailable in this Linux build.", false);
        ApplicationList(rules, "Excluded applications", &Settings::excludedProcesses);
        ApplicationList(rules, "Always allow in these apps", &Settings::allowedProcesses);
        Pack(rules, Label("Separate executable basenames or window classes with semicolons. GNOME also accepts application IDs.", "muted small", true));
        Pack(rules, Label("Allowed apps bypass fullscreen pause. Exclusions still win; locking the session or hiding the pointer still pauses effects.", "muted small", true));

        auto* profiles = Card(page, "Cursor profiles", "These settings are retained for compatibility with the Windows build. Linux currently supports the installed theme at its existing size; profile replacement and reverse mode are unavailable.");
        auto* profileControls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        Pack(profiles, profileControls);
        Combo(profileControls, "Released / pressed artwork", &Settings::cursorStyle,
              {{"Current desktop theme", 0}, {"Windows default arrow", 1}, {"Classic-style arrow", 2}, {"Custom arrow pair", 3}});
        Toggle(profileControls, "Reverse: dark at rest", &Settings::cursorReverse);
        Combo(profileControls, "Cursor image size", &Settings::cursorSize,
              {{"Keep source size", 0}, {"24 px", 24}, {"32 px", 32}, {"48 px", 48}, {"64 px", 64}, {"96 px", 96}, {"128 px", 128}});
        File(profileControls, "Released cursor", &Settings::releasedCursorPath, true);
        File(profileControls, "Pressed cursor • empty means invert", &Settings::pressedCursorPath, true);
        gtk_widget_set_sensitive(profileControls, FALSE);

        auto* startup = Card(page, "On your desktop");
        Toggle(startup, "Start when I sign in", &Settings::startWithWindows, "Adds a per-user XDG autostart entry for this executable.");
        Toggle(startup, "Open settings minimized", &Settings::startMinimized,
               "Uses the tray where supported; otherwise keeps a minimized window in the taskbar.");
    }
    void BuildHighlights() {
        auto* page = Page("highlights", "Highlights", "A steady halo, a trail if you like, and clear left, right and middle clicks.");
        auto* previewCard = Card(page, "Try your effects", "Click the preview with either mouse button. This local preview also works when global desktop effects are unavailable.");
        preview_ = gtk_drawing_area_new();
        gtk_widget_set_size_request(preview_, -1, 156);
        gtk_widget_add_events(preview_, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
        g_signal_connect(preview_, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
            static_cast<Application*>(data)->DrawPreview(widget, cr); return FALSE;
        }), this);
        g_signal_connect(preview_, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* event, gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            const unsigned bit = event->button == 1 ? 1u : event->button == 3 ? 2u : event->button == 2 ? 4u : 0u;
            if (!bit) return FALSE;
            self->previewPressed_ = true;
            if (self->pulses_.size() == 8) self->pulses_.erase(self->pulses_.begin());
            self->pulses_.push_back({event->x, event->y, g_get_monotonic_time(), bit});
            self->StartPreviewAnimation();
            return TRUE;
        }), this);
        g_signal_connect(preview_, "button-release-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventButton*, gpointer data) -> gboolean {
            static_cast<Application*>(data)->previewPressed_ = false;
            gtk_widget_queue_draw(widget); return TRUE;
        }), this);
        Pack(previewCard, preview_);
        auto* halo = Card(page, "Pointer highlight", wayland_
            ? "The GNOME companion draws every style except the spotlight, which falls back to a glow."
            : "The spotlight dims the pointer's monitor around a soft circle; the other styles stay small and cheap.");
        Toggle(halo, "Show a persistent highlight", &Settings::highlightEnabled);
        Combo(halo, "Style", &Settings::highlightStyle,
              {{"Filled circle", kHighlightFilled}, {"Ring", kHighlightRing}, {"Soft glow", kHighlightGlow},
               {"Crosshair", kHighlightCrosshair}, {"Spotlight", kHighlightSpotlight}});
        Slider(halo, "Radius", &Settings::highlightRadius, 8, 100, "px");
        Slider(halo, "Opacity", &Settings::highlightOpacity, 5, 100, "%");
        Slider(halo, "Outline width", &Settings::highlightThickness, 1, 12, "px");
        Color(halo, "Highlight color", &Settings::highlightColor);
        Toggle(halo, "Breathing", &Settings::highlightBreathing, "Slowly swells and fades; not used by the spotlight.");
        Combo(halo, "Hide when the pointer rests", &Settings::highlightIdleHideMs,
              {{"Never", 0}, {"After 1 s", 1000}, {"After 2 s", 2000}, {"After 3 s", 3000}, {"After 5 s", 5000}, {"After 10 s", 10000}});
        Slider(halo, "Spotlight dimming", &Settings::spotlightDim, 10, 90, "%");
        Slider(halo, "Spotlight radius", &Settings::spotlightRadius, 40, 400, "px", 10);

        auto* trail = Card(page, "Pointer trail", wayland_
            ? "Pointer trails are unavailable in the GNOME Wayland companion. Your preferences are retained."
            : "Leave a short fading trail behind the pointer. This effect is off by default.");
        auto* trailControls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        Pack(trail, trailControls);
        Toggle(trailControls, "Show a pointer trail", &Settings::trailEnabled);
        Combo(trailControls, "Trail style", &Settings::trailStyle, {{"Dots", 0}, {"Rings", 1}, {"Sparkles", 2}, {"Comet", 3}, {"Fine sparkle", 4}});
        Slider(trailControls, "Size", &Settings::trailRadius, 2, 16, "px");
        Slider(trailControls, "Opacity", &Settings::trailOpacity, 5, 100, "%");
        Slider(trailControls, "Fade time", &Settings::trailDurationMs, 80, 700, "ms", 10);
        Color(trailControls, "Trail color", &Settings::trailColor);
        gtk_widget_set_sensitive(trailControls, !wayland_);

        auto* clicks = Card(page, "Click effects", "Left, right and middle clicks have independent colors. Effects use their original press positions.");
        Toggle(clicks, "Animate clicks", &Settings::clickEffectsEnabled);
        Toggle(clicks, "Show left clicks", &Settings::leftClickEnabled);
        Toggle(clicks, "Show right clicks", &Settings::rightClickEnabled);
        Toggle(clicks, "Show middle clicks", &Settings::middleClickEnabled, "Pressing the wheel, not scrolling it.");
        Combo(clicks, "Style", &Settings::clickStyle,
              {{"Ring", kClickRing}, {"Pulse", kClickPulse}, {"Ripple", kClickRipple}, {"Burst", kClickBurst}, {"Neon ring", kClickNeon},
               {"Target", kClickTarget}});
        Slider(clicks, "Radius", &Settings::clickRadius, 12, 120, "px");
        Slider(clicks, "Duration", &Settings::clickDurationMs, 100, 1500, "ms", 10);
        Slider(clicks, "Opacity", &Settings::clickOpacity, 5, 100, "%");
        Slider(clicks, "Outline width", &Settings::clickThickness, 1, 12, "px");
        Combo(clicks, "Animation easing", &Settings::clickEasing,
              {{"Linear", 0}, {"Ease out", 1}, {"Smooth step", 2}});
        Color(clicks, "Left click color", &Settings::leftClickColor);
        Color(clicks, "Right click color", &Settings::rightClickColor);
        Color(clicks, "Middle click color", &Settings::middleClickColor);
    }
    void BuildSounds() {
        auto* page = Page("sounds", "Sounds", "Choose a sound for each mouse button. The start is cached; longer WAV files stream in the background.");
        auto* sounds = Card(page, "Custom click sounds", "Linux supports PCM WAV files and a PulseAudio-compatible server, including PipeWire's PulseAudio service. Playback depends on the selected session backend.");
        Toggle(sounds, "Play click sounds", &Settings::clickSoundsEnabled);
        File(sounds, "Left click", &Settings::leftSoundPath, false, 1);
        File(sounds, "Right click", &Settings::rightSoundPath, false, 2);
        File(sounds, "Middle click", &Settings::middleSoundPath, false, 4);
        Slider(sounds, "Volume", &Settings::soundVolume, 0, 100, "%");
        Pack(sounds, Label("The lists show WAV files in the sounds folder beside ClickShade. Add your own files there and refresh the list, or choose a file elsewhere. Test does not enable click sounds; middle-click means pressing the wheel, not scrolling it.", "muted small", true));
        auto* keyboard = Card(page, "Keyboard sounds", wayland_
            ? "The GNOME companion reports only whether a key, the space bar, Enter or Backspace was pressed. Nothing is recorded."
            : "Typing plays a key sound from the chosen set. ClickShade only looks at whether a key, the space bar, Enter or Backspace went down, and only while this is on. Nothing is recorded.");
        Toggle(keyboard, "Play keyboard sounds", &Settings::keyboardSoundsEnabled);
        KeyboardSet(keyboard);
        Slider(keyboard, "Keyboard volume", &Settings::keyboardVolume, 0, 100, "%");
        Pack(keyboard, Label("A set folder holds key-1.wav to key-6.wav plus optional space.wav, enter.wav and backspace.wav. Variants are shuffled so fast typing never repeats one sample.", "muted small", true));
        auto* notes = Card(page, "A few good defaults");
        Pack(notes, Label("A soft click at low volume is easier to listen to during a long recording. The example WAV files in the release are original sounds you can try: the classic click and mechanical, pop, laser and retro sets, plus classic, mechanical, laptop and typewriter keyboards.", "muted", true));
    }
    void BuildPresenter() {
        auto* page = Page("presenter", "Presenter", "A magnifier lens beside the pointer and an on-screen display of the keys you press.");
        auto* lens = Card(page, "Screen magnifier", wayland_
            ? "The GNOME companion draws a square lens beside the pointer from the compositor's own window layer. ClickShade's highlights are not part of it."
            : "A live lens beside the pointer, read from the screen about thirty times a second. It needs a compositing window manager and a 24- or 32-bit display.");
        Toggle(lens, "Show a magnifier lens", &Settings::magnifierEnabled);
        Combo(lens, "Shape", &Settings::magnifierShape, {{"Round", 0}, {"Rounded square", 1}});
        Slider(lens, "Zoom", &Settings::magnifierZoom, 125, 800, "%", 25);
        Slider(lens, "Lens size", &Settings::magnifierSize, 80, 600, "px", 10);
        Toggle(lens, "Draw a frame around the lens", &Settings::magnifierFrame);
        Color(lens, "Frame color", &Settings::magnifierBorderColor);
        Pack(lens, Label(wayland_ ? "GNOME always draws the lens as a rounded square beside the pointer; the shape setting is kept for Windows and X11."
                                  : "On Linux the lens always sits beside the pointer, because a lens centered on it would capture itself. It follows the pause rules like every other effect, but stays put while an editor hides the cursor as you type.", "muted small", true));
        auto* keys = Card(page, "Key display", wayland_
            ? "The GNOME companion shows pressed keys as keycaps inside the Shell. Keyboard input is observed only while this switch (or keyboard sounds) is on."
            : "Shortcuts appear as keycaps on the pointer's monitor and fade after the hold time; with Every key, typed letters join into words. Keyboard input is subscribed only while this switch (or keyboard sounds) is on.");
        Toggle(keys, "Show the keys I press on screen", &Settings::keyDisplayEnabled);
        Combo(keys, "What to show", &Settings::keyDisplayMode, {{"Shortcuts only", 0}, {"Every key", 1}});
        Toggle(keys, "Also show mouse buttons", &Settings::keyDisplayMouse);
        Combo(keys, "Position", &Settings::keyDisplayPosition,
              {{"Bottom center", 0}, {"Top center", 1}, {"Bottom left", 2}, {"Bottom right", 3}});
        Slider(keys, "Text size", &Settings::keyDisplaySize, 60, 250, "%", 5);
        Slider(keys, "Hold time", &Settings::keyDisplayDurationMs, 300, 6000, "ms", 100);
        Slider(keys, "Opacity", &Settings::keyDisplayOpacity, 20, 100, "%");
        Color(keys, "Backdrop color", &Settings::keyDisplayColor);
        Pack(keys, Label("Shortcuts only shows combinations with Ctrl, Alt or Super plus function keys, so typed text never appears. A key lives in memory only while its row is on screen; nothing is recorded, stored or sent. The display keeps working while an editor hides the pointer; excluded apps, fullscreen rules and the lock screen still pause it.", "muted small", true));
    }
    void BuildAppearance() {
        auto* page = Page("appearance", "Appearance", "Make ClickShade feel at home. Interface colors are independent of your pointer effects.");
        auto* theme = Card(page, "Your palette", "Choose a starting point, then change the background and accent to suit your desktop.");
        auto* modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        auto* dark = gtk_radio_button_new_with_label(nullptr, "Dark");
        auto* light = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(dark), "Light");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(dark), FALSE);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(light), FALSE);
        Pack(modes, dark, true); Pack(modes, light, true); Pack(theme, modes);
        const auto mode = [this](bool darkMode) {
            if (refreshing_ || darkMode == settings_.uiDarkMode) return;
            NormalizeSettings(settings_);
            settings_.uiDarkMode = darkMode;
            settings_.uiBackground = darkMode ? settings_.uiDarkBackground : settings_.uiLightBackground;
            settings_.uiAccent = darkMode ? settings_.uiDarkAccent : settings_.uiLightAccent;
            RefreshControls(); Changed(true);
        };
        Connect(dark, "toggled", [dark, mode] { if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dark))) mode(true); });
        Connect(light, "toggled", [light, mode] { if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(light))) mode(false); });
        refresh_.push_back([this, dark, light] {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark), settings_.uiDarkMode);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(light), !settings_.uiDarkMode);
        });
        Pack(theme, Label("Dark and light each remember their own background and accent.", "muted small", true));
        auto* presets = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        const auto preset = [this, presets](const char* name, bool darkMode, std::uint32_t background, std::uint32_t accent) {
            Pack(presets, Button(name, [this, darkMode, background, accent] {
                // Preserve this mode's custom pair before selecting a preset
                // in the other mode; Daylight must not overwrite Dark colors.
                NormalizeSettings(settings_);
                settings_.uiDarkMode = darkMode;
                settings_.uiBackground = background; settings_.uiAccent = accent;
                RefreshControls(); Changed(true);
            }), true);
        };
        preset("Midnight", true, Rgb(17, 21, 28), Rgb(120, 226, 194));
        preset("Ocean", true, Rgb(15, 23, 38), Rgb(104, 178, 255));
        preset("Rose", true, Rgb(28, 18, 29), Rgb(247, 145, 183));
        preset("Daylight", false, Rgb(244, 246, 249), Rgb(21, 122, 103));
        Pack(theme, presets);
        Color(theme, "Background", &Settings::uiBackground);
        Color(theme, "Accent", &Settings::uiAccent);
        auto* scale = Card(page, "Comfortable on every display", "GTK follows the desktop's display scale automatically. Add interface zoom if you prefer larger controls. Zoom is limited when needed to fit the display; pages remain scrollable.");
        Combo(scale, "Interface zoom", &Settings::uiScalePercent,
              {{"Automatic", 0}, {"100%", 100}, {"125%", 125}, {"150%", 150}, {"175%", 175}, {"200%", 200}});
        auto* timing = Card(page, "Frame pacing", wayland_
            ? "Auto follows GNOME Shell's display timing. Shell controls composition; this backend does not use workload adaptation. Manual sets a sampling limit."
            : "Auto adapts click and trail animation to the time ClickShade spends drawing, up to your display's refresh rate, while the highlight, spotlight and magnifier keep following the pointer at the display limit. A manual limit caps all of them. No hardware profiling or benchmark runs.");
        auto* paceRow = Row(timing, "Frame limit");
        auto* paceMode = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(paceMode), "auto", wayland_ ? "Auto (compositor)" : "Auto (adaptive)");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(paceMode), "manual", "Manual");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(paceMode), settings_.visualRefreshRate ? "manual" : "auto");
        Pack(paceRow, paceMode);
        auto* paceLimit = Slider(timing, "Manual limit", &Settings::visualRefreshRate, 30, 360, "fps");
        gtk_widget_set_sensitive(paceLimit, settings_.visualRefreshRate != 0);
        auto paceManual = std::make_shared<int>(settings_.visualRefreshRate ? settings_.visualRefreshRate : 60);
        Connect(paceMode, "changed", [this, paceMode, paceManual] {
            if (refreshing_) return;
            const char* selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(paceMode));
            if (!selected) return;
            if (settings_.visualRefreshRate) *paceManual = settings_.visualRefreshRate;
            settings_.visualRefreshRate = std::string(selected) == "manual" ? *paceManual : 0;
            RefreshControls();
            Changed();
        });
        refresh_.push_back([this, paceMode, paceLimit, paceManual] {
            if (settings_.visualRefreshRate) *paceManual = settings_.visualRefreshRate;
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(paceMode), settings_.visualRefreshRate ? "manual" : "auto");
            gtk_widget_set_sensitive(paceLimit, settings_.visualRefreshRate != 0);
        });
        Pack(timing, Label("The limit changes effects only. It does not change mouse movement or your monitor's refresh rate. Actual frame delivery depends on the desktop and system load.", "muted small", true));
        auto* shortcuts = Card(page, "Keyboard shortcuts", wayland_
            ? "Global shortcuts are unavailable on Wayland in this version. Use the settings window or tray controls. Saved assignments are retained."
            : "Enter a combination such as Ctrl+Shift+F10, then choose Apply shortcuts. Include Ctrl, Alt, or Win/Super. Clear leaves an action unassigned when applied.");
        auto* shortcutControls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        Pack(shortcuts, shortcutControls);
        constexpr std::array<const char*, kHotkeyActionCount> names{{"Toggle all effects", "Toggle pointer highlight", "Toggle click effects", "Toggle click sounds", "Show settings", "Pause and restore cursor", "Toggle keyboard sounds", "Toggle magnifier", "Toggle key display"}};
        const auto fields = HotkeyFields();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            Pack(box, Label(names[i]));
            auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            auto* entry = gtk_entry_new(); hotkeyEditors_[i] = entry;
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Not assigned");
            gtk_entry_set_max_length(GTK_ENTRY(entry), 80);
            gtk_widget_set_hexpand(entry, TRUE);
            Pack(row, entry, true);
            Pack(row, Button("Clear", [entry] { gtk_entry_set_text(GTK_ENTRY(entry), ""); }));
            Connect(entry, "changed", [this] {
                if (refreshing_) return;
                hotkeyDraft_ = true;
                if (savedText_) gtk_label_set_text(GTK_LABEL(savedText_), "Apply shortcuts to save them");
            });
            Connect(entry, "activate", [this] { ApplyHotkeyEdits(); });
            const auto field = fields[i];
            refresh_.push_back([this, entry, field] {
                if (!hotkeyDraft_) gtk_entry_set_text(GTK_ENTRY(entry), (settings_.*field).c_str());
            });
            Pack(box, row); Pack(shortcutControls, box);
        }
        Pack(shortcutControls, Button("Apply shortcuts", [this] { ApplyHotkeyEdits(); }));
        gtk_widget_set_sensitive(shortcutControls, !wayland_);
        auto* session = Card(page, "This session", "Linux support depends on the display server and desktop compositor.");
        backendText_ = Label("Starting…", "row-title", true);
        capabilityText_ = Label("", "muted", true);
        Pack(session, backendText_);
        Pack(session, capabilityText_);
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        Pack(row, Button("Retry desktop connection", [this] { RestartEngine(); }));
        Pack(row, Button("Open settings folder", [this] {
            const auto parent = std::filesystem::path(SettingsPath()).parent_path().string();
            GError* error = nullptr;
            char* uri = g_filename_to_uri(parent.c_str(), nullptr, &error);
            if (uri) { gtk_show_uri_on_window(GTK_WINDOW(window_), uri, GDK_CURRENT_TIME, &error); g_free(uri); }
            if (error) { saveError_ = error->message; g_error_free(error); RefreshStatus(); }
        }));
        Pack(session, row);
        auto* profiles = Card(page, "Settings profiles", "Export or back up every Linux preference, then import a saved profile when needed. File paths may need to be selected again on another computer. Import keeps your current start-at-sign-in choice.");
        auto* profileActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        Pack(profileActions, Button("Export…", [this] { ExportProfile(); }), true);
        Pack(profileActions, Button("Backup current", [this] { BackupProfile(); }), true);
        Pack(profileActions, Button("Import…", [this] { ImportProfile(); }), true);
        Pack(profiles, profileActions);
        profileText_ = Label("A single before-import backup protects the previous preferences.", "muted small", true);
        Pack(profiles, profileText_);
        auto* reset = Card(page, "Restore all defaults", "Reset every preference, including startup, selected files, exclusions, colors, and interface zoom.");
        Pack(reset, Button("Restore all defaults…", [this] { RestoreAllDefaults(); }));
    }
    void Build() {
        window_ = gtk_application_window_new(application_);
        gtk_window_set_title(GTK_WINDOW(window_), "ClickShade");
        auto* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
        GdkPixbuf* icon = nullptr;
        if (loader && gdk_pixbuf_loader_write(loader, kIconPng, sizeof(kIconPng), nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr)) {
            icon = gdk_pixbuf_loader_get_pixbuf(loader);
            if (icon) gtk_window_set_icon(GTK_WINDOW(window_), icon);
        }
        const auto workarea = WorkArea();
        // GTK's requested content size excludes some titlebar/decorations.
        // Reserve logical space for those on short / high-scale work areas.
        gtk_window_set_default_size(GTK_WINDOW(window_), std::min(880, workarea.width - 36), std::min(900, workarea.height - 80));
        gtk_widget_set_size_request(window_, std::min(700, workarea.width - 24), std::min(540, workarea.height - 80));
        Class(window_, "clickshade");
        auto* header = gtk_header_bar_new();
        gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header), "ClickShade");
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header), "Linux · 1.0");
        gtk_window_set_titlebar(GTK_WINDOW(window_), header);
        auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(window_), content);
        auto* hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_container_set_border_width(GTK_CONTAINER(hero), 20);
        auto* brand = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
        Pack(brand, Label("ClickShade", "brand"));
        Pack(brand, Label("Make every click clear.", "muted"));
        Pack(hero, brand, true);
        auto* master = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
        Toggle(master, "Effects active", &Settings::enabled);
        statusText_ = Label("Starting desktop connection…", "muted small", true);
        gtk_label_set_max_width_chars(GTK_LABEL(statusText_), 36);
        Pack(master, statusText_);
        Pack(hero, master);
        Pack(content, hero);
        auto* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        Pack(content, separator);
        stack_ = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(stack_), 100);
        gtk_stack_set_homogeneous(GTK_STACK(stack_), FALSE);
        auto* switcher = gtk_stack_switcher_new();
        gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack_));
        gtk_widget_set_halign(switcher, GTK_ALIGN_FILL);
        gtk_widget_set_margin_start(switcher, 24);
        gtk_widget_set_margin_end(switcher, 24);
        gtk_widget_set_margin_top(switcher, 16);
        Class(switcher, "page-tabs");
        Pack(content, switcher);
        Pack(content, stack_, true);
        BuildCursor(); BuildHighlights(); BuildSounds(); BuildPresenter(); BuildAppearance();
        gtk_box_set_homogeneous(GTK_BOX(switcher), TRUE);
        gtk_container_foreach(GTK_CONTAINER(switcher), +[](GtkWidget* child, gpointer) {
            auto* label = GTK_IS_BIN(child) ? gtk_bin_get_child(GTK_BIN(child)) : nullptr;
            if (label && GTK_IS_LABEL(label)) {
                gtk_widget_set_tooltip_text(child, gtk_label_get_text(GTK_LABEL(label)));
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            }
        }, nullptr);
        errorText_ = Label("", "error small", true);
        gtk_widget_set_margin_start(errorText_, 24);
        gtk_widget_set_margin_end(errorText_, 24);
        Pack(content, errorText_);
        auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_container_set_border_width(GTK_CONTAINER(footer), 20);
        savedText_ = Label("Changes save automatically", "muted small");
        gtk_label_set_ellipsize(GTK_LABEL(savedText_), PANGO_ELLIPSIZE_END);
        Pack(footer, savedText_, true);
        Pack(footer, Button("Pause and restore", [this] { Restore(); }));
        Pack(footer, Button("Quit", [this] { Quit(); }));
        Pack(content, footer);
        g_signal_connect(window_, "delete-event", G_CALLBACK(+[](GtkWidget* widget, GdkEvent*, gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            self->SaveNow();
            if (self->tray_ && gtk_status_icon_is_embedded(self->tray_)) gtk_widget_hide(widget);
            else gtk_window_iconify(GTK_WINDOW(widget));
            return TRUE;
        }), this);
        g_signal_connect(window_, "configure-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventConfigure*, gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            const auto area = WorkArea(widget);
            if (area.width != self->lastWorkArea_.width || area.height != self->lastWorkArea_.height)
                self->ApplyTheme();
            return FALSE;
        }), this);
        g_signal_connect(window_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            static_cast<Application*>(data)->ApplyTheme();
        }), this);
        auto* quitAction = g_simple_action_new("quit", nullptr);
        g_signal_connect(quitAction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
            static_cast<Application*>(data)->Quit();
        }), this);
        g_action_map_add_action(G_ACTION_MAP(application_), G_ACTION(quitAction));
        g_object_unref(quitAction);
        const gchar* quitAccelerators[] = {"<Primary>q", nullptr};
        gtk_application_set_accels_for_action(application_, "app.quit", quitAccelerators);
        tray_ = icon ? gtk_status_icon_new_from_pixbuf(icon) : gtk_status_icon_new_from_icon_name("input-mouse");
        if (loader) g_object_unref(loader);
        gtk_status_icon_set_title(tray_, "ClickShade");
        gtk_status_icon_set_tooltip_text(tray_, "ClickShade — pointer effects");
        g_signal_connect(tray_, "activate", G_CALLBACK(+[](GtkStatusIcon*, gpointer data) { static_cast<Application*>(data)->ShowExplicit(); }), this);
        g_signal_connect(tray_, "popup-menu", G_CALLBACK(+[](GtkStatusIcon*, guint button, guint time, gpointer data) {
            auto* self = static_cast<Application*>(data);
            auto* menu = gtk_menu_new();
            auto* show = gtk_menu_item_new_with_label("Show settings");
            auto* pause = gtk_menu_item_new_with_label(self->settings_.enabled ? "Pause effects" : "Resume effects");
            auto* quit = gtk_menu_item_new_with_label("Quit ClickShade");
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), show);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), pause);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
            Connect(show, "activate", [self] { self->ShowExplicit(); });
            Connect(pause, "activate", [self] { self->settings_.enabled = !self->settings_.enabled; self->RefreshControls(); self->Changed(); });
            Connect(quit, "activate", [self] { self->Quit(); });
            g_signal_connect_swapped(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), menu);
            gtk_widget_show_all(menu);
            gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr, gtk_status_icon_position_menu, self->tray_, button, time);
        }), this);
        css_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        ApplyTheme();
        RestartEngine();
        RefreshControls();
        RestartHotkeys();
    }
    static std::array<std::string Settings::*, kHotkeyActionCount> HotkeyFields() {
        return {{&Settings::hotkeyToggleAll, &Settings::hotkeyHighlight, &Settings::hotkeyClicks, &Settings::hotkeySounds,
                 &Settings::hotkeySettings, &Settings::hotkeyRestore, &Settings::hotkeyKeyboardSounds,
                 &Settings::hotkeyMagnifier, &Settings::hotkeyKeyDisplay}};
    }
    void Hotkey(HotkeyAction action) {
        switch (action) {
        case HotkeyAction::ToggleAll: settings_.enabled = !settings_.enabled; break;
        case HotkeyAction::ToggleHighlight: settings_.highlightEnabled = !settings_.highlightEnabled; break;
        case HotkeyAction::ToggleClicks: settings_.clickEffectsEnabled = !settings_.clickEffectsEnabled; break;
        case HotkeyAction::ToggleSounds: settings_.clickSoundsEnabled = !settings_.clickSoundsEnabled; break;
        case HotkeyAction::ToggleKeyboardSounds: settings_.keyboardSoundsEnabled = !settings_.keyboardSoundsEnabled; break;
        case HotkeyAction::ToggleMagnifier: settings_.magnifierEnabled = !settings_.magnifierEnabled; break;
        case HotkeyAction::ToggleKeyDisplay: settings_.keyDisplayEnabled = !settings_.keyDisplayEnabled; break;
        case HotkeyAction::ShowSettings: ShowExplicit(); return;
        case HotkeyAction::RestorePause: Restore(); return;
        }
        RefreshControls(); Changed();
    }
    void RestartHotkeys() {
        hotkeys_.Stop(); hotkeyEditError_.clear();
        if (!wayland_) {
            std::string error;
            hotkeys_.Start(settings_, [this](HotkeyAction action) { Hotkey(action); }, error);
        }
        RefreshStatus();
    }
    void ApplyHotkeyEdits() {
        if (wayland_) return;
        Settings candidate = settings_;
        const auto fields = HotkeyFields();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            HotkeyChord chord;
            if (!ParseHotkey(gtk_entry_get_text(GTK_ENTRY(hotkeyEditors_[i])), chord, hotkeyEditError_)) { RefreshStatus(); return; }
            candidate.*fields[i] = chord.canonical;
        }
        if (!hotkeys_.Apply(candidate, hotkeyEditError_)) { RefreshStatus(); return; }
        settings_ = std::move(candidate);
        hotkeyDraft_ = false;
        hotkeyEditError_.clear();
        RefreshControls(); Changed();
    }
    void RestartEngine() {
        if (engine_) engine_->Stop();
        engine_ = CreatePlatformEngine();
        std::string error;
        if (!engine_->Start(settings_, error) && !error.empty()) saveError_ = error;
        else saveError_.clear();
        RefreshStatus();
    }
    void ProfileResult(const std::string& message, bool failed = false) {
        if (profileText_) gtk_label_set_text(GTK_LABEL(profileText_), message.c_str());
        if (failed) {
            saveError_ = message;
            RefreshStatus();
        }
    }
    std::string ChooseProfile(bool exporting) {
        auto* dialog = gtk_file_chooser_dialog_new(exporting ? "Export Linux settings profile" : "Import Linux settings profile",
            GTK_WINDOW(window_), exporting ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
            "Cancel", GTK_RESPONSE_CANCEL, exporting ? "Export" : "Import", GTK_RESPONSE_ACCEPT, nullptr);
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "ClickShade profile (*.ini)");
        gtk_file_filter_add_pattern(filter, "*.ini");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
        if (exporting) {
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "clickshade-linux-profile.ini");
            gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
        }
        std::string selected;
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (filename) { selected = filename; g_free(filename); }
        }
        gtk_widget_destroy(dialog);
        return selected;
    }
    void ExportProfile() {
        const auto path = ChooseProfile(true);
        if (path.empty()) return;
        std::string error;
        if (!SaveSettingsProfile(path, settings_, error)) { ProfileResult(error, true); return; }
        ProfileResult("Profile exported to " + path);
    }
    void BackupProfile() {
        const auto path = SettingsBackupPath();
        std::string error;
        if (!SaveSettingsProfile(path, settings_, error)) { ProfileResult(error, true); return; }
        ProfileResult("Current preferences backed up to " + path);
    }
    void ImportProfile() {
        const auto path = ChooseProfile(false);
        if (path.empty()) return;
        Settings candidate;
        std::string error;
        if (!LoadSettingsProfile(path, candidate, error)) { ProfileResult(error, true); return; }
        const auto fields = HotkeyFields();
        std::set<std::string> assigned;
        for (const auto field : fields) {
            HotkeyChord chord;
            if (!ParseHotkey(candidate.*field, chord, error)) { ProfileResult(error, true); return; }
            if (!chord.canonical.empty() && !assigned.insert(chord.canonical).second) {
                ProfileResult("The imported profile assigns one shortcut to more than one action.", true); return;
            }
            candidate.*field = chord.canonical;
        }
        auto* confirmation = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Import this Linux settings profile?");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirmation),
            "All preferences will be replaced. Your start-at-sign-in choice stays unchanged. The previous preferences will be saved to a single before-import backup. Selected sound and cursor files are not copied.");
        gtk_dialog_add_buttons(GTK_DIALOG(confirmation), "Cancel", GTK_RESPONSE_CANCEL,
            "Import profile", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(confirmation), GTK_RESPONSE_CANCEL);
        const bool confirmed = gtk_dialog_run(GTK_DIALOG(confirmation)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(confirmation);
        if (!confirmed) return;

        const Settings previous = settings_;
        candidate.startWithWindows = IsStartupEnabled();
        // Every external operation completes before replacing the live model.
        // The old working grabs are retained on registration failure and are
        // restored if atomic preferences persistence fails afterward.
        if (!SaveSettingsProfile(SettingsBackupPath(true), previous, error)) { ProfileResult(error, true); return; }
        if (!wayland_ && !hotkeys_.Apply(candidate, error)) { ProfileResult(error, true); return; }
        if (!SaveSettings(candidate, error)) {
            if (!wayland_) {
                std::string rollback;
                if (!hotkeys_.Apply(previous, rollback)) error += " Previous shortcuts could not be restored: " + rollback;
            }
            ProfileResult(error, true); return;
        }
        if (saveTimer_) { g_source_remove(saveTimer_); saveTimer_ = 0; }
        settings_ = std::move(candidate);
        hotkeyDraft_ = false; hotkeyEditError_.clear(); saveError_.clear();
        pulses_.clear(); previewPressed_ = false;
        RefreshControls(); ApplyTheme();
        if (engine_) engine_->Update(settings_);
        if (savedText_) gtk_label_set_text(GTK_LABEL(savedText_), "Profile imported and saved");
        ProfileResult("Profile imported. Previous preferences: " + SettingsBackupPath(true));
        RefreshStatus();
    }
    void RestoreAllDefaults() {
        auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Restore all ClickShade defaults?");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
            "Every preference will be reset, including cursor choices, highlights, click effects, sounds, exclusions, colors, and zoom. Start at sign-in and start minimized will be turned off. Selected cursor and sound files will stay on disk.");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL,
            "Restore all defaults", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const bool confirmed = gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT;
        gtk_widget_destroy(dialog);
        if (!confirmed) return;

        if (saveTimer_) { g_source_remove(saveTimer_); saveTimer_ = 0; }
        std::string error;
        if (!ResetSettings(settings_, error)) {
            saveError_ = error;
            RefreshControls();
            RefreshStatus();
            gtk_label_set_text(GTK_LABEL(savedText_), "Defaults could not be restored");
            auto* failure = gtk_message_dialog_new(GTK_WINDOW(window_), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Could not restore all defaults.");
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(failure), "%s", error.c_str());
            gtk_dialog_run(GTK_DIALOG(failure));
            gtk_widget_destroy(failure);
            return;
        }

        pulses_.clear();
        hotkeyDraft_ = false;
        previewPressed_ = false;
        if (previewTick_) { gtk_widget_remove_tick_callback(preview_, previewTick_); previewTick_ = 0; }
        RefreshControls();
        ApplyTheme();
        // A global reset also clears held/pulse/audio state. Ordinary appearance
        // edits still use Update and never restart the desktop connection.
        RestartEngine();
        RestartHotkeys();
        gtk_label_set_text(GTK_LABEL(savedText_), "All defaults restored");
    }
    void ApplyTheme() {
        if (!css_) return;
        const bool light = Luminance(settings_.uiBackground) > 145;
        const auto foreground = light ? Rgb(24, 31, 42) : Rgb(239, 244, 250);
        const auto surface = Mix(settings_.uiBackground, light ? 0u : 0xffffffu, light ? .025 : .05);
        const auto border = Mix(settings_.uiBackground, foreground, .14);
        const auto muted = Mix(settings_.uiBackground, foreground, .65);
        const auto hover = Mix(surface, settings_.uiAccent, .13);
        const auto accentText = Luminance(settings_.uiAccent) > 145 ? Rgb(13, 27, 23) : Rgb(255, 255, 255);
        const auto area = WorkArea(window_);
        const double requested = settings_.uiScalePercent ? settings_.uiScalePercent / 100. : 1.;
        const double fit = std::clamp(std::min((area.width - 24) / 640., (area.height - 80) / 460.), 1., 2.);
        const double zoom = std::min(requested, fit);
        effectiveZoom_ = zoom;
        lastWorkArea_ = area;
        std::ostringstream css;
        css << "@define-color cs_bg " << Hex(settings_.uiBackground) << ";"
            << "@define-color cs_surface " << Hex(surface) << ";"
            << "@define-color cs_text " << Hex(foreground) << ";"
            << "@define-color cs_muted " << Hex(muted) << ";"
            << "@define-color cs_line " << Hex(border) << ";"
            << "@define-color cs_accent " << Hex(settings_.uiAccent) << ";"
            << "@define-color cs_hover " << Hex(hover) << ";"
            << "@define-color cs_onaccent " << Hex(accentText) << ";"
            << "window.clickshade {background:@cs_bg; color:@cs_text; font-family: Cantarell, 'DejaVu Sans', sans-serif; font-size:" << std::lround(14 * zoom) << "px;}"
            << ".clickshade headerbar {background:@cs_surface; color:@cs_text; border-bottom:1px solid @cs_line; box-shadow:none;}"
            << ".clickshade label {color:@cs_text;} .clickshade .muted {color:@cs_muted;}"
            << ".clickshade .small {font-size:" << std::lround(12 * zoom) << "px;}"
            << ".clickshade .brand {font-size:" << std::lround(32 * zoom) << "px; font-weight:700; letter-spacing:-1px;}"
            << ".clickshade .page-title {font-size:" << std::lround(24 * zoom) << "px; font-weight:700;}"
            << ".clickshade .card-title {font-size:" << std::lround(17 * zoom) << "px; font-weight:700;}"
            << ".clickshade .row-title {font-weight:500;}"
            << ".clickshade .card {background:@cs_surface; border:1px solid @cs_line; border-radius:14px;}"
            << ".clickshade .card > border {border:0;}"
            << ".clickshade separator {background:@cs_line; min-height:1px;}"
            << ".clickshade button, .clickshade combobox button {background:@cs_surface; color:@cs_text; border:1px solid @cs_line; border-radius:8px; padding:8px 13px; box-shadow:none; text-shadow:none;}"
            << ".clickshade button:hover {background:@cs_hover; border-color:@cs_accent;}"
            << ".clickshade button:checked {background:@cs_accent; color:@cs_onaccent; border-color:@cs_accent;}"
            << ".clickshade button:checked label {color:@cs_onaccent;}"
            << ".clickshade button:focus, .clickshade entry:focus {border-color:@cs_accent; outline-color:@cs_accent; outline-width:1px;}"
            << ".clickshade button:disabled, .clickshade entry:disabled, .clickshade switch:disabled {opacity:.5;}"
            << ".clickshade .page-tabs button {border-radius:8px; margin-right:5px;}"
            << ".clickshade entry, .clickshade spinbutton {background:@cs_bg; color:@cs_text; border:1px solid @cs_line; border-radius:8px; padding:7px; box-shadow:none;}"
            << ".clickshade spinbutton entry {border:none; padding:0; background:transparent;}"
            << ".clickshade spinbutton button {border:none; padding:3px; background:transparent;}"
            << ".clickshade switch {background:@cs_line; border:1px solid @cs_line; border-radius:15px; min-width:40px; min-height:24px;}"
            << ".clickshade switch:checked {background:@cs_accent; border-color:@cs_accent;}"
            << ".clickshade switch slider {background:white; border:none; border-radius:50%; min-width:20px; min-height:20px; box-shadow:0 1px 2px rgba(0,0,0,.2);}"
            << ".clickshade scale trough {background:@cs_line; min-height:5px; border:none; border-radius:4px;}"
            << ".clickshade scale highlight {background:@cs_accent; border:none; border-radius:4px;}"
            << ".clickshade scale slider {background:@cs_accent; border:2px solid @cs_surface; min-width:16px; min-height:16px; border-radius:50%; box-shadow:none;}"
            << ".clickshade checkbutton check {background:@cs_bg; border:1px solid @cs_line; border-radius:4px;}"
            << ".clickshade checkbutton check:checked {background:@cs_accent; color:@cs_onaccent; border-color:@cs_accent;}"
            << ".clickshade scrollbar {background:transparent;} .clickshade scrollbar slider {background:@cs_line; border:none; min-width:6px; border-radius:5px;}"
            << ".clickshade .error {color:" << (light ? "#a93636" : "#ffaaaa") << ";}";
        GError* error = nullptr;
        gtk_css_provider_load_from_data(css_, css.str().c_str(), -1, &error);
        if (error) { saveError_ = "The interface colors could not be applied: " + std::string(error->message); g_error_free(error); RefreshStatus(); }
        const int maximumWidth = std::max(320, area.width - 24);
        const int maximumHeight = std::max(240, area.height - static_cast<int>(std::lround(70 * effectiveZoom_)));
        gtk_widget_set_size_request(window_, std::min(700, maximumWidth), std::min(540, maximumHeight));
        int width = 0, height = 0;
        gtk_window_get_size(GTK_WINDOW(window_), &width, &height);
        if (width > maximumWidth || height > maximumHeight)
            gtk_window_resize(GTK_WINDOW(window_), std::min(width, maximumWidth), std::min(height, maximumHeight));
        if (preview_) gtk_widget_queue_draw(preview_);
    }
    void RefreshControls() {
        refreshing_ = true;
        for (const auto& refresh : refresh_) refresh();
        refreshing_ = false;
        if (preview_) gtk_widget_queue_draw(preview_);
    }
    void Changed(bool theme = false) {
        if (refreshing_) return;
        NormalizeSettings(settings_);
        if (theme) ApplyTheme();
        if (preview_) gtk_widget_queue_draw(preview_);
        if (engine_) engine_->Update(settings_);
        if (saveTimer_) g_source_remove(saveTimer_);
        if (savedText_) gtk_label_set_text(GTK_LABEL(savedText_), "Saving changes…");
        saveTimer_ = g_timeout_add(160, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            self->saveTimer_ = 0; self->SaveNow(); return G_SOURCE_REMOVE;
        }, this);
        RefreshStatus();
    }
    void SaveNow() {
        if (saveTimer_) { g_source_remove(saveTimer_); saveTimer_ = 0; }
        std::string error;
        const bool ok = SaveSettings(settings_, error);
        saveError_ = ok ? std::string() : error;
        if (savedText_) gtk_label_set_text(GTK_LABEL(savedText_), ok ?
            (hotkeyDraft_ ? "Apply shortcuts to save them" : "All changes saved") : "Settings could not be saved");
        // The GNOME companion reads the atomic settings file. Re-notify after
        // persistence; its worker coalesces reloads and never blocks this UI.
        if (ok && engine_) engine_->Update(settings_);
        RefreshStatus();
    }
    void RefreshStatus() {
        if (!window_ || !engine_) return;
        const auto status = engine_->Status();
        // A status label is deliberately sampled much less often than effects.
        // Avoid displaying a stale transient "pressed" state between samples.
        const std::string headline = status.pressed ? "Effects active" : status.stateText;
        if (statusText_) gtk_label_set_text(GTK_LABEL(statusText_), headline.c_str());
        if (backendText_) gtk_label_set_text(GTK_LABEL(backendText_), status.backend.c_str());
        if (capabilityText_) gtk_label_set_text(GTK_LABEL(capabilityText_), wayland_
            ? "Global highlights, click effects, the magnifier lens, the key display, click sounds and keyboard sounds require the bundled GNOME Shell extension. Other Wayland desktops are not supported. Cursor darkening, shake enlargement, trails, the spotlight, a centered lens, alternate profiles, reverse mode, custom cursors, cursor resizing, global shortcuts, and confined-pointer detection are unavailable."
            : "A compositing desktop is required for pointer darkening, highlights, click effects, the magnifier lens and the key display. WAV click and keyboard sounds use the desktop audio service. The lens always sits beside the pointer. Alternate cursor profiles, reverse mode, custom cursors, cursor resizing, and confined-pointer detection are unavailable on X11.");
        const auto shortcutError = !hotkeyEditError_.empty() ? hotkeyEditError_ : hotkeys_.LastError();
        const std::string error = !saveError_.empty() ? saveError_ : !shortcutError.empty() ? shortcutError : status.lastError;
        if (errorText_) {
            gtk_label_set_text(GTK_LABEL(errorText_), error.c_str());
            gtk_widget_set_visible(errorText_, !error.empty());
        }
    }
    void StartPreviewAnimation() {
        gtk_widget_queue_draw(preview_);
        if (previewTick_) return;
        previewTick_ = gtk_widget_add_tick_callback(preview_, +[](GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<Application*>(data);
            const gint64 now = g_get_monotonic_time();
            self->pulses_.erase(std::remove_if(self->pulses_.begin(), self->pulses_.end(), [self, now](const Pulse& pulse) {
                return now - pulse.start >= static_cast<gint64>(self->settings_.clickDurationMs) * 1000;
            }), self->pulses_.end());
            gtk_widget_queue_draw(widget);
            if (self->pulses_.empty()) { self->previewTick_ = 0; return G_SOURCE_REMOVE; }
            return G_SOURCE_CONTINUE;
        }, this, nullptr);
    }
    void DrawPreview(GtkWidget* widget, cairo_t* cr) {
        const double width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
        const double x = width / 2, y = height / 2 - 10;
        Source(cr, settings_.uiBackground); cairo_paint(cr);
        const bool light = Luminance(settings_.uiBackground) > 145;
        Source(cr, light ? 0u : 0xffffffu, .06);
        for (double dx = 12; dx < width; dx += 22)
            for (double dy = 12; dy < height; dy += 22) { cairo_arc(cr, dx, dy, .8, 0, 2*kPi); cairo_fill(cr); }
        if (settings_.highlightEnabled) {
            const int style = settings_.highlightStyle;
            const bool spotlight = style == kHighlightSpotlight;
            const double radius = spotlight ? std::min<double>(settings_.spotlightRadius, height * .42) : settings_.highlightRadius;
            const double alpha = settings_.highlightOpacity / 100.;
            const double thickness = settings_.highlightThickness;
            const auto rgba = Rgba(settings_.highlightColor);
            if (spotlight) {
                auto* dim = cairo_pattern_create_radial(x, y, 0, x, y, radius);
                cairo_pattern_add_color_stop_rgba(dim, 0, 0, 0, 0, 0);
                cairo_pattern_add_color_stop_rgba(dim, .78, 0, 0, 0, 0);
                cairo_pattern_add_color_stop_rgba(dim, 1, 0, 0, 0, settings_.spotlightDim / 100.);
                cairo_pattern_set_extend(dim, CAIRO_EXTEND_PAD);
                cairo_set_source(cr, dim); cairo_paint(cr); cairo_pattern_destroy(dim);
                Source(cr, settings_.highlightColor, alpha);
                cairo_set_line_width(cr, thickness); cairo_arc(cr, x, y, radius, 0, 2*kPi); cairo_stroke(cr);
            } else if (style == kHighlightGlow) {
                auto* glow = cairo_pattern_create_radial(x, y, 0, x, y, radius * 1.4);
                cairo_pattern_add_color_stop_rgba(glow, 0, rgba.red, rgba.green, rgba.blue, alpha * .9);
                cairo_pattern_add_color_stop_rgba(glow, .55, rgba.red, rgba.green, rgba.blue, alpha * .45);
                cairo_pattern_add_color_stop_rgba(glow, 1, rgba.red, rgba.green, rgba.blue, 0);
                cairo_set_source(cr, glow); cairo_arc(cr, x, y, radius * 1.4, 0, 2*kPi); cairo_fill(cr);
                cairo_pattern_destroy(glow);
            } else if (style == kHighlightCrosshair) {
                Source(cr, settings_.highlightColor, alpha);
                cairo_set_line_width(cr, std::max(1., thickness * .6));
                for (const auto& arm : std::array<std::pair<double, double>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                    cairo_move_to(cr, x + arm.first * radius * .45, y + arm.second * radius * .45);
                    cairo_line_to(cr, x + arm.first * radius * 1.75, y + arm.second * radius * 1.75);
                }
                cairo_stroke(cr);
                cairo_set_line_width(cr, thickness); cairo_arc(cr, x, y, radius, 0, 2*kPi); cairo_stroke(cr);
            } else {
                Source(cr, settings_.highlightColor, alpha);
                cairo_arc(cr, x, y, radius, 0, 2*kPi);
                if (style == kHighlightFilled) cairo_fill(cr);
                else { cairo_set_line_width(cr, thickness); cairo_stroke(cr); }
            }
        }
        if (settings_.clickEffectsEnabled) {
            const gint64 now = g_get_monotonic_time();
            for (const auto& pulse : pulses_) {
                const bool wanted = (pulse.button == 1 && settings_.leftClickEnabled) || (pulse.button == 2 && settings_.rightClickEnabled) ||
                    (pulse.button == 4 && settings_.middleClickEnabled);
                if (!wanted) continue;
                const double t = std::clamp((now - pulse.start) / (settings_.clickDurationMs * 1000.), 0., 1.);
                const double eased = settings_.clickEasing == 1 ? 1 - std::pow(1 - t, 3) :
                                     settings_.clickEasing == 2 ? t*t*(3-2*t) : t;
                const double radius = settings_.clickRadius * (.25 + .75 * eased);
                const auto color = pulse.button == 1 ? settings_.leftClickColor : pulse.button == 2 ? settings_.rightClickColor : settings_.middleClickColor;
                const double alpha = settings_.clickOpacity / 100. * (1-t);
                const double thickness = settings_.clickThickness;
                const auto ring = [&](double r, double lineWidth, double a) {
                    Source(cr, color, a); cairo_set_line_width(cr, lineWidth);
                    cairo_arc(cr, pulse.x, pulse.y, std::max(.5, r - lineWidth / 2), 0, 2*kPi); cairo_stroke(cr);
                };
                switch (settings_.clickStyle) {
                case kClickPulse: Source(cr, color, alpha); cairo_arc(cr, pulse.x, pulse.y, radius, 0, 2*kPi); cairo_fill(cr); break;
                case kClickRipple: ring(radius, thickness, alpha); ring(radius * .62, thickness, alpha * .55); break;
                case kClickBurst:
                    Source(cr, color, alpha);
                    for (unsigned point = 0; point < 8; ++point) {
                        const double angle = point * kPi / 4 + kPi / 8;
                        cairo_arc(cr, pulse.x + std::cos(angle) * radius, pulse.y + std::sin(angle) * radius, std::max(1.5, thickness * .9 * (1 - t * .5)), 0, 2*kPi);
                        cairo_fill(cr);
                    }
                    break;
                case kClickNeon: ring(radius, thickness * 3.2, alpha * .28); ring(radius, thickness, alpha); break;
                case kClickTarget: {
                    const double scope = std::max(2., settings_.clickRadius * (1 - .5 * eased));
                    ring(scope, thickness, alpha);
                    Source(cr, color, alpha); cairo_set_line_width(cr, thickness);
                    for (unsigned tick = 0; tick < 4; ++tick) {
                        const double angle = tick * kPi / 2;
                        cairo_move_to(cr, pulse.x + std::cos(angle) * scope * .55, pulse.y + std::sin(angle) * scope * .55);
                        cairo_line_to(cr, pulse.x + std::cos(angle) * (scope - thickness / 2), pulse.y + std::sin(angle) * (scope - thickness / 2));
                    }
                    cairo_stroke(cr);
                    break;
                }
                default: ring(radius, thickness, alpha); break;
                }
            }
        }
        cairo_save(cr); cairo_translate(cr, x, y);
        cairo_move_to(cr, 0, 0); cairo_line_to(cr, 0, 27); cairo_line_to(cr, 7, 20);
        cairo_line_to(cr, 13, 33); cairo_line_to(cr, 18, 30); cairo_line_to(cr, 12, 18);
        cairo_line_to(cr, 22, 18); cairo_close_path(cr);
        const bool dark = settings_.darkCursorEnabled && previewPressed_;
        Source(cr, dark ? Rgb(24, 28, 34) : Rgb(255, 255, 255)); cairo_fill_preserve(cr);
        Source(cr, dark ? Rgb(255, 255, 255) : Rgb(25, 29, 35));
        cairo_set_line_width(cr, 1.5); cairo_stroke(cr); cairo_restore(cr);
    }
};

} // namespace clickshade::linuxapp

int main(int argc, char** argv) {
    using namespace clickshade::linuxapp;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            std::cout << "ClickShade 1.0 — Linux x64\nUsage: ClickShade [--background|--restore|--quit]\n"
                         "Native X11 effects or the optional GNOME Wayland companion.\n"
                         "Settings: " << SettingsPath() << '\n';
            return 0;
        }
        if (std::string(argv[i]) == "--version") { std::cout << "ClickShade 1.0 Linux x64\n"; return 0; }
    }
    XInitThreads();
    auto* gtk = gtk_application_new("io.clickshade.ClickShade", G_APPLICATION_HANDLES_COMMAND_LINE);
    Application app(gtk);
    g_signal_connect(gtk, "activate", G_CALLBACK(+[](GtkApplication*, gpointer data) {
        static_cast<Application*>(data)->ShowExplicit();
    }), &app);
    g_signal_connect(gtk, "command-line", G_CALLBACK(+[](GApplication*, GApplicationCommandLine* command, gpointer data) -> int {
        auto* self = static_cast<Application*>(data);
        int count = 0;
        char** args = g_application_command_line_get_arguments(command, &count);
        bool background = false;
        bool restore = false;
        bool quit = false;
        for (int i = 1; i < count; ++i) {
            const std::string arg = args[i];
            if (arg == "--background" || arg == "--tray") background = true;
            else if (arg == "--restore") restore = true;
            else if (arg == "--quit") quit = true;
            else { g_application_command_line_printerr(command, "Unknown option: %s\n", args[i]); g_strfreev(args); return 2; }
        }
        g_strfreev(args);
        if (quit) { self->Quit(); return 0; }
        self->Present(background);
        if (restore) self->Restore();
        return 0;
    }), &app);
    const int result = g_application_run(G_APPLICATION(gtk), argc, argv);
    g_object_unref(gtk);
    return result;
}
