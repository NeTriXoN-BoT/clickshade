#include "settings.h"
#include "desktop_entry.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace clickshade::linuxapp {
namespace {
std::string ConfigRoot() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return xdg;
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) + "/.config" : std::string();
}
std::string StartupPath() {
    const auto root = ConfigRoot();
    return root.empty() ? std::string() : root + "/autostart/clickshade.desktop";
}
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
std::string SingleLine(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return c == '\r' || c == '\n' || c == 0;
    }), value.end());
    if (value.size() > 32768) {
        std::size_t end = 32768;
        // Do not leave a partial UTF-8 sequence at the byte-size boundary.
        while (end && (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u) --end;
        value.resize(end);
    }
    return value;
}
bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        const int count = lead >= 0xc2 && lead <= 0xdf ? 1 :
                          lead >= 0xe0 && lead <= 0xef ? 2 :
                          lead >= 0xf0 && lead <= 0xf4 ? 3 : -1;
        if (count < 0 || text.size() - i < static_cast<std::size_t>(count)) return false;
        std::uint32_t scalar = lead & (count == 1 ? 0x1fu : count == 2 ? 0x0fu : 0x07u);
        for (int j = 0; j < count; ++j) {
            const unsigned char part = static_cast<unsigned char>(text[i++]);
            if ((part & 0xc0u) != 0x80u) return false;
            scalar = (scalar << 6u) | (part & 0x3fu);
        }
        if (scalar < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
            scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu)) return false;
    }
    return true;
}
bool ReadRegularText(const std::string& path, std::size_t maximum, std::string& text, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) { error = "Could not open the settings file."; return false; }
    struct Close {
        explicit Close(int descriptor) : fd(descriptor) {}
        Close(const Close&) = delete;
        Close& operator=(const Close&) = delete;
        ~Close() { close(fd); }
        int fd;
    } owner{fd};
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > maximum) {
        error = "Choose a regular UTF-8 settings file no larger than 512 KiB."; return false;
    }
    text.assign(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < text.size()) {
        const auto n = read(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = "Could not read the complete settings file."; return false; }
        done += static_cast<std::size_t>(n);
    }
    return true;
}
std::map<std::string, std::string> Entries(const std::string& data, const char* section, bool& duplicate) {
    std::map<std::string, std::string> result;
    std::istringstream input(data.compare(0, 3, "\xef\xbb\xbf") == 0 ? data.substr(3) : data);
    std::string line;
    bool active = false;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') { active = line == section; continue; }
        const auto equals = line.find('=');
        if (!active || equals == std::string::npos) continue;
        if (!result.emplace(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1))).second) duplicate = true;
    }
    return result;
}
bool AtomicWrite(const std::string& path, const std::string& contents, std::string& error) {
    if (path.empty()) { error = "Your user configuration folder could not be located."; return false; }
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) { error = "Could not create the settings folder: " + ec.message(); return false; }
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) { error = "Could not create a temporary settings file: " + std::string(std::strerror(errno)); return false; }
    bool ok = true;
    std::size_t done = 0;
    while (done < contents.size()) {
        const ssize_t n = write(fd, contents.data() + done, contents.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += static_cast<std::size_t>(n);
    }
    if (ok && fsync(fd) != 0) ok = false;
    const int savedError = errno;
    if (close(fd) != 0) ok = false;
    if (ok && rename(name.data(), path.c_str()) != 0) ok = false;
    if (!ok) {
        const int why = errno ? errno : savedError;
        unlink(name.data());
        error = "Could not save settings: " + std::string(std::strerror(why));
        return false;
    }
    return true;
}
} // namespace

std::string ExecutablePath() {
    std::vector<char> path(65536);
    const ssize_t n = readlink("/proc/self/exe", path.data(), path.size() - 1);
    return n > 0 && static_cast<std::size_t>(n) < path.size() - 1
        ? std::string(path.data(), static_cast<std::size_t>(n)) : std::string();
}
std::string SettingsPath() {
    const auto root = ConfigRoot();
    return root.empty() ? std::string() : root + "/clickshade/settings.ini";
}
void NormalizeSettings(Settings& s) noexcept {
    s.cursorStyle = std::clamp(s.cursorStyle, 0, 3);
    s.cursorSize = s.cursorSize ? std::clamp(s.cursorSize, 24, 128) : 0;
    s.cursorFlashMs = std::clamp(s.cursorFlashMs, 50, 500);
    s.shakeStyle = std::clamp(s.shakeStyle, 0, 2);
    s.shakeSensitivity = std::clamp(s.shakeSensitivity, 50, 200);
    s.shakeMaximumScale = std::clamp(s.shakeMaximumScale, 150, 400);
    s.buttonMask &= 31u;
    s.highlightStyle = std::clamp(s.highlightStyle, 0, kHighlightStyleCount - 1);
    s.highlightRadius = std::clamp(s.highlightRadius, 8, 100);
    s.highlightOpacity = std::clamp(s.highlightOpacity, 5, 100);
    s.highlightThickness = std::clamp(s.highlightThickness, 1, 12);
    s.highlightIdleHideMs = s.highlightIdleHideMs ? std::clamp(s.highlightIdleHideMs, 500, 10000) : 0;
    s.spotlightDim = std::clamp(s.spotlightDim, 10, 90);
    s.spotlightRadius = std::clamp(s.spotlightRadius, 40, 400);
    s.clickStyle = std::clamp(s.clickStyle, 0, kClickStyleCount - 1);
    s.keyboardVolume = std::clamp(s.keyboardVolume, 0, 100);
    s.middleClickColor &= 0xffffffu;
    s.trailStyle = std::clamp(s.trailStyle, 0, 4);
    s.trailRadius = std::clamp(s.trailRadius, 2, 16);
    s.trailOpacity = std::clamp(s.trailOpacity, 5, 100);
    s.trailDurationMs = std::clamp(s.trailDurationMs, 80, 700);
    s.trailColor &= 0xffffffu;
    s.clickRadius = std::clamp(s.clickRadius, 12, 120);
    s.clickDurationMs = std::clamp(s.clickDurationMs, 100, 1500);
    s.clickOpacity = std::clamp(s.clickOpacity, 5, 100);
    s.clickThickness = std::clamp(s.clickThickness, 1, 12);
    s.clickEasing = std::clamp(s.clickEasing, 0, 2);
    s.visualRefreshRate = s.visualRefreshRate ? std::clamp(s.visualRefreshRate, 30, 360) : 0;
    s.soundVolume = std::clamp(s.soundVolume, 0, 100);
    s.uiScalePercent = s.uiScalePercent ? std::clamp(s.uiScalePercent, 100, 200) : 0;
    s.magnifierZoom = std::clamp(s.magnifierZoom, 125, 800);
    s.magnifierSize = std::clamp(s.magnifierSize, 80, 600);
    s.magnifierShape = std::clamp(s.magnifierShape, 0, 1);
    s.magnifierPosition = std::clamp(s.magnifierPosition, 0, 1);
    s.magnifierBorderColor &= 0xffffffu;
    s.keyDisplayMode = std::clamp(s.keyDisplayMode, 0, 1);
    s.keyDisplayPosition = std::clamp(s.keyDisplayPosition, 0, 3);
    s.keyDisplaySize = std::clamp(s.keyDisplaySize, 60, 250);
    s.keyDisplayDurationMs = std::clamp(s.keyDisplayDurationMs, 300, 6000);
    s.keyDisplayOpacity = std::clamp(s.keyDisplayOpacity, 20, 100);
    s.keyDisplayColor &= 0xffffffu;
    s.highlightColor &= 0xffffffu;
    s.leftClickColor &= 0xffffffu;
    s.rightClickColor &= 0xffffffu;
    s.uiBackground &= 0xffffffu;
    s.uiAccent &= 0xffffffu;
    s.uiDarkBackground &= 0xffffffu; s.uiDarkAccent &= 0xffffffu;
    s.uiLightBackground &= 0xffffffu; s.uiLightAccent &= 0xffffffu;
    if (s.uiDarkMode) { s.uiDarkBackground = s.uiBackground; s.uiDarkAccent = s.uiAccent; }
    else { s.uiLightBackground = s.uiBackground; s.uiLightAccent = s.uiAccent; }
}

static bool ParseSettingsText(std::string data, Settings& s, std::string& error) {
    if (data.compare(0, 3, "\xef\xbb\xbf") == 0) data.erase(0, 3);
    if (data.find('\0') != std::string::npos || !ValidUtf8(data)) {
        error = "Linux settings must be saved as UTF-8 text."; return false;
    }
    std::map<std::string, std::string> values;
    std::istringstream input(data);
    std::string line;
    bool inSection = false;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') { inSection = line == "[ClickShade]"; continue; }
        if (!inSection) continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
    }
    const auto number = [&](const char* key, long long fallback) {
        const auto it = values.find(key);
        if (it == values.end()) return fallback;
        long long value = 0;
        const auto& text = it->second;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : fallback;
    };
    const auto integer = [&](const char* key, int fallback) {
        return static_cast<int>(std::clamp(number(key, fallback), -2147483647LL, 2147483647LL));
    };
    const auto boolean = [&](const char* key, bool fallback) { return number(key, fallback ? 1 : 0) != 0; };
    const auto color = [&](const char* key, std::uint32_t fallback) {
        return static_cast<std::uint32_t>(number(key, fallback)) & 0xffffffu;
    };
    const auto text = [&](const char* key, const std::string& fallback) {
        const auto it = values.find(key); return it == values.end() ? fallback : it->second;
    };
#define BOOL(field, key) s.field = boolean(key, s.field)
#define INT(field, key) s.field = integer(key, s.field)
#define TEXT(field, key) s.field = text(key, s.field)
#define COLOR(field, key) s.field = color(key, s.field)
    BOOL(enabled, "Enabled"); BOOL(darkCursorEnabled, "DarkCursorEnabled");
    INT(cursorStyle, "CursorStyle"); BOOL(cursorReverse, "CursorReverse");
    INT(cursorSize, "CursorSize"); BOOL(cursorHold, "CursorHold"); INT(cursorFlashMs, "CursorFlashMs");
    INT(shakeStyle, "ShakeStyle"); INT(shakeSensitivity, "ShakeSensitivity"); INT(shakeMaximumScale, "ShakeMaximumScale");
    TEXT(releasedCursorPath, "ReleasedCursorPath"); TEXT(pressedCursorPath, "PressedCursorPath");
    s.buttonMask = static_cast<unsigned>(number("Buttons", s.buttonMask)) & 31u;
    BOOL(pauseFullscreen, "PauseFullscreen"); BOOL(pauseConfinedCursor, "PauseConfinedCursor");
    BOOL(startWithWindows, "StartWithWindows"); BOOL(startMinimized, "StartMinimized");
    BOOL(captureVisible, "CaptureVisible");
    TEXT(excludedProcesses, "ExcludedProcesses"); BOOL(highlightEnabled, "HighlightEnabled");
    TEXT(allowedProcesses, "AllowedProcesses");
    // Preview-build files carry HighlightFilled/ClickFilled instead of the styles.
    const int version = integer("SettingsVersion", 1);
    if (values.find("HighlightStyle") != values.end()) INT(highlightStyle, "HighlightStyle");
    else s.highlightStyle = boolean("HighlightFilled", true) ? kHighlightFilled : kHighlightRing;
    INT(highlightRadius, "HighlightRadius");
    INT(highlightOpacity, "HighlightOpacity"); INT(highlightThickness, "HighlightThickness");
    COLOR(highlightColor, "HighlightColor"); BOOL(clickEffectsEnabled, "ClickEffectsEnabled");
    BOOL(highlightBreathing, "HighlightBreathing"); INT(highlightIdleHideMs, "HighlightIdleHideMs");
    INT(spotlightDim, "SpotlightDim"); INT(spotlightRadius, "SpotlightRadius");
    BOOL(trailEnabled, "TrailEnabled"); INT(trailStyle, "TrailStyle"); INT(trailRadius, "TrailRadius");
    INT(trailOpacity, "TrailOpacity"); INT(trailDurationMs, "TrailDurationMs"); COLOR(trailColor, "TrailColor");
    BOOL(leftClickEnabled, "LeftClickEnabled"); BOOL(rightClickEnabled, "RightClickEnabled");
    BOOL(middleClickEnabled, "MiddleClickEnabled");
    if (values.find("ClickStyle") != values.end()) INT(clickStyle, "ClickStyle");
    else s.clickStyle = boolean("ClickFilled", false) ? kClickPulse : kClickRing;
    INT(clickRadius, "ClickRadius");
    INT(clickDurationMs, "ClickDurationMs"); INT(clickOpacity, "ClickOpacity");
    INT(clickThickness, "ClickThickness"); INT(clickEasing, "ClickEasing");
    INT(visualRefreshRate, "VisualRefreshRate"); COLOR(leftClickColor, "LeftClickColor");
    COLOR(rightClickColor, "RightClickColor"); COLOR(middleClickColor, "MiddleClickColor");
    BOOL(clickSoundsEnabled, "ClickSoundsEnabled");
    TEXT(leftSoundPath, "LeftSoundPath"); TEXT(rightSoundPath, "RightSoundPath");
    TEXT(middleSoundPath, "MiddleSoundPath");
    INT(soundVolume, "SoundVolume");
    BOOL(keyboardSoundsEnabled, "KeyboardSoundsEnabled"); TEXT(keyboardSoundPath, "KeyboardSoundPath");
    INT(keyboardVolume, "KeyboardVolume");
    BOOL(magnifierEnabled, "MagnifierEnabled"); INT(magnifierZoom, "MagnifierZoom"); INT(magnifierSize, "MagnifierSize");
    INT(magnifierShape, "MagnifierShape"); INT(magnifierPosition, "MagnifierPosition"); COLOR(magnifierBorderColor, "MagnifierBorderColor");
    BOOL(magnifierFrame, "MagnifierFrame");
    BOOL(keyDisplayEnabled, "KeyDisplayEnabled"); INT(keyDisplayMode, "KeyDisplayMode"); BOOL(keyDisplayMouse, "KeyDisplayMouse");
    INT(keyDisplayPosition, "KeyDisplayPosition"); INT(keyDisplaySize, "KeyDisplaySize"); INT(keyDisplayDurationMs, "KeyDisplayDurationMs");
    INT(keyDisplayOpacity, "KeyDisplayOpacity"); COLOR(keyDisplayColor, "KeyDisplayColor");
    COLOR(uiBackground, "UiBackground");
    COLOR(uiAccent, "UiAccent"); INT(uiScalePercent, "UiScalePercent"); BOOL(uiAnimatedAccent, "UiAnimatedAccent");
    if (version < 2) {
        // Untouched preview-build click colors move to the neon defaults.
        const Settings defaults;
        if (s.leftClickColor == Rgb(73, 167, 255)) s.leftClickColor = defaults.leftClickColor;
        if (s.rightClickColor == Rgb(255, 110, 138)) s.rightClickColor = defaults.rightClickColor;
    }
    const bool explicitMode = values.find("UiDarkMode") != values.end();
    if (explicitMode) BOOL(uiDarkMode, "UiDarkMode");
    else s.uiDarkMode = ((s.uiBackground & 255u) * 299 + ((s.uiBackground >> 8u) & 255u) * 587 +
                        ((s.uiBackground >> 16u) & 255u) * 114) <= 145000;
    COLOR(uiDarkBackground, "UiDarkBackground"); COLOR(uiDarkAccent, "UiDarkAccent");
    COLOR(uiLightBackground, "UiLightBackground"); COLOR(uiLightAccent, "UiLightAccent");
    if (explicitMode && values.find("UiBackground") == values.end())
        s.uiBackground = s.uiDarkMode ? s.uiDarkBackground : s.uiLightBackground;
    if (explicitMode && values.find("UiAccent") == values.end())
        s.uiAccent = s.uiDarkMode ? s.uiDarkAccent : s.uiLightAccent;
    TEXT(hotkeyToggleAll, "HotkeyToggleAll"); TEXT(hotkeyHighlight, "HotkeyHighlight");
    TEXT(hotkeyClicks, "HotkeyClicks"); TEXT(hotkeySounds, "HotkeySounds");
    TEXT(hotkeySettings, "HotkeySettings"); TEXT(hotkeyRestore, "HotkeyRestore");
    TEXT(hotkeyKeyboardSounds, "HotkeyKeyboardSounds");
    TEXT(hotkeyMagnifier, "HotkeyMagnifier"); TEXT(hotkeyKeyDisplay, "HotkeyKeyDisplay");
#undef BOOL
#undef INT
#undef TEXT
#undef COLOR
    NormalizeSettings(s);
    return true;
}

static std::string SerializeSettings(const Settings& input) {
    Settings s = input;
    NormalizeSettings(s);
    std::ostringstream out;
    out << "; ClickShade 1.0 — UTF-8. Colors use r | (g << 8) | (b << 16).\n[ClickShade]\n";
    const auto write = [&](const char* key, const auto& value) { out << key << '=' << value << '\n'; };
#define WRITE(field, key) write(key, s.field)
#define TEXT(field, key) write(key, SingleLine(s.field))
    write("SettingsVersion", 2);
    WRITE(enabled, "Enabled"); WRITE(darkCursorEnabled, "DarkCursorEnabled");
    WRITE(cursorStyle, "CursorStyle"); WRITE(cursorReverse, "CursorReverse");
    WRITE(cursorSize, "CursorSize"); WRITE(cursorHold, "CursorHold"); WRITE(cursorFlashMs, "CursorFlashMs");
    WRITE(shakeStyle, "ShakeStyle"); WRITE(shakeSensitivity, "ShakeSensitivity"); WRITE(shakeMaximumScale, "ShakeMaximumScale");
    TEXT(releasedCursorPath, "ReleasedCursorPath"); TEXT(pressedCursorPath, "PressedCursorPath");
    WRITE(buttonMask, "Buttons"); WRITE(pauseFullscreen, "PauseFullscreen");
    WRITE(pauseConfinedCursor, "PauseConfinedCursor"); WRITE(startWithWindows, "StartWithWindows");
    WRITE(startMinimized, "StartMinimized"); WRITE(captureVisible, "CaptureVisible");
    TEXT(excludedProcesses, "ExcludedProcesses");
    TEXT(allowedProcesses, "AllowedProcesses");
    WRITE(highlightEnabled, "HighlightEnabled"); WRITE(highlightStyle, "HighlightStyle");
    WRITE(highlightRadius, "HighlightRadius"); WRITE(highlightOpacity, "HighlightOpacity");
    WRITE(highlightThickness, "HighlightThickness"); WRITE(highlightColor, "HighlightColor");
    WRITE(highlightBreathing, "HighlightBreathing"); WRITE(highlightIdleHideMs, "HighlightIdleHideMs");
    WRITE(spotlightDim, "SpotlightDim"); WRITE(spotlightRadius, "SpotlightRadius");
    WRITE(trailEnabled, "TrailEnabled"); WRITE(trailStyle, "TrailStyle"); WRITE(trailRadius, "TrailRadius");
    WRITE(trailOpacity, "TrailOpacity"); WRITE(trailDurationMs, "TrailDurationMs"); WRITE(trailColor, "TrailColor");
    WRITE(clickEffectsEnabled, "ClickEffectsEnabled"); WRITE(leftClickEnabled, "LeftClickEnabled");
    WRITE(rightClickEnabled, "RightClickEnabled"); WRITE(middleClickEnabled, "MiddleClickEnabled");
    WRITE(clickStyle, "ClickStyle");
    WRITE(clickRadius, "ClickRadius"); WRITE(clickDurationMs, "ClickDurationMs");
    WRITE(clickOpacity, "ClickOpacity"); WRITE(clickThickness, "ClickThickness");
    WRITE(clickEasing, "ClickEasing"); WRITE(visualRefreshRate, "VisualRefreshRate");
    WRITE(leftClickColor, "LeftClickColor"); WRITE(rightClickColor, "RightClickColor");
    WRITE(middleClickColor, "MiddleClickColor");
    WRITE(clickSoundsEnabled, "ClickSoundsEnabled"); TEXT(leftSoundPath, "LeftSoundPath");
    TEXT(rightSoundPath, "RightSoundPath"); WRITE(soundVolume, "SoundVolume");
    TEXT(middleSoundPath, "MiddleSoundPath");
    WRITE(keyboardSoundsEnabled, "KeyboardSoundsEnabled"); TEXT(keyboardSoundPath, "KeyboardSoundPath");
    WRITE(keyboardVolume, "KeyboardVolume");
    WRITE(magnifierEnabled, "MagnifierEnabled"); WRITE(magnifierZoom, "MagnifierZoom"); WRITE(magnifierSize, "MagnifierSize");
    WRITE(magnifierShape, "MagnifierShape"); WRITE(magnifierPosition, "MagnifierPosition"); WRITE(magnifierBorderColor, "MagnifierBorderColor");
    WRITE(magnifierFrame, "MagnifierFrame");
    WRITE(keyDisplayEnabled, "KeyDisplayEnabled"); WRITE(keyDisplayMode, "KeyDisplayMode"); WRITE(keyDisplayMouse, "KeyDisplayMouse");
    WRITE(keyDisplayPosition, "KeyDisplayPosition"); WRITE(keyDisplaySize, "KeyDisplaySize"); WRITE(keyDisplayDurationMs, "KeyDisplayDurationMs");
    WRITE(keyDisplayOpacity, "KeyDisplayOpacity"); WRITE(keyDisplayColor, "KeyDisplayColor");
    WRITE(uiBackground, "UiBackground"); WRITE(uiAccent, "UiAccent"); WRITE(uiScalePercent, "UiScalePercent");
    WRITE(uiAnimatedAccent, "UiAnimatedAccent");
    WRITE(uiDarkMode, "UiDarkMode"); WRITE(uiDarkBackground, "UiDarkBackground"); WRITE(uiDarkAccent, "UiDarkAccent");
    WRITE(uiLightBackground, "UiLightBackground"); WRITE(uiLightAccent, "UiLightAccent");
    TEXT(hotkeyToggleAll, "HotkeyToggleAll"); TEXT(hotkeyHighlight, "HotkeyHighlight");
    TEXT(hotkeyClicks, "HotkeyClicks"); TEXT(hotkeySounds, "HotkeySounds");
    TEXT(hotkeySettings, "HotkeySettings"); TEXT(hotkeyRestore, "HotkeyRestore");
    TEXT(hotkeyKeyboardSounds, "HotkeyKeyboardSounds");
    TEXT(hotkeyMagnifier, "HotkeyMagnifier"); TEXT(hotkeyKeyDisplay, "HotkeyKeyDisplay");
#undef WRITE
#undef TEXT
    return out.str();
}

bool LoadSettingsFile(const std::string& path, Settings& settings, std::string& error) {
    std::string data;
    return ReadRegularText(path, 512 * 1024, data, error) && ParseSettingsText(std::move(data), settings, error);
}
bool SaveSettingsFile(const std::string& path, const Settings& settings, std::string& error) {
    return AtomicWrite(path, SerializeSettings(settings), error);
}
std::string SettingsBackupPath(bool beforeImport) {
    const auto path = SettingsPath();
    return path.empty() ? std::string() : (std::filesystem::path(path).parent_path() / "profiles" /
        (beforeImport ? "before-import.ini" : "settings-backup.ini")).string();
}
bool SaveSettingsProfile(const std::string& path, const Settings& settings, std::string& error) {
    const std::string data = "[ClickShadeProfile]\nFormatVersion=2\nPlatform=Linux\n\n" + SerializeSettings(settings);
    if (!ValidUtf8(data)) { error = "Settings profiles must contain valid UTF-8 text."; return false; }
    return AtomicWrite(path, data, error);
}
bool LoadSettingsProfile(const std::string& path, Settings& settings, std::string& error) {
    std::string data;
    if (!ReadRegularText(path, 512 * 1024, data, error)) return false;
    if (data.find('\0') != std::string::npos || !ValidUtf8(data)) {
        error = "Choose a UTF-8 ClickShade settings profile."; return false;
    }
    bool duplicate = false;
    const auto metadata = Entries(data, "[ClickShadeProfile]", duplicate);
    if (duplicate || metadata.size() != 2 || metadata.find("FormatVersion") == metadata.end() ||
        (metadata.at("FormatVersion") != "1" && metadata.at("FormatVersion") != "2") ||
        metadata.find("Platform") == metadata.end() || metadata.at("Platform") != "Linux") {
        error = "This file is not a supported Linux ClickShade profile (format version 1 or 2)."; return false;
    }
    const bool legacy = metadata.at("FormatVersion") == "1";
    const auto entries = Entries(data, "[ClickShade]", duplicate);
    auto schema = Entries(SerializeSettings(Settings{}), "[ClickShade]", duplicate);
    // Format 1 profiles predate these keys and carried the two shape switches.
    static const char* const kVersion2Keys[]{
        "SettingsVersion", "CaptureVisible", "HighlightStyle", "HighlightBreathing", "HighlightIdleHideMs",
        "SpotlightDim", "SpotlightRadius", "MiddleClickEnabled", "ClickStyle", "MiddleClickColor",
        "KeyboardSoundsEnabled", "KeyboardSoundPath", "KeyboardVolume", "UiAnimatedAccent", "HotkeyKeyboardSounds"};
    if (legacy) {
        for (const char* key : kVersion2Keys) schema.erase(key);
        schema.emplace("HighlightFilled", "1");
        schema.emplace("ClickFilled", "0");
    }
    // The presenter tools arrived after format 2 profiles already existed, so
    // an export without their keys is still complete; they take their defaults.
    static const char* const kOptionalKeys[]{
        "MagnifierEnabled", "MagnifierZoom", "MagnifierSize", "MagnifierShape", "MagnifierPosition", "MagnifierBorderColor", "MagnifierFrame",
        "KeyDisplayEnabled", "KeyDisplayMode", "KeyDisplayMouse", "KeyDisplayPosition", "KeyDisplaySize", "KeyDisplayDurationMs",
        "KeyDisplayOpacity", "KeyDisplayColor", "HotkeyMagnifier", "HotkeyKeyDisplay"};
    for (const char* key : kOptionalKeys) if (entries.find(key) == entries.end()) schema.erase(key);
    if (duplicate || entries.size() != schema.size()) {
        error = "The profile must contain every recognized preference exactly once."; return false;
    }
    const std::vector<std::string> textKeys{
        "ReleasedCursorPath", "PressedCursorPath", "ExcludedProcesses", "AllowedProcesses",
        "LeftSoundPath", "RightSoundPath", "MiddleSoundPath", "KeyboardSoundPath", "HotkeyToggleAll", "HotkeyHighlight",
        "HotkeyClicks", "HotkeySounds", "HotkeySettings", "HotkeyRestore", "HotkeyKeyboardSounds", "HotkeyMagnifier", "HotkeyKeyDisplay"};
    std::map<std::string, long long> numbers;
    for (const auto& expected : schema) {
        const auto found = entries.find(expected.first);
        if (found == entries.end()) { error = "The profile is missing " + expected.first + "."; return false; }
        if (std::find(textKeys.begin(), textKeys.end(), expected.first) != textKeys.end()) {
            if (found->second.size() > 32768) { error = expected.first + " is too long."; return false; }
            continue;
        }
        long long value = 0;
        const auto& text = found->second;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            error = expected.first + " has an invalid numeric value."; return false;
        }
        numbers.emplace(expected.first, value);
    }
    Settings candidate;
    if (!ParseSettingsText(data, candidate, error)) return false;
    const auto normalized = Entries(SerializeSettings(candidate), "[ClickShade]", duplicate);
    for (const auto& number : numbers) {
        // Legacy switches and the recorded version are consumed by the upgrade.
        if (number.first == "HighlightFilled" || number.first == "ClickFilled" || number.first == "SettingsVersion") continue;
        long long value = 0;
        const auto& text = normalized.at(number.first);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || value != number.second) {
            error = number.first + " is outside its supported range or conflicts with its selected mode."; return false;
        }
    }
    settings = std::move(candidate);
    return true;
}

Settings LoadSettings() {
    Settings settings;
    std::string error;
    const auto path = SettingsPath();
    if (!path.empty()) LoadSettingsFile(path, settings, error);
    // Only relocate the two known bundled files from the original flat layout,
    // and only when the old path no longer exists. Custom selections are kept.
    const auto migrateBundled = [](std::string& value, const char* filename) {
        const auto previous = std::filesystem::path(value);
        std::error_code ec;
        if (previous.filename() != filename || previous.parent_path().filename() != "sounds" ||
            std::filesystem::exists(previous, ec) || ec) return;
        const auto current = std::filesystem::path(ExecutablePath()).parent_path() / "sounds" / "soft" / filename;
        if (std::filesystem::is_regular_file(current, ec) && !ec) value = current.string();
    };
    migrateBundled(settings.leftSoundPath, "soft-left.wav");
    migrateBundled(settings.rightSoundPath, "soft-right.wav");
    settings.startWithWindows = IsStartupEnabled();
    return settings;
}
bool SaveSettings(const Settings& s, std::string& error) { return SaveSettingsFile(SettingsPath(), s, error); }
bool ResetSettings(Settings& settings, std::string& error) {
    const bool wasStartupEnabled = IsStartupEnabled();
    if (!ConfigureStartup(false, error)) return false;
    Settings defaults;
    if (!SaveSettings(defaults, error)) {
        // Keep the live preferences unchanged if the atomic INI replacement
        // failed. Restore an enabled startup choice where possible as well.
        if (wasStartupEnabled) {
            std::string startupError;
            if (!ConfigureStartup(true, startupError))
                error += " Startup also remains off: " + startupError;
        }
        settings.startWithWindows = IsStartupEnabled();
        return false;
    }
    settings = std::move(defaults);
    return true;
}
bool IsStartupEnabled() {
    const auto path = StartupPath();
    if (path.empty()) return false;
    std::string contents, readError;
    if (!ReadRegularText(path, 64 * 1024, contents, readError)) return false;
    std::istringstream file(contents);
    std::string line;
    const auto expected = "Exec=" + detail::QuoteDesktopExecArgument(ExecutablePath()) + " --background";
    bool matched = false;
    while (std::getline(file, line)) {
        if (line == "Hidden=true") return false;
        if (line == expected) matched = true;
    }
    return matched;
}
bool ConfigureStartup(bool enabled, std::string& error) {
    const auto path = StartupPath();
    if (path.empty()) { error = "Your autostart folder could not be located."; return false; }
    if (!enabled) {
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            error = "Could not remove the ClickShade autostart entry: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }
    const auto executable = ExecutablePath();
    if (executable.empty()) { error = "Could not find the running ClickShade executable."; return false; }
    if (executable.find('=') != std::string::npos) {
        error = "To enable sign-in startup, move ClickShade to a path without an equals sign (=)."; return false;
    }
    return AtomicWrite(path,
        "[Desktop Entry]\nType=Application\nName=ClickShade\nComment=Cursor highlights and click effects\nExec=" +
        detail::QuoteDesktopExecArgument(executable) + " --background\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", error);
}

} // namespace clickshade::linuxapp
