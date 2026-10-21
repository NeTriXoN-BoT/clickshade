#include "settings.h"
#include "desktop_entry.h"

#include <glib.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace clickshade::linuxapp {
int RunSelfTest() {
    int passed = 0, failed = 0;
    const auto check = [&](bool value, const char* name) {
        std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
        value ? ++passed : ++failed;
    };
    char pattern[] = "/tmp/clickshade-settings-XXXXXX";
    char* created = mkdtemp(pattern);
    if (!created) { std::cerr << "FAIL could not create isolated test directory\n"; return 1; }
    const std::filesystem::path directory(created);
    const auto file = (directory / "nested" / "settings.ini").string();
    std::string error;
    Settings original;
    original.cursorStyle = 3;
    original.cursorReverse = true;
    original.cursorSize = 64;
    original.cursorHold = false;
    original.cursorFlashMs = 230;
    original.shakeStyle = 2; original.shakeSensitivity = 155; original.shakeMaximumScale = 350;
    original.excludedProcesses = "editor;game;应用";
    original.allowedProcesses = "creative-app;work-tool;绘图";
    original.releasedCursorPath = "/home/test/Cursor files/指针.cur";
    original.pressedCursorPath = "/home/test/Cursor files/pressed.cur";
    original.highlightColor = Rgb(9, 117, 204);
    original.highlightStyle = kHighlightCrosshair;
    original.highlightRadius = 37;
    original.highlightBreathing = true; original.highlightIdleHideMs = 2500; original.spotlightDim = 33; original.spotlightRadius = 99;
    original.clickStyle = kClickRipple; original.middleClickEnabled = true; original.middleClickColor = Rgb(4, 5, 6);
    original.keyboardSoundsEnabled = true; original.keyboardSoundPath = "/home/test/Sounds/keyboard/mechanical";
    original.keyboardVolume = 42; original.captureVisible = false; original.uiAnimatedAccent = false;
    original.hotkeyKeyboardSounds = "Ctrl+Shift+F6";
    original.magnifierEnabled = true; original.magnifierZoom = 300; original.magnifierSize = 180; original.magnifierShape = 1;
    original.magnifierBorderColor = Rgb(1, 2, 3); original.magnifierFrame = false;
    original.keyDisplayEnabled = true; original.keyDisplayMode = 1; original.keyDisplayMouse = true; original.keyDisplayPosition = 2;
    original.keyDisplaySize = 140; original.keyDisplayDurationMs = 2200; original.keyDisplayOpacity = 55; original.keyDisplayColor = Rgb(9, 8, 7);
    original.hotkeyMagnifier = "Ctrl+Shift+F7"; original.hotkeyKeyDisplay = "Ctrl+Shift+F8";
    original.trailEnabled = true; original.trailStyle = 2; original.trailRadius = 8;
    original.trailOpacity = 70; original.trailDurationMs = 400; original.trailColor = Rgb(90, 120, 240);
    original.clickEffectsEnabled = false;
    original.clickEasing = 2;
    original.visualRefreshRate = 240;
    original.leftSoundPath = "/home/test/Sounds/quiet left.wav";
    original.rightSoundPath = "/home/test/Sounds/right.wav";
    original.middleSoundPath = "/home/test/Sounds/middle.wav";
    original.soundVolume = 26;
    original.uiScalePercent = 175;
    original.uiAccent = Rgb(231, 43, 72);
    original.uiDarkMode = false;
    original.uiBackground = Rgb(240, 245, 250);
    original.uiDarkBackground = Rgb(10, 20, 30); original.uiDarkAccent = Rgb(200, 190, 180);
    original.hotkeyToggleAll = "Ctrl+Shift+F1"; original.hotkeyHighlight = "Ctrl+Shift+F2";
    original.hotkeyClicks = "Ctrl+Shift+F3"; original.hotkeySounds = "Ctrl+Shift+F4";
    original.hotkeySettings = "Ctrl+Shift+F5"; original.hotkeyRestore = "Ctrl+Shift+F12";
    check(SaveSettingsFile(file, original, error), "atomic UTF-8 save creates parent directory");
    struct stat mode{};
    check(stat(file.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "settings are private to the current user");
    Settings restored;
    check(LoadSettingsFile(file, restored, error), "saved settings can be read");
    check(restored.cursorStyle == 3 && restored.cursorReverse && restored.cursorSize == 64 && !restored.cursorHold && restored.cursorFlashMs == 230,
          "cursor style, reverse, size and flash settings round-trip");
    check(restored.shakeStyle == 2 && restored.shakeSensitivity == 155 && restored.shakeMaximumScale == 350,
          "all shake-to-locate preferences round-trip");
    check(restored.releasedCursorPath == original.releasedCursorPath && restored.pressedCursorPath == original.pressedCursorPath,
          "Unicode and spaced cursor paths round-trip");
    check(restored.excludedProcesses == original.excludedProcesses, "Unicode exclusion list round-trips");
    check(restored.allowedProcesses == original.allowedProcesses, "Unicode fullscreen allowlist round-trips");
    check(restored.highlightColor == Rgb(9, 117, 204) && restored.highlightStyle == kHighlightCrosshair && restored.highlightRadius == 37,
          "highlight color, style and size round-trip");
    check(restored.highlightBreathing && restored.highlightIdleHideMs == 2500 && restored.spotlightDim == 33 && restored.spotlightRadius == 99 &&
          restored.clickStyle == kClickRipple && restored.middleClickEnabled && restored.middleClickColor == Rgb(4, 5, 6) &&
          restored.keyboardSoundsEnabled && restored.keyboardSoundPath == original.keyboardSoundPath && restored.keyboardVolume == 42 &&
          !restored.captureVisible && !restored.uiAnimatedAccent && restored.hotkeyKeyboardSounds == "Ctrl+Shift+F6",
          "style, middle click, keyboard sound, spotlight and idle-hide options round-trip");
    check(restored.magnifierEnabled && restored.magnifierZoom == 300 && restored.magnifierSize == 180 && restored.magnifierShape == 1 &&
          restored.magnifierBorderColor == Rgb(1, 2, 3) && !restored.magnifierFrame && restored.keyDisplayEnabled && restored.keyDisplayMode == 1 && restored.keyDisplayMouse &&
          restored.keyDisplayPosition == 2 && restored.keyDisplaySize == 140 && restored.keyDisplayDurationMs == 2200 &&
          restored.keyDisplayOpacity == 55 && restored.keyDisplayColor == Rgb(9, 8, 7) && restored.hotkeyMagnifier == "Ctrl+Shift+F7" &&
          restored.hotkeyKeyDisplay == "Ctrl+Shift+F8",
          "magnifier and key display options and their shortcuts round-trip");
    {
        std::ofstream legacy04(file);
        // 16754505 = RGB(73,167,255) and 9072383 = RGB(255,110,138), the preview-build defaults.
        legacy04 << "[ClickShade]\nHighlightFilled=0\nClickFilled=1\nLeftClickColor=16754505\nRightClickColor=9072383\n";
    }
    Settings upgraded;
    check(LoadSettingsFile(file, upgraded, error) && upgraded.highlightStyle == kHighlightRing && upgraded.clickStyle == kClickPulse &&
          upgraded.leftClickColor == Settings{}.leftClickColor && upgraded.rightClickColor == Settings{}.rightClickColor,
          "a preview-build settings file upgrades its shape switches and untouched click colors");
    {
        std::ofstream customized(file);
        customized << "[ClickShade]\nSettingsVersion=2\nLeftClickColor=16754505\n";
    }
    check(LoadSettingsFile(file, upgraded, error) && upgraded.leftClickColor == 16754505u,
          "a versioned file keeps the old blue when the user chose it deliberately");
    check(restored.trailEnabled && restored.trailStyle == 2 && restored.trailRadius == 8 && restored.trailOpacity == 70 &&
          restored.trailDurationMs == 400 && restored.trailColor == original.trailColor, "all optional trail preferences round-trip");
    check(!restored.clickEffectsEnabled && restored.clickEasing == 2 && restored.visualRefreshRate == 240,
          "animation and frame limit round-trip");
    check(restored.leftSoundPath == original.leftSoundPath && restored.rightSoundPath == original.rightSoundPath && restored.soundVolume == 26,
          "separate sound files and volume round-trip");
    check(restored.middleSoundPath == original.middleSoundPath, "middle-button sound selection round-trips independently");
    check(restored.uiScalePercent == 175 && restored.uiAccent == original.uiAccent, "interface zoom and colors round-trip");
    check(!restored.uiDarkMode && restored.uiDarkBackground == original.uiDarkBackground && restored.uiDarkAccent == original.uiDarkAccent &&
          restored.uiLightBackground == original.uiBackground && restored.uiLightAccent == original.uiAccent,
          "explicit light mode retains independent dark and light color pairs");
    check(restored.hotkeyToggleAll == original.hotkeyToggleAll && restored.hotkeyHighlight == original.hotkeyHighlight &&
          restored.hotkeyClicks == original.hotkeyClicks && restored.hotkeySounds == original.hotkeySounds &&
          restored.hotkeySettings == original.hotkeySettings && restored.hotkeyRestore == original.hotkeyRestore,
          "all six global shortcut assignments round-trip");
    check(Rgb(1, 2, 3) == 0x030201u, "color bytes match the shared Windows COLORREF convention");
    {
        std::ofstream bad(file);
        bad << "[Unrelated]\nHighlightRadius=88\n[ClickShade]\nHighlightRadius=10000000\nClickRadius=-50\n"
               "SoundVolume=not-a-number\nUiScalePercent=999\nCursorSize=2\nVisualRefreshRate=1\n"
               "Buttons=63\nUnknownFutureKey=ignored\nHighlightOpacity=0\nCursorFlashMs=99999\n"
               "TrailStyle=99\nTrailRadius=99\nTrailOpacity=-4\nTrailDurationMs=99999\n"
               "ShakeStyle=99\nShakeSensitivity=2\nShakeMaximumScale=999\n";
    }
    Settings bounded;
    check(LoadSettingsFile(file, bounded, error), "unknown keys and unrelated sections are tolerated");
    check(bounded.highlightRadius == 100 && bounded.clickRadius == 12 && bounded.highlightOpacity == 5,
          "out-of-range visual sizes and opacity are bounded");
    check(bounded.soundVolume == 75, "invalid numeric text keeps the default");
    check(bounded.uiScalePercent == 200 && bounded.cursorSize == 24 && bounded.visualRefreshRate == 30 && bounded.cursorFlashMs == 500,
          "cursor, zoom, refresh and flash values are bounded");
    check(bounded.shakeStyle == 2 && bounded.shakeSensitivity == 50 && bounded.shakeMaximumScale == 400,
          "shake style, sensitivity and maximum enlargement are bounded");
    check(bounded.buttonMask == 31, "unsupported mouse button bits are removed");
    check(bounded.trailStyle == 4 && bounded.trailRadius == 16 && bounded.trailOpacity == 5 && bounded.trailDurationMs == 700,
          "trail styles, size, opacity and duration are bounded");
    {
        std::ofstream legacy(file);
        legacy << "[ClickShade]\nUiBackground=16777215\nUiAccent=123456\n";
    }
    Settings legacy;
    check(LoadSettingsFile(file, legacy, error) && !legacy.uiDarkMode && legacy.uiLightBackground == 0xffffffu &&
          legacy.uiLightAccent == 123456u && legacy.uiDarkBackground == Settings{}.uiDarkBackground,
          "legacy active colors seed the inferred mode without losing the other mode defaults");
    {
        std::ofstream bom(file, std::ios::binary);
        bom << "\xef\xbb\xbf[ClickShade]\nEnabled=0\n";
    }
    Settings utf8;
    check(LoadSettingsFile(file, utf8, error) && !utf8.enabled, "UTF-8 BOM is supported");
    {
        std::ofstream invalid(file, std::ios::binary);
        const char bytes[] = {'[', '\0', 'C', '\0'};
        invalid.write(bytes, sizeof(bytes));
    }
    check(!LoadSettingsFile(file, utf8, error), "binary or UTF-16 input is rejected with a clear error");
    {
        std::ofstream invalid(file, std::ios::binary);
        invalid << "[ClickShade]\nLeftSoundPath=\xc0\xaf\n";
    }
    check(!LoadSettingsFile(file, utf8, error), "invalid and overlong UTF-8 sequences are rejected");
    {
        std::ofstream large(file, std::ios::binary);
        large << std::string(512 * 1024 + 1, 'a');
    }
    check(!LoadSettingsFile(file, utf8, error), "oversized input is rejected before parsing");
    original.leftSoundPath = "/tmp/click.wav\nEnabled=0";
    check(SaveSettingsFile(file, original, error), "save sanitizes line breaks in path values");
    Settings sanitized;
    check(LoadSettingsFile(file, sanitized, error) && sanitized.enabled && sanitized.leftSoundPath.find('\n') == std::string::npos,
          "a path cannot inject a second INI key");
    Settings longUnicode;
    for (unsigned i = 0; i < 12000; ++i) longUnicode.leftSoundPath += "指";
    Settings longRestored;
    check(SaveSettingsFile(file, longUnicode, error) && LoadSettingsFile(file, longRestored, error) &&
          longRestored.leftSoundPath.size() <= 32768 && longRestored.leftSoundPath.size() % 3 == 0,
          "bounded long Unicode paths retain complete UTF-8 sequences and valid settings");
    check(!SaveSettingsFile(directory.string(), original, error), "unwritable file target reports failure without replacing the directory");
    const char* previousConfig = std::getenv("XDG_CONFIG_HOME");
    const bool hadConfig = previousConfig != nullptr;
    const std::string savedConfig = previousConfig ? previousConfig : "";
    const auto isolatedConfig = (directory / "isolated-config").string();
    setenv("XDG_CONFIG_HOME", isolatedConfig.c_str(), 1);
    check(ConfigureStartup(true, error), "autostart can be created in an isolated XDG folder");
    check(IsStartupEnabled(), "autostart entry resolves to the current executable");
    check(ConfigureStartup(false, error) && !IsStartupEnabled(), "autostart can be removed without touching other entries");
    // GLib independently applies the Desktop Entry string layer and the Exec
    // argument layer. Unusual valid installation paths must survive both.
    const auto startupRoundTrip = [](const std::string& path) {
        const std::string contents = "[Desktop Entry]\nType=Application\nName=ClickShade\nExec=" +
            detail::QuoteDesktopExecArgument(path) + " --background\n";
        GKeyFile* entry = g_key_file_new();
        GError* parseError = nullptr;
        bool ok = g_key_file_load_from_data(entry, contents.data(), contents.size(), G_KEY_FILE_NONE, &parseError);
        gchar* command = ok ? g_key_file_get_string(entry, "Desktop Entry", "Exec", &parseError) : nullptr;
        gint argc = 0;
        gchar** argv = nullptr;
        ok = command && g_shell_parse_argv(command, &argc, &argv, &parseError) && argc == 2;
        // Desktop Entry's %% field code is a literal percent after expansion.
        std::string executable = ok ? argv[0] : "";
        for (std::size_t at = 0; (at = executable.find("%%", at)) != std::string::npos; ++at) executable.erase(at, 1);
        ok = ok && executable == path && std::string(argv[1]) == "--background";
        g_strfreev(argv); g_free(command); g_clear_error(&parseError); g_key_file_free(entry);
        return ok;
    };
    check(startupRoundTrip("/tmp/a folder/ClickShade"), "autostart preserves spaces through Desktop Entry and argument parsing");
    check(startupRoundTrip("/tmp/dollar$and`backtick/ClickShade"), "autostart preserves dollar and backtick characters without expansion");
    check(startupRoundTrip("/tmp/quote\"and\\slash/ClickShade"), "autostart preserves quotes and literal backslashes through both escape layers");
    check(startupRoundTrip("/tmp/100%/ClickShade"), "autostart escapes literal percent field codes");
    check(startupRoundTrip("/tmp/line\nreturn\rtab\t/ClickShade"), "autostart safely encodes control characters in valid filesystem paths");
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream output; output << input.rdbuf(); return output.str();
    };
    const auto profile = (directory / "profile.ini").string();
    original.leftSoundPath = "/tmp/sounds/custom left.wav";
    check(SaveSettingsProfile(profile, original, error), "profile export writes a versioned Linux snapshot");
    const auto validProfile = read(profile);
    Settings imported;
    check(LoadSettingsProfile(profile, imported, error) && imported.shakeStyle == original.shakeStyle &&
          imported.middleSoundPath == original.middleSoundPath && imported.hotkeyRestore == original.hotkeyRestore,
          "profile import preserves new effects, all sound slots and shortcuts");
    const auto fullSnapshot = (directory / "snapshot.ini").string();
    const auto importedSnapshot = (directory / "imported.ini").string();
    check(SaveSettingsFile(fullSnapshot, original, error) && SaveSettingsFile(importedSnapshot, imported, error) &&
          read(fullSnapshot) == read(importedSnapshot), "every profile preference round-trips through the standard serializer");
    const auto rejectProfile = [&](std::string content) {
        { std::ofstream output(profile, std::ios::binary); output << content; }
        Settings unchanged; unchanged.highlightRadius = 73;
        error.clear();
        return !LoadSettingsProfile(profile, unchanged, error) && !error.empty() && unchanged.highlightRadius == 73;
    };
    const auto replace = [&](const std::string& oldValue, const std::string& newValue) {
        std::string content = validProfile;
        const auto position = content.find(oldValue);
        if (position == std::string::npos) return std::string();
        content.replace(position, oldValue.size(), newValue);
        return content;
    };
    check(rejectProfile(replace("Platform=Linux", "Platform=Windows")), "a foreign-platform profile is rejected without changing settings");
    check(rejectProfile(replace("FormatVersion=2", "FormatVersion=3")), "unsupported profile versions are rejected");
    {
        // A format 1 preview-build export: no style keys, the two shape switches instead.
        std::string legacyProfile = replace("FormatVersion=2", "FormatVersion=1");
        for (const char* key : {"SettingsVersion", "CaptureVisible", "HighlightBreathing", "HighlightIdleHideMs", "SpotlightDim",
                                "SpotlightRadius", "MiddleClickEnabled", "MiddleClickColor", "KeyboardSoundsEnabled", "KeyboardSoundPath",
                                "KeyboardVolume", "UiAnimatedAccent", "HotkeyKeyboardSounds"}) {
            const auto at = legacyProfile.find(std::string("\n") + key + "=");
            if (at == std::string::npos) { legacyProfile.clear(); break; }
            legacyProfile.erase(at, legacyProfile.find('\n', at + 1) - at);
        }
        const auto styleAt = legacyProfile.find("\nHighlightStyle=");
        if (styleAt != std::string::npos) legacyProfile.replace(styleAt, legacyProfile.find('\n', styleAt + 1) - styleAt, "\nHighlightFilled=0");
        const auto clickAt = legacyProfile.find("\nClickStyle=");
        if (clickAt != std::string::npos) legacyProfile.replace(clickAt, legacyProfile.find('\n', clickAt + 1) - clickAt, "\nClickFilled=1");
        { std::ofstream output(profile, std::ios::binary); output << legacyProfile; }
        Settings fromLegacy;
        check(!legacyProfile.empty() && LoadSettingsProfile(profile, fromLegacy, error) && fromLegacy.highlightStyle == kHighlightRing &&
              fromLegacy.clickStyle == kClickPulse && fromLegacy.keyboardVolume == Settings{}.keyboardVolume && fromLegacy.captureVisible,
              "a complete format 1 profile imports with upgraded styles and default newer options");
    }
    {
        // A format 2 profile from before the presenter tools lacks their keys
        // entirely; it is still complete and they take their defaults.
        std::string older = validProfile;
        for (const char* key : {"MagnifierEnabled", "MagnifierZoom", "MagnifierSize", "MagnifierShape", "MagnifierPosition",
                                "MagnifierBorderColor", "MagnifierFrame", "KeyDisplayEnabled", "KeyDisplayMode", "KeyDisplayMouse", "KeyDisplayPosition",
                                "KeyDisplaySize", "KeyDisplayDurationMs", "KeyDisplayOpacity", "KeyDisplayColor", "HotkeyMagnifier", "HotkeyKeyDisplay"}) {
            const auto at = older.find(std::string("\n") + key + "=");
            if (at == std::string::npos) { older.clear(); break; }
            older.erase(at, older.find('\n', at + 1) - at);
        }
        { std::ofstream output(profile, std::ios::binary); output << older; }
        Settings fromOlder;
        check(!older.empty() && LoadSettingsProfile(profile, fromOlder, error) && !fromOlder.magnifierEnabled && !fromOlder.keyDisplayEnabled &&
              fromOlder.magnifierZoom == Settings{}.magnifierZoom && fromOlder.hotkeyKeyDisplay.empty() && fromOlder.shakeStyle == 2,
              "a format 2 profile without the presenter keys imports with their defaults");
    }
    check(rejectProfile(replace("MagnifierZoom=300", "MagnifierZoom=801")), "out-of-range magnifier zoom is rejected");
    check(rejectProfile(replace("MiddleSoundPath=" + original.middleSoundPath + "\n", "")), "incomplete profile snapshots are rejected");
    check(rejectProfile(validProfile + "UnknownPreference=1\n"), "unknown profile preferences are rejected instead of silently ignored");
    check(rejectProfile(validProfile + "Enabled=0\n"), "duplicate profile preferences are rejected");
    check(rejectProfile(replace("SoundVolume=26", "SoundVolume=invalid")), "malformed profile numbers are rejected");
    check(rejectProfile(replace("SoundVolume=26", "SoundVolume=101")), "out-of-range profile values are rejected rather than clamped");
    check(rejectProfile(replace("Enabled=1", "Enabled=2")), "profile booleans accept only zero or one");
    check(rejectProfile(replace("UiLightAccent=" + std::to_string(original.uiAccent), "UiLightAccent=1")),
          "inconsistent profile active and remembered theme colors are rejected");
    check(rejectProfile(validProfile + std::string("\xc0\xaf", 2)), "invalid UTF-8 profiles are rejected");
    check(rejectProfile(std::string(512 * 1024 + 1, 'a')), "oversized profiles are rejected before allocation or parsing");
    std::filesystem::remove(profile);
    check(mkfifo(profile.c_str(), 0600) == 0 && !LoadSettingsProfile(profile, imported, error),
          "profile FIFO input is rejected without waiting for a writer");
    std::filesystem::remove(profile);
    check(!LoadSettingsProfile(directory.string(), imported, error), "profile directories are rejected as nonregular files");
    check(!SaveSettingsProfile(directory.string(), original, error), "profile export failure preserves the destination directory");
    check(ConfigureStartup(true, error) && SaveSettingsProfile(profile, Settings{}, error) &&
          LoadSettingsProfile(profile, imported, error) && IsStartupEnabled(),
          "profile serialization and parsing never change actual autostart registration");
    check(ConfigureStartup(false, error) && SettingsBackupPath().find("profiles/settings-backup.ini") != std::string::npos &&
          SettingsBackupPath(true).find("profiles/before-import.ini") != std::string::npos,
          "manual and pre-import backups have distinct fixed paths without accumulating history");
    Settings reset = original;
    reset.startWithWindows = true;
    reset.startMinimized = true;
    reset.enabled = false;
    reset.pauseFullscreen = false;
    check(ConfigureStartup(true, error) && SaveSettings(reset, error) && ResetSettings(reset, error),
          "global reset saves default preferences successfully");
    check(!IsStartupEnabled() && !reset.startWithWindows && !reset.startMinimized,
          "global reset disables actual XDG startup and start-minimized");
    const auto expected = directory / "defaults.ini";
    const auto live = directory / "live-defaults.ini";
    const bool serialized = SaveSettingsFile(expected.string(), Settings{}, error) && SaveSettingsFile(live.string(), reset, error);
    check(serialized && read(SettingsPath()) == read(expected) && read(live) == read(expected),
          "global reset replaces every persisted and live field with Settings defaults");
    const auto configFolder = std::filesystem::path(SettingsPath()).parent_path();
    check(std::distance(std::filesystem::directory_iterator(configFolder), std::filesystem::directory_iterator{}) == 1,
          "global reset creates no backup, history, or log files");
    const auto startupFile = std::filesystem::path(isolatedConfig) / "autostart/clickshade.desktop";
    std::filesystem::create_directory(startupFile);
    Settings blocked = original;
    const auto beforeFailure = read(SettingsPath());
    check(!ResetSettings(blocked, error) && !error.empty() && blocked.cursorStyle == original.cursorStyle &&
          read(SettingsPath()) == beforeFailure,
          "startup-removal failure leaves preferences unchanged and reports the error");
    std::filesystem::remove(startupFile);
    std::filesystem::remove(SettingsPath());
    std::filesystem::create_directory(SettingsPath());
    blocked.startWithWindows = true;
    check(ConfigureStartup(true, error) && !ResetSettings(blocked, error) && !error.empty() &&
          blocked.cursorStyle == original.cursorStyle && blocked.startWithWindows && IsStartupEnabled(),
          "INI replacement failure retains preferences and restores the prior startup choice");
    if (hadConfig) setenv("XDG_CONFIG_HOME", savedConfig.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    check(!ec, "isolated test files are removed");
    std::cout << "\n" << passed << " passed, " << failed << " failed.\n"
                 "This headless suite tests settings only; desktop input, composition, and audio need interactive verification.\n";
    return failed ? 1 : 0;
}
} // namespace clickshade::linuxapp

int main() { return clickshade::linuxapp::RunSelfTest(); }
