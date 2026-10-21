#include "settings.h"
#include "hotkeys.h"
#define SECURITY_WIN32
#include <security.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace clickshade {
namespace {
constexpr int kSettingsVersion = 2;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ClickShade";
constexpr wchar_t kTaskName[] = L"ClickShade";
constexpr wchar_t kPortableMarker[] = L"ClickShade.portable";
// Preview-build defaults. A saved file that still carries exactly these values
// was never customized, so the upgrade moves it to the neon defaults.
constexpr COLORREF kLegacyLeftClickColor = RGB(73, 167, 255);
constexpr COLORREF kLegacyRightClickColor = RGB(255, 110, 138);

std::wstring Quoted(const std::wstring& value) { return L"\"" + value + L"\""; }

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& directory) {
    if (directory.empty()) return false;
    if (CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    return false;
}
} // namespace

std::wstring ExecutablePath() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return n && n < path.size() ? std::wstring(path.data(), n) : std::wstring();
}

std::wstring ExecutableDirectory() {
    const auto executable = ExecutablePath();
    const auto separator = executable.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : executable.substr(0, separator);
}

std::wstring SoundDirectory() {
    const auto directory = ExecutableDirectory();
    return directory.empty() ? std::wstring() : directory + L"\\sounds";
}

std::wstring KeyboardSoundDirectory() {
    const auto sounds = SoundDirectory();
    return sounds.empty() ? std::wstring() : sounds + L"\\keyboard";
}

bool PortableMode() {
    static const bool portable = [] {
        const auto directory = ExecutableDirectory();
        return !directory.empty() && FileExists(directory + L"\\" + kPortableMarker);
    }();
    return portable;
}

std::wstring SettingsDirectory() {
    if (PortableMode()) return ExecutableDirectory();
    wchar_t folder[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                              nullptr, SHGFP_TYPE_CURRENT, folder))) return {};
    const std::wstring directory = std::wstring(folder) + L"\\ClickShade";
    return EnsureDirectory(directory) ? directory : std::wstring();
}

std::wstring SettingsPath() {
    const auto directory = SettingsDirectory();
    return directory.empty() ? std::wstring() : directory + L"\\settings.ini";
}

std::wstring CrashReportDirectory() {
    const auto directory = SettingsDirectory();
    if (directory.empty()) return {};
    const auto reports = directory + L"\\crash-reports";
    return EnsureDirectory(reports) ? reports : std::wstring();
}

bool IsProcessElevated() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool RelaunchElevated(std::wstring& error) {
    error.clear();
    const auto exe = ExecutablePath();
    if (exe.empty()) { error = L"Could not locate ClickShade.exe."; return false; }
    // The new instance waits for this process to exit before claiming the
    // single-instance lock, so a settings-window handoff feels like a restart.
    const std::wstring arguments = L"--replace " + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) return true;
    const DWORD code = GetLastError();
    error = code == ERROR_CANCELLED
        ? L"The administrator prompt was declined, so ClickShade keeps running normally."
        : L"Windows could not start ClickShade as administrator (Windows error " + std::to_wstring(code) + L").";
    return false;
}

namespace {
struct Bstr {
    BSTR value = nullptr;
    explicit Bstr(const std::wstring& text) : value(SysAllocString(text.c_str())) {}
    ~Bstr() { if (value) SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};
template <class T> struct Com {
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
    T** Put() { if (value) value->Release(); value = nullptr; return &value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool Usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
struct EmptyVariant {
    VARIANT value;
    EmptyVariant() { VariantInit(&value); }
    ~EmptyVariant() { VariantClear(&value); }
};
// Interface identifiers are spelled out so no platform-specific import library
// is needed on either MSVC or the MinGW/Zig toolchain.
constexpr GUID kClsidTaskScheduler{0x0f87369f, 0xa4e5, 0x4cfc, {0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd}};
constexpr GUID kIidTaskService{0x2faba4c7, 0x4da9, 0x4013, {0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85}};
constexpr GUID kIidLogonTrigger{0x72dade38, 0xfae4, 0x4b3e, {0xba, 0xf4, 0x5d, 0x00, 0x9a, 0xf0, 0x2b, 0x1c}};
constexpr GUID kIidExecAction{0x4c3d624d, 0xfd6b, 0x49a3, {0xb9, 0xb7, 0x09, 0xcb, 0x3c, 0xd3, 0xf0, 0x47}};

std::wstring HResultMessage(HRESULT value) {
    wchar_t* text = nullptr;
    std::wstring result;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(value), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr) && text) {
        result = text;
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    }
    if (text) LocalFree(text);
    wchar_t code[32]{};
    swprintf(code, 32, L" (0x%08lX)", static_cast<unsigned long>(value));
    return result + code;
}

bool OpenTaskFolder(Apartment& apartment, Com<ITaskService>& service, Com<ITaskFolder>& folder, std::wstring& error) {
    if (!apartment.Usable()) { error = L"COM is unavailable for Task Scheduler access."; return false; }
    HRESULT hr = CoCreateInstance(kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, kIidTaskService,
                                  reinterpret_cast<void**>(service.Put()));
    if (FAILED(hr)) { error = L"Task Scheduler is unavailable: " + HResultMessage(hr); return false; }
    EmptyVariant none[4];
    hr = service->Connect(none[0].value, none[1].value, none[2].value, none[3].value);
    if (FAILED(hr)) { error = L"Could not connect to Task Scheduler: " + HResultMessage(hr); return false; }
    Bstr root(L"\\");
    hr = service->GetFolder(root.value, folder.Put());
    if (FAILED(hr)) { error = L"Could not open the Task Scheduler root folder: " + HResultMessage(hr); return false; }
    return true;
}

// Returns true when a ClickShade task exists; matches reports whether its
// action launches this executable.
bool QueryScheduledTask(bool& matches) {
    matches = false;
    Apartment apartment;
    Com<ITaskService> service; Com<ITaskFolder> folder; std::wstring error;
    if (!OpenTaskFolder(apartment, service, folder, error)) return false;
    Bstr name(kTaskName);
    Com<IRegisteredTask> task;
    if (FAILED(folder->GetTask(name.value, task.Put())) || !task) return false;
    Com<ITaskDefinition> definition;
    Com<IActionCollection> actions;
    if (SUCCEEDED(task->get_Definition(definition.Put())) && definition &&
        SUCCEEDED(definition->get_Actions(actions.Put())) && actions) {
        Com<IAction> action;
        if (SUCCEEDED(actions->get_Item(1, action.Put())) && action) {
            Com<IExecAction> exec;
            if (SUCCEEDED(action->QueryInterface(kIidExecAction, reinterpret_cast<void**>(exec.Put()))) && exec) {
                BSTR path = nullptr;
                if (SUCCEEDED(exec->get_Path(&path)) && path) {
                    matches = _wcsicmp(path, ExecutablePath().c_str()) == 0;
                    SysFreeString(path);
                }
            }
        }
    }
    return true;
}

std::wstring CurrentUserName() {
    std::array<wchar_t, 512> name{};
    ULONG size = static_cast<ULONG>(name.size());
    if (GetUserNameExW(NameSamCompatible, name.data(), &size) && size) return std::wstring(name.data(), size);
    return {};
}

bool CreateScheduledTask(std::wstring& error) {
    Apartment apartment;
    Com<ITaskService> service; Com<ITaskFolder> folder;
    if (!OpenTaskFolder(apartment, service, folder, error)) return false;
    const auto exe = ExecutablePath();
    const auto user = CurrentUserName();
    if (exe.empty() || user.empty()) { error = L"Could not determine the executable path or user account."; return false; }
    Com<ITaskDefinition> definition;
    HRESULT hr = service->NewTask(0, definition.Put());
    if (FAILED(hr)) { error = L"Could not create a task definition: " + HResultMessage(hr); return false; }
    Com<IRegistrationInfo> registration;
    if (SUCCEEDED(definition->get_RegistrationInfo(registration.Put())) && registration) {
        Bstr description(L"Starts ClickShade with administrator rights at sign-in. Created by ClickShade; remove with Rules > Start with Windows.");
        Bstr author(L"ClickShade");
        registration->put_Description(description.value);
        registration->put_Author(author.value);
    }
    Com<IPrincipal> principal;
    if (SUCCEEDED(definition->get_Principal(principal.Put())) && principal) {
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    }
    Com<ITaskSettings> settings;
    if (SUCCEEDED(definition->get_Settings(settings.Put())) && settings) {
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_Enabled(VARIANT_TRUE);
        Bstr unlimited(L"PT0S");
        settings->put_ExecutionTimeLimit(unlimited.value);
    }
    Com<ITriggerCollection> triggers;
    hr = definition->get_Triggers(triggers.Put());
    Com<ITrigger> trigger;
    if (SUCCEEDED(hr)) hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.Put());
    Com<ILogonTrigger> logon;
    if (SUCCEEDED(hr)) hr = trigger->QueryInterface(kIidLogonTrigger, reinterpret_cast<void**>(logon.Put()));
    if (SUCCEEDED(hr)) { Bstr account(user); hr = logon->put_UserId(account.value); }
    if (FAILED(hr)) { error = L"Could not add the sign-in trigger: " + HResultMessage(hr); return false; }
    Com<IActionCollection> actions;
    hr = definition->get_Actions(actions.Put());
    Com<IAction> action;
    if (SUCCEEDED(hr)) hr = actions->Create(TASK_ACTION_EXEC, action.Put());
    Com<IExecAction> exec;
    if (SUCCEEDED(hr)) hr = action->QueryInterface(kIidExecAction, reinterpret_cast<void**>(exec.Put()));
    if (SUCCEEDED(hr)) { Bstr path(exe); hr = exec->put_Path(path.value); }
    if (SUCCEEDED(hr)) { Bstr arguments(L"--tray"); hr = exec->put_Arguments(arguments.value); }
    if (SUCCEEDED(hr)) { Bstr directory(ExecutableDirectory()); hr = exec->put_WorkingDirectory(directory.value); }
    if (FAILED(hr)) { error = L"Could not add the start action: " + HResultMessage(hr); return false; }
    Bstr name(kTaskName);
    EmptyVariant noUser, noPassword, noSddl;
    Com<IRegisteredTask> registered;
    hr = folder->RegisterTaskDefinition(name.value, definition.value, TASK_CREATE_OR_UPDATE, noUser.value,
                                        noPassword.value, TASK_LOGON_INTERACTIVE_TOKEN, noSddl.value, registered.Put());
    if (FAILED(hr)) {
        error = hr == E_ACCESSDENIED
            ? L"Registering an elevated startup task needs ClickShade itself to run as administrator."
            : L"Could not register the startup task: " + HResultMessage(hr);
        return false;
    }
    return true;
}

bool DeleteScheduledTask(std::wstring& error) {
    bool matches = false;
    if (!QueryScheduledTask(matches)) return true; // Nothing to remove.
    Apartment apartment;
    Com<ITaskService> service; Com<ITaskFolder> folder;
    if (!OpenTaskFolder(apartment, service, folder, error)) return false;
    Bstr name(kTaskName);
    const HRESULT hr = folder->DeleteTask(name.value, 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        error = hr == E_ACCESSDENIED
            ? L"Removing the elevated startup task needs ClickShade itself to run as administrator."
            : L"Could not remove the startup task: " + HResultMessage(hr);
        return false;
    }
    return true;
}

bool RunValueMatches() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    std::vector<wchar_t> value(32768);
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t)), type = 0;
    const LSTATUS result = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                         reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) return false;
    value[value.size() - 1] = 0;
    return std::wstring(value.data()) == Quoted(ExecutablePath()) + L" --tray";
}

bool WriteRunValue(bool enabled, std::wstring& error) {
    HKEY key{};
    LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                   KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) {
        error = L"Could not open your Windows startup settings (" + std::to_wstring(result) + L").";
        return false;
    }
    if (enabled) {
        const auto exe = ExecutablePath();
        if (exe.empty()) { RegCloseKey(key); error = L"Could not locate ClickShade.exe."; return false; }
        const auto command = Quoted(exe) + L" --tray";
        result = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        error = L"Could not update your Windows startup settings (" + std::to_wstring(result) + L").";
        return false;
    }
    return true;
}
} // namespace

bool IsStartupElevated() {
    bool matches = false;
    return QueryScheduledTask(matches) && matches;
}

bool IsStartupEnabled() {
    return RunValueMatches() || IsStartupElevated();
}

bool ConfigureStartup(bool enabled, bool elevated, std::wstring& error) {
    error.clear();
    if (!enabled) {
        // Removing both forms keeps the machine state unambiguous. A missing
        // elevated task is not an error; an undeletable one is reported.
        std::wstring taskError;
        const bool taskRemoved = DeleteScheduledTask(taskError);
        const bool valueRemoved = WriteRunValue(false, error);
        if (!taskRemoved) { error = taskError; return false; }
        return valueRemoved;
    }
    if (elevated) {
        if (!CreateScheduledTask(error)) return false;
        std::wstring ignored;
        WriteRunValue(false, ignored); // The task replaces the ordinary entry.
        return true;
    }
    bool matches = false;
    if (QueryScheduledTask(matches)) {
        std::wstring taskError;
        if (!DeleteScheduledTask(taskError)) {
            error = L"The elevated startup task could not be replaced. " + taskError;
            return false;
        }
    }
    return WriteRunValue(true, error);
}

namespace {
struct IniReader {
    std::wstring path;
    UINT Integer(const wchar_t* key, UINT fallback) const {
        return GetPrivateProfileIntW(L"ClickShade", key, fallback, path.c_str());
    }
    bool Has(const wchar_t* key) const {
        wchar_t probe[8]{};
        GetPrivateProfileStringW(L"ClickShade", key, L"\x01", probe, 8, path.c_str());
        return probe[0] != L'\x01';
    }
    std::wstring Text(const wchar_t* key, const std::wstring& fallback, std::size_t limit = 32768) const {
        std::vector<wchar_t> buffer(limit);
        GetPrivateProfileStringW(L"ClickShade", key, fallback.c_str(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()), path.c_str());
        return buffer.data();
    }
};

void RelocateOldBundledSound(std::wstring& path) {
    if (path.empty()) return;
    std::wstring normalized = path;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    for (const wchar_t* name : {L"soft-left.wav", L"soft-right.wav"}) {
        const std::wstring suffix = std::wstring(L"\\sounds\\") + name;
        if (normalized.size() < suffix.size() ||
            _wcsicmp(normalized.c_str() + normalized.size() - suffix.size(), suffix.c_str()) != 0) continue;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        const auto folder = SoundDirectory();
        if (folder.empty()) return;
        const auto replacement = folder + L"\\soft\\" + name;
        if (FileExists(replacement)) path = replacement;
        return;
    }
}

int Bounded(int value, int minimum, int maximum, bool zeroAllowed = false) {
    if (zeroAllowed && value == 0) return 0;
    return std::clamp(value, minimum, maximum);
}

// CursorScale replaced an absolute CursorSize (24..128 image pixels). An older
// size keeps its look as a scale of the standard 32-pixel cursor.
int LegacyCursorScale(int size) {
    return std::clamp(static_cast<int>(std::lround(size * 100.0 / 32.0)), 100, 600);
}

// Shared by preference loading and legacy (format 1) profile import.
void ApplyLegacyUpgrades(Settings& s, int version, bool hadHighlightStyle, bool highlightFilled,
                         bool hadClickStyle, bool clickFilled) {
    if (!hadHighlightStyle) s.highlightStyle = highlightFilled ? kHighlightFilled : kHighlightRing;
    if (!hadClickStyle) s.clickStyle = clickFilled ? kClickPulse : kClickRing;
    if (version < 2) {
        const Settings defaults;
        if (s.leftClickColor == kLegacyLeftClickColor) s.leftClickColor = defaults.leftClickColor;
        if (s.rightClickColor == kLegacyRightClickColor) s.rightClickColor = defaults.rightClickColor;
    }
}
} // namespace

Settings LoadSettings() {
    Settings s;
    const auto path = SettingsPath();
    if (!path.empty() && FileExists(path)) {
        const IniReader ini{path};
        const auto integer = [&](const wchar_t* key, int fallback) { return static_cast<int>(ini.Integer(key, static_cast<UINT>(fallback))); };
        const auto flag = [&](const wchar_t* key, bool fallback) { return ini.Integer(key, fallback ? 1u : 0u) != 0; };
        const auto color = [&](const wchar_t* key, COLORREF fallback) { return ini.Integer(key, fallback) & 0xffffff; };
        const int version = integer(L"SettingsVersion", 1);
        s.enabled = flag(L"Enabled", s.enabled);
        s.darkCursorEnabled = flag(L"DarkCursorEnabled", s.darkCursorEnabled);
        s.cursorStyle = Bounded(integer(L"CursorStyle", s.cursorStyle), 0, 3);
        s.cursorReverse = flag(L"CursorReverse", s.cursorReverse);
        if (const int scale = integer(L"CursorScale", 0)) s.cursorScale = Bounded(scale, 100, 600);
        else if (const int legacySize = integer(L"CursorSize", 0)) s.cursorScale = LegacyCursorScale(legacySize);
        s.cursorOpacity = Bounded(integer(L"CursorOpacity", s.cursorOpacity), 30, 100);
        s.cursorShadow = flag(L"CursorShadow", s.cursorShadow);
        s.cursorColors = flag(L"CursorColors", s.cursorColors);
        s.cursorFillColor = color(L"CursorFillColor", s.cursorFillColor);
        s.cursorOutlineColor = color(L"CursorOutlineColor", s.cursorOutlineColor);
        s.cursorHold = flag(L"CursorHold", s.cursorHold);
        s.cursorFlashMs = Bounded(integer(L"CursorFlashMs", s.cursorFlashMs), 50, 500);
        s.shakeStyle = Bounded(integer(L"ShakeStyle", s.shakeStyle), 0, 2);
        s.shakeSensitivity = Bounded(integer(L"ShakeSensitivity", s.shakeSensitivity), 50, 200);
        s.shakeMaximumScale = Bounded(integer(L"ShakeMaximumScale", s.shakeMaximumScale), 150, 400);
        s.buttonMask = ini.Integer(L"Buttons", s.buttonMask) & 31;
        s.pauseFullscreen = flag(L"PauseFullscreen", s.pauseFullscreen);
        s.pauseConfinedCursor = flag(L"PauseConfinedCursor", s.pauseConfinedCursor);
        s.startMinimized = flag(L"StartMinimized", s.startMinimized);
        s.runAsAdministrator = flag(L"RunAsAdministrator", s.runAsAdministrator);
        s.captureVisible = flag(L"CaptureVisible", s.captureVisible);
        s.excludedProcesses = ini.Text(L"ExcludedProcesses", L"", 4096);
        s.allowedProcesses = ini.Text(L"AllowedProcesses", L"", 4096);
        s.highlightEnabled = flag(L"HighlightEnabled", s.highlightEnabled);
        const bool hadHighlightStyle = ini.Has(L"HighlightStyle");
        const bool highlightFilled = flag(L"HighlightFilled", true);
        s.highlightStyle = Bounded(integer(L"HighlightStyle", s.highlightStyle), 0, kHighlightStyleCount - 1);
        s.highlightRadius = Bounded(integer(L"HighlightRadius", s.highlightRadius), 8, 100);
        s.highlightOpacity = Bounded(integer(L"HighlightOpacity", s.highlightOpacity), 5, 100);
        s.highlightThickness = Bounded(integer(L"HighlightThickness", s.highlightThickness), 1, 12);
        s.highlightColor = color(L"HighlightColor", s.highlightColor);
        s.highlightBreathing = flag(L"HighlightBreathing", s.highlightBreathing);
        s.highlightIdleHideMs = Bounded(integer(L"HighlightIdleHideMs", s.highlightIdleHideMs), 500, 10000, true);
        s.spotlightDim = Bounded(integer(L"SpotlightDim", s.spotlightDim), 10, 90);
        s.spotlightRadius = Bounded(integer(L"SpotlightRadius", s.spotlightRadius), 40, 400);
        s.trailEnabled = flag(L"TrailEnabled", s.trailEnabled);
        s.trailStyle = Bounded(integer(L"TrailStyle", s.trailStyle), 0, 4);
        s.trailRadius = Bounded(integer(L"TrailRadius", s.trailRadius), 2, 16);
        s.trailOpacity = Bounded(integer(L"TrailOpacity", s.trailOpacity), 5, 100);
        s.trailDurationMs = Bounded(integer(L"TrailDurationMs", s.trailDurationMs), 80, 700);
        s.trailColor = color(L"TrailColor", s.trailColor);
        s.clickEffectsEnabled = flag(L"ClickEffectsEnabled", s.clickEffectsEnabled);
        s.leftClickEnabled = flag(L"LeftClickEnabled", s.leftClickEnabled);
        s.rightClickEnabled = flag(L"RightClickEnabled", s.rightClickEnabled);
        s.middleClickEnabled = flag(L"MiddleClickEnabled", s.middleClickEnabled);
        const bool hadClickStyle = ini.Has(L"ClickStyle");
        const bool clickFilled = flag(L"ClickFilled", false);
        s.clickStyle = Bounded(integer(L"ClickStyle", s.clickStyle), 0, kClickStyleCount - 1);
        s.clickRadius = Bounded(integer(L"ClickRadius", s.clickRadius), 12, 120);
        s.clickDurationMs = Bounded(integer(L"ClickDurationMs", s.clickDurationMs), 100, 1500);
        s.clickOpacity = Bounded(integer(L"ClickOpacity", s.clickOpacity), 5, 100);
        s.clickThickness = Bounded(integer(L"ClickThickness", s.clickThickness), 1, 12);
        s.clickEasing = Bounded(integer(L"ClickEasing", s.clickEasing), 0, 2);
        s.visualRefreshRate = Bounded(integer(L"VisualRefreshRate", s.visualRefreshRate), 30, 360, true);
        s.leftClickColor = color(L"LeftClickColor", s.leftClickColor);
        s.rightClickColor = color(L"RightClickColor", s.rightClickColor);
        s.middleClickColor = color(L"MiddleClickColor", s.middleClickColor);
        s.clickSoundsEnabled = flag(L"ClickSoundsEnabled", s.clickSoundsEnabled);
        s.soundVolume = Bounded(integer(L"SoundVolume", s.soundVolume), 0, 100);
        s.keyboardSoundsEnabled = flag(L"KeyboardSoundsEnabled", s.keyboardSoundsEnabled);
        s.keyboardSoundPath = ini.Text(L"KeyboardSoundPath", L"");
        s.keyboardVolume = Bounded(integer(L"KeyboardVolume", s.keyboardVolume), 0, 100);
        s.magnifierEnabled = flag(L"MagnifierEnabled", s.magnifierEnabled);
        s.magnifierZoom = Bounded(integer(L"MagnifierZoom", s.magnifierZoom), 125, 800);
        s.magnifierSize = Bounded(integer(L"MagnifierSize", s.magnifierSize), 80, 600);
        s.magnifierShape = Bounded(integer(L"MagnifierShape", s.magnifierShape), 0, 1);
        s.magnifierPosition = Bounded(integer(L"MagnifierPosition", s.magnifierPosition), 0, 1);
        s.magnifierBorderColor = color(L"MagnifierBorderColor", s.magnifierBorderColor);
        s.magnifierFrame = flag(L"MagnifierFrame", s.magnifierFrame);
        s.keyDisplayEnabled = flag(L"KeyDisplayEnabled", s.keyDisplayEnabled);
        s.keyDisplayMode = Bounded(integer(L"KeyDisplayMode", s.keyDisplayMode), 0, 1);
        s.keyDisplayMouse = flag(L"KeyDisplayMouse", s.keyDisplayMouse);
        s.keyDisplayPosition = Bounded(integer(L"KeyDisplayPosition", s.keyDisplayPosition), 0, 3);
        s.keyDisplaySize = Bounded(integer(L"KeyDisplaySize", s.keyDisplaySize), 60, 250);
        s.keyDisplayDurationMs = Bounded(integer(L"KeyDisplayDurationMs", s.keyDisplayDurationMs), 300, 6000);
        s.keyDisplayOpacity = Bounded(integer(L"KeyDisplayOpacity", s.keyDisplayOpacity), 20, 100);
        s.keyDisplayColor = color(L"KeyDisplayColor", s.keyDisplayColor);
        s.uiBackground = color(L"UiBackground", s.uiBackground);
        s.uiAccent = color(L"UiAccent", s.uiAccent);
        const bool legacyDark = (GetRValue(s.uiBackground) * 299 + GetGValue(s.uiBackground) * 587 +
                                 GetBValue(s.uiBackground) * 114) < 145000;
        s.uiDarkMode = flag(L"UiDarkMode", legacyDark);
        s.uiDarkBackground = color(L"UiDarkBackground", s.uiDarkBackground);
        s.uiDarkAccent = color(L"UiDarkAccent", s.uiDarkAccent);
        s.uiLightBackground = color(L"UiLightBackground", s.uiLightBackground);
        s.uiLightAccent = color(L"UiLightAccent", s.uiLightAccent);
        // The legacy active pair stays authoritative, including after upgrades.
        if (s.uiDarkMode) { s.uiDarkBackground = s.uiBackground; s.uiDarkAccent = s.uiAccent; }
        else { s.uiLightBackground = s.uiBackground; s.uiLightAccent = s.uiAccent; }
        s.uiScalePercent = Bounded(integer(L"UiScalePercent", s.uiScalePercent), 100, 200, true);
        s.uiAnimatedAccent = flag(L"UiAnimatedAccent", s.uiAnimatedAccent);
        s.leftSoundPath = ini.Text(L"LeftSoundPath", L"");
        s.rightSoundPath = ini.Text(L"RightSoundPath", L"");
        s.middleSoundPath = ini.Text(L"MiddleSoundPath", L"");
        RelocateOldBundledSound(s.leftSoundPath);
        RelocateOldBundledSound(s.rightSoundPath);
        s.releasedCursorPath = ini.Text(L"ReleasedCursorPath", L"");
        s.pressedCursorPath = ini.Text(L"PressedCursorPath", L"");
        const auto shortcut = [&](const wchar_t* key, std::wstring& value) { value = ini.Text(key, value, 96); };
        shortcut(L"HotkeyToggleAll", s.hotkeyToggleAll);
        shortcut(L"HotkeyHighlight", s.hotkeyHighlight);
        shortcut(L"HotkeyClicks", s.hotkeyClicks);
        shortcut(L"HotkeySounds", s.hotkeySounds);
        shortcut(L"HotkeySettings", s.hotkeySettings);
        shortcut(L"HotkeyRestore", s.hotkeyRestore);
        shortcut(L"HotkeyKeyboardSounds", s.hotkeyKeyboardSounds);
        shortcut(L"HotkeyMagnifier", s.hotkeyMagnifier);
        shortcut(L"HotkeyKeyDisplay", s.hotkeyKeyDisplay);
        ApplyLegacyUpgrades(s, version, hadHighlightStyle, highlightFilled, hadClickStyle, clickFilled);
    }
    s.startWithWindows = IsStartupEnabled();
    return s;
}

namespace {
std::wstring SerializeSettings(const Settings& s, bool profile) {
    std::wstring data = profile
        ? L"\xfeff[ClickShadeProfile]\r\nFormatVersion=2\r\nPlatform=Windows\r\n\r\n[ClickShade]\r\n"
        : L"\xfeff[ClickShade]\r\n";
    const auto write = [&](const wchar_t* key, const std::wstring& value) {
        data.append(key).append(L"=").append(value).append(L"\r\n");
    };
    const auto number = [&](const wchar_t* key, long long value) { write(key, std::to_wstring(value)); };
    const auto flag = [&](const wchar_t* key, bool value) { write(key, value ? L"1" : L"0"); };
    number(L"SettingsVersion", kSettingsVersion);
    flag(L"Enabled", s.enabled);
    flag(L"DarkCursorEnabled", s.darkCursorEnabled);
    number(L"CursorStyle", s.cursorStyle);
    flag(L"CursorReverse", s.cursorReverse);
    number(L"CursorScale", s.cursorScale);
    number(L"CursorOpacity", s.cursorOpacity);
    flag(L"CursorShadow", s.cursorShadow);
    flag(L"CursorColors", s.cursorColors);
    number(L"CursorFillColor", s.cursorFillColor);
    number(L"CursorOutlineColor", s.cursorOutlineColor);
    flag(L"CursorHold", s.cursorHold);
    number(L"CursorFlashMs", s.cursorFlashMs);
    number(L"ShakeStyle", s.shakeStyle);
    number(L"ShakeSensitivity", s.shakeSensitivity);
    number(L"ShakeMaximumScale", s.shakeMaximumScale);
    write(L"ReleasedCursorPath", s.releasedCursorPath);
    write(L"PressedCursorPath", s.pressedCursorPath);
    number(L"Buttons", s.buttonMask);
    flag(L"PauseFullscreen", s.pauseFullscreen);
    flag(L"PauseConfinedCursor", s.pauseConfinedCursor);
    flag(L"StartMinimized", s.startMinimized);
    if (profile) flag(L"StartWithWindows", s.startWithWindows);
    flag(L"RunAsAdministrator", s.runAsAdministrator);
    flag(L"CaptureVisible", s.captureVisible);
    write(L"ExcludedProcesses", s.excludedProcesses);
    write(L"AllowedProcesses", s.allowedProcesses);
    flag(L"HighlightEnabled", s.highlightEnabled);
    number(L"HighlightStyle", s.highlightStyle);
    number(L"HighlightRadius", s.highlightRadius);
    number(L"HighlightOpacity", s.highlightOpacity);
    number(L"HighlightThickness", s.highlightThickness);
    number(L"HighlightColor", s.highlightColor);
    flag(L"HighlightBreathing", s.highlightBreathing);
    number(L"HighlightIdleHideMs", s.highlightIdleHideMs);
    number(L"SpotlightDim", s.spotlightDim);
    number(L"SpotlightRadius", s.spotlightRadius);
    flag(L"TrailEnabled", s.trailEnabled);
    number(L"TrailStyle", s.trailStyle);
    number(L"TrailRadius", s.trailRadius);
    number(L"TrailOpacity", s.trailOpacity);
    number(L"TrailDurationMs", s.trailDurationMs);
    number(L"TrailColor", s.trailColor);
    flag(L"ClickEffectsEnabled", s.clickEffectsEnabled);
    flag(L"LeftClickEnabled", s.leftClickEnabled);
    flag(L"RightClickEnabled", s.rightClickEnabled);
    flag(L"MiddleClickEnabled", s.middleClickEnabled);
    number(L"ClickStyle", s.clickStyle);
    number(L"ClickRadius", s.clickRadius);
    number(L"ClickDurationMs", s.clickDurationMs);
    number(L"ClickOpacity", s.clickOpacity);
    number(L"ClickThickness", s.clickThickness);
    number(L"ClickEasing", s.clickEasing);
    number(L"VisualRefreshRate", s.visualRefreshRate);
    number(L"LeftClickColor", s.leftClickColor);
    number(L"RightClickColor", s.rightClickColor);
    number(L"MiddleClickColor", s.middleClickColor);
    flag(L"ClickSoundsEnabled", s.clickSoundsEnabled);
    write(L"LeftSoundPath", s.leftSoundPath);
    write(L"RightSoundPath", s.rightSoundPath);
    write(L"MiddleSoundPath", s.middleSoundPath);
    number(L"SoundVolume", s.soundVolume);
    flag(L"KeyboardSoundsEnabled", s.keyboardSoundsEnabled);
    write(L"KeyboardSoundPath", s.keyboardSoundPath);
    number(L"KeyboardVolume", s.keyboardVolume);
    flag(L"MagnifierEnabled", s.magnifierEnabled);
    number(L"MagnifierZoom", s.magnifierZoom);
    number(L"MagnifierSize", s.magnifierSize);
    number(L"MagnifierShape", s.magnifierShape);
    number(L"MagnifierPosition", s.magnifierPosition);
    number(L"MagnifierBorderColor", s.magnifierBorderColor);
    flag(L"MagnifierFrame", s.magnifierFrame);
    flag(L"KeyDisplayEnabled", s.keyDisplayEnabled);
    number(L"KeyDisplayMode", s.keyDisplayMode);
    flag(L"KeyDisplayMouse", s.keyDisplayMouse);
    number(L"KeyDisplayPosition", s.keyDisplayPosition);
    number(L"KeyDisplaySize", s.keyDisplaySize);
    number(L"KeyDisplayDurationMs", s.keyDisplayDurationMs);
    number(L"KeyDisplayOpacity", s.keyDisplayOpacity);
    number(L"KeyDisplayColor", s.keyDisplayColor);
    number(L"UiBackground", s.uiBackground);
    number(L"UiAccent", s.uiAccent);
    flag(L"UiDarkMode", s.uiDarkMode);
    number(L"UiDarkBackground", s.uiDarkMode ? s.uiBackground : s.uiDarkBackground);
    number(L"UiDarkAccent", s.uiDarkMode ? s.uiAccent : s.uiDarkAccent);
    number(L"UiLightBackground", s.uiDarkMode ? s.uiLightBackground : s.uiBackground);
    number(L"UiLightAccent", s.uiDarkMode ? s.uiLightAccent : s.uiAccent);
    number(L"UiScalePercent", s.uiScalePercent);
    flag(L"UiAnimatedAccent", s.uiAnimatedAccent);
    write(L"HotkeyToggleAll", s.hotkeyToggleAll);
    write(L"HotkeyHighlight", s.hotkeyHighlight);
    write(L"HotkeyClicks", s.hotkeyClicks);
    write(L"HotkeySounds", s.hotkeySounds);
    write(L"HotkeySettings", s.hotkeySettings);
    write(L"HotkeyRestore", s.hotkeyRestore);
    write(L"HotkeyKeyboardSounds", s.hotkeyKeyboardSounds);
    write(L"HotkeyMagnifier", s.hotkeyMagnifier);
    write(L"HotkeyKeyDisplay", s.hotkeyKeyDisplay);
    return data;
}

bool WriteSettingsData(const std::wstring& path, const std::wstring& data, std::wstring& error) {
    error.clear();
    if (path.empty()) { error = L"Choose a settings file first."; return false; }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        error = L"Choose a regular settings file, not a directory or a link.";
        return false;
    }
    // CREATE_NEW prevents a second instance or a pre-existing temporary link
    // from sharing the staging file. It is removed on failure and after rename.
    static std::atomic<unsigned> sequence{0};
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        temporary = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++sequence);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not save the settings file (Windows error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    if (GetFileType(file) != FILE_TYPE_DISK) {
        CloseHandle(file); DeleteFileW(temporary.c_str());
        error = L"Choose a regular settings file, not a device.";
        return false;
    }
    const DWORD bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    DWORD written = 0;
    bool ok = WriteFile(file, data.data(), bytes, &written, nullptr) != FALSE;
    DWORD code = ok ? ERROR_WRITE_FAULT : GetLastError();
    ok = ok && written == bytes;
    if (ok && !FlushFileBuffers(file)) { code = GetLastError(); ok = false; }
    CloseHandle(file);
    if (ok && !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        code = GetLastError(); ok = false;
    }
    if (!ok) {
        DeleteFileW(temporary.c_str());
        error = L"Could not save the settings file (Windows error " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}
} // namespace

bool SaveSettings(const Settings& settings, std::wstring& error) {
    const auto path = SettingsPath();
    if (path.empty()) { error = L"Could not access your local ClickShade settings folder."; return false; }
    if (!WriteSettingsData(path, SerializeSettings(settings, false), error)) return false;
    // The profile API's zero cache-flush return is not a save failure.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

namespace {
constexpr std::size_t kMaximumProfileBytes = 1024 * 1024;
using ProfileValues = std::map<std::wstring, std::wstring>;
struct ProfileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit ProfileHandle(HANDLE handle) : value(handle) {}
    ~ProfileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    ProfileHandle(const ProfileHandle&) = delete;
    ProfileHandle& operator=(const ProfileHandle&) = delete;
};
// sinceVersion2: absent from format 1 profiles. optional: added after format 2
// profiles already existed, so an exported file may lack it in any version.
struct BooleanField { const wchar_t* key; bool Settings::*member; bool sinceVersion2 = false; bool optional = false; };
struct IntegerField { const wchar_t* key; int Settings::*member; unsigned minimum, maximum; bool zero = false; bool sinceVersion2 = false; bool optional = false; };
struct ColorField { const wchar_t* key; COLORREF Settings::*member; bool sinceVersion2 = false; bool optional = false; };
struct StringField { const wchar_t* key; std::wstring Settings::*member; std::size_t maximum; bool sinceVersion2 = false; bool optional = false; };
constexpr BooleanField kProfileBooleans[]{
    {L"Enabled",&Settings::enabled}, {L"DarkCursorEnabled",&Settings::darkCursorEnabled},
    {L"CursorReverse",&Settings::cursorReverse}, {L"CursorHold",&Settings::cursorHold},
    {L"PauseFullscreen",&Settings::pauseFullscreen}, {L"PauseConfinedCursor",&Settings::pauseConfinedCursor},
    {L"StartMinimized",&Settings::startMinimized}, {L"StartWithWindows",&Settings::startWithWindows},
    {L"RunAsAdministrator",&Settings::runAsAdministrator,true}, {L"CaptureVisible",&Settings::captureVisible,true},
    {L"HighlightEnabled",&Settings::highlightEnabled},
    {L"HighlightBreathing",&Settings::highlightBreathing,true},
    {L"TrailEnabled",&Settings::trailEnabled}, {L"ClickEffectsEnabled",&Settings::clickEffectsEnabled},
    {L"LeftClickEnabled",&Settings::leftClickEnabled}, {L"RightClickEnabled",&Settings::rightClickEnabled},
    {L"MiddleClickEnabled",&Settings::middleClickEnabled,true},
    {L"ClickSoundsEnabled",&Settings::clickSoundsEnabled},
    {L"KeyboardSoundsEnabled",&Settings::keyboardSoundsEnabled,true},
    {L"UiDarkMode",&Settings::uiDarkMode}, {L"UiAnimatedAccent",&Settings::uiAnimatedAccent,true},
    {L"MagnifierEnabled",&Settings::magnifierEnabled,true,true}, {L"KeyDisplayEnabled",&Settings::keyDisplayEnabled,true,true},
    {L"KeyDisplayMouse",&Settings::keyDisplayMouse,true,true}, {L"MagnifierFrame",&Settings::magnifierFrame,true,true},
    {L"CursorShadow",&Settings::cursorShadow,true,true}, {L"CursorColors",&Settings::cursorColors,true,true},
};
constexpr IntegerField kProfileIntegers[]{
    {L"CursorStyle",&Settings::cursorStyle,0,3}, {L"CursorScale",&Settings::cursorScale,100,600,false,false,true},
    {L"CursorOpacity",&Settings::cursorOpacity,30,100,false,false,true},
    {L"CursorFlashMs",&Settings::cursorFlashMs,50,500}, {L"ShakeStyle",&Settings::shakeStyle,0,2},
    {L"ShakeSensitivity",&Settings::shakeSensitivity,50,200}, {L"ShakeMaximumScale",&Settings::shakeMaximumScale,150,400},
    {L"HighlightStyle",&Settings::highlightStyle,0,kHighlightStyleCount-1,false,true},
    {L"HighlightRadius",&Settings::highlightRadius,8,100}, {L"HighlightOpacity",&Settings::highlightOpacity,5,100},
    {L"HighlightThickness",&Settings::highlightThickness,1,12},
    {L"HighlightIdleHideMs",&Settings::highlightIdleHideMs,500,10000,true,true},
    {L"SpotlightDim",&Settings::spotlightDim,10,90,false,true}, {L"SpotlightRadius",&Settings::spotlightRadius,40,400,false,true},
    {L"TrailStyle",&Settings::trailStyle,0,4},
    {L"TrailRadius",&Settings::trailRadius,2,16}, {L"TrailOpacity",&Settings::trailOpacity,5,100},
    {L"TrailDurationMs",&Settings::trailDurationMs,80,700},
    {L"ClickStyle",&Settings::clickStyle,0,kClickStyleCount-1,false,true},
    {L"ClickRadius",&Settings::clickRadius,12,120},
    {L"ClickDurationMs",&Settings::clickDurationMs,100,1500}, {L"ClickOpacity",&Settings::clickOpacity,5,100},
    {L"ClickThickness",&Settings::clickThickness,1,12}, {L"ClickEasing",&Settings::clickEasing,0,2},
    {L"VisualRefreshRate",&Settings::visualRefreshRate,30,360,true}, {L"SoundVolume",&Settings::soundVolume,0,100},
    {L"KeyboardVolume",&Settings::keyboardVolume,0,100,false,true},
    {L"UiScalePercent",&Settings::uiScalePercent,100,200,true},
    {L"MagnifierZoom",&Settings::magnifierZoom,125,800,false,true,true}, {L"MagnifierSize",&Settings::magnifierSize,80,600,false,true,true},
    {L"MagnifierShape",&Settings::magnifierShape,0,1,false,true,true}, {L"MagnifierPosition",&Settings::magnifierPosition,0,1,false,true,true},
    {L"KeyDisplayMode",&Settings::keyDisplayMode,0,1,false,true,true}, {L"KeyDisplayPosition",&Settings::keyDisplayPosition,0,3,false,true,true},
    {L"KeyDisplaySize",&Settings::keyDisplaySize,60,250,false,true,true}, {L"KeyDisplayDurationMs",&Settings::keyDisplayDurationMs,300,6000,false,true,true},
    {L"KeyDisplayOpacity",&Settings::keyDisplayOpacity,20,100,false,true,true},
};
constexpr ColorField kProfileColors[]{
    {L"HighlightColor",&Settings::highlightColor}, {L"TrailColor",&Settings::trailColor},
    {L"LeftClickColor",&Settings::leftClickColor}, {L"RightClickColor",&Settings::rightClickColor},
    {L"MiddleClickColor",&Settings::middleClickColor,true},
    {L"UiBackground",&Settings::uiBackground}, {L"UiAccent",&Settings::uiAccent},
    {L"UiDarkBackground",&Settings::uiDarkBackground}, {L"UiDarkAccent",&Settings::uiDarkAccent},
    {L"UiLightBackground",&Settings::uiLightBackground}, {L"UiLightAccent",&Settings::uiLightAccent},
    {L"MagnifierBorderColor",&Settings::magnifierBorderColor,true,true}, {L"KeyDisplayColor",&Settings::keyDisplayColor,true,true},
    {L"CursorFillColor",&Settings::cursorFillColor,true,true}, {L"CursorOutlineColor",&Settings::cursorOutlineColor,true,true},
};
constexpr StringField kProfileStrings[]{
    {L"ReleasedCursorPath",&Settings::releasedCursorPath,32767}, {L"PressedCursorPath",&Settings::pressedCursorPath,32767},
    {L"ExcludedProcesses",&Settings::excludedProcesses,4095}, {L"AllowedProcesses",&Settings::allowedProcesses,4095},
    {L"LeftSoundPath",&Settings::leftSoundPath,32767}, {L"RightSoundPath",&Settings::rightSoundPath,32767},
    {L"MiddleSoundPath",&Settings::middleSoundPath,32767},
    {L"KeyboardSoundPath",&Settings::keyboardSoundPath,32767,true},
    {L"HotkeyToggleAll",&Settings::hotkeyToggleAll,95}, {L"HotkeyHighlight",&Settings::hotkeyHighlight,95},
    {L"HotkeyClicks",&Settings::hotkeyClicks,95}, {L"HotkeySounds",&Settings::hotkeySounds,95},
    {L"HotkeySettings",&Settings::hotkeySettings,95}, {L"HotkeyRestore",&Settings::hotkeyRestore,95},
    {L"HotkeyKeyboardSounds",&Settings::hotkeyKeyboardSounds,95,true},
    {L"HotkeyMagnifier",&Settings::hotkeyMagnifier,95,true,true}, {L"HotkeyKeyDisplay",&Settings::hotkeyKeyDisplay,95,true,true},
};

bool ParseProfileText(const std::wstring& text, ProfileValues& values, int& version, std::wstring& error) {
    ProfileValues metadata;
    std::set<std::wstring> sections;
    ProfileValues* current = nullptr;
    std::size_t offset = !text.empty() && text.front() == 0xfeff ? 1 : 0;
    while (offset < text.size()) {
        const auto end = text.find(L'\n', offset);
        std::wstring line = text.substr(offset, end == std::wstring::npos ? end : end - offset);
        offset = end == std::wstring::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.find_first_of(L"\r\n") != std::wstring::npos || line.find(L'\0') != std::wstring::npos) {
            error = L"The profile contains invalid line data."; return false;
        }
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.front() == L'[') {
            if (!sections.insert(line).second) { error = L"The profile repeats a section."; return false; }
            if (line == L"[ClickShadeProfile]") current = &metadata;
            else if (line == L"[ClickShade]") current = &values;
            else { error = L"The profile contains an unrecognized section."; return false; }
            continue;
        }
        const auto equals = line.find(L'=');
        if (!current || equals == std::wstring::npos || equals == 0 || equals > 64) {
            error = L"The profile is not a complete ClickShade settings file."; return false;
        }
        if (!current->emplace(line.substr(0, equals), line.substr(equals + 1)).second) {
            error = L"The profile repeats a setting: " + line.substr(0, equals) + L"."; return false;
        }
        if (metadata.size() > 2 || values.size() > 160) {
            error = L"The profile contains too many settings."; return false;
        }
    }
    const auto formatVersion = metadata.find(L"FormatVersion");
    if (metadata.size() != 2 || formatVersion == metadata.end() ||
        (formatVersion->second != L"1" && formatVersion->second != L"2")) {
        error = L"This profile version is not supported. Export a profile from ClickShade 1.0 or newer."; return false;
    }
    version = formatVersion->second == L"1" ? 1 : 2;
    if (metadata[L"Platform"] != L"Windows") {
        error = L"Choose a ClickShade profile exported for Windows. Linux profiles use different platform options."; return false;
    }
    return true;
}

bool DecodeProfile(const std::vector<unsigned char>& bytes, std::wstring& text, std::wstring& error) {
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        if (bytes.size() % 2) { error = L"The profile has incomplete Unicode data."; return false; }
        text.reserve(bytes.size() / 2);
        for (std::size_t n = 2; n < bytes.size(); n += 2)
            text.push_back(static_cast<wchar_t>(bytes[n] | (static_cast<unsigned>(bytes[n + 1]) << 8)));
        for (std::size_t n = 0; n < text.size(); ++n) {
            const unsigned code = text[n];
            if (code >= 0xd800 && code <= 0xdbff) {
                if (++n == text.size() || text[n] < 0xdc00 || text[n] > 0xdfff) {
                    error = L"The profile contains invalid Unicode text."; return false;
                }
            } else if (code >= 0xdc00 && code <= 0xdfff) {
                error = L"The profile contains invalid Unicode text."; return false;
            }
        }
        return true;
    }
    const std::size_t start = bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    const int count = static_cast<int>(bytes.size() - start);
    const char* input = reinterpret_cast<const char*>(bytes.data() + start);
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, count, nullptr, 0);
    if (needed <= 0) { error = L"The profile is not valid UTF-8 or UTF-16 text."; return false; }
    text.resize(static_cast<std::size_t>(needed));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, count, text.data(), needed) != needed) {
        error = L"The profile's Unicode text could not be decoded."; return false;
    }
    return true;
}

bool DeserializeProfile(const ProfileValues& values, int version, Settings& output, std::wstring& error) {
    Settings result;
    std::size_t used = 0;
    const bool legacy = version < 2;
    const auto integer = [&](const wchar_t* key, unsigned minimum, unsigned maximum, bool zero, unsigned& number, bool optional) {
        const auto found = values.find(key);
        if (found == values.end()) {
            if (optional) return true;
            error = std::wstring(L"The profile is missing ") + key + L"."; return false;
        }
        ++used; const auto& text = found->second;
        unsigned value = 0;
        if (text.empty() || text.size() > 10) { error = std::wstring(L"Invalid number for ") + key + L"."; return false; }
        for (wchar_t digit : text) {
            if (digit < L'0' || digit > L'9' || value > maximum / 10 ||
                (value == maximum / 10 && static_cast<unsigned>(digit - L'0') > maximum % 10)) {
                error = std::wstring(L"Invalid or out-of-range value for ") + key + L"."; return false;
            }
            value = value * 10 + static_cast<unsigned>(digit - L'0');
        }
        if (value < minimum && !(zero && value == 0)) {
            error = std::wstring(L"Out-of-range value for ") + key + L"."; return false;
        }
        number = value; return true;
    };
    unsigned value = 0;
    for (const auto& field : kProfileBooleans) {
        value = result.*field.member ? 1u : 0u;
        if (!integer(field.key, 0, 1, false, value, field.optional || (legacy && field.sinceVersion2))) return false;
        result.*field.member = value != 0;
    }
    for (const auto& field : kProfileIntegers) {
        value = static_cast<unsigned>(result.*field.member);
        if (!integer(field.key, field.minimum, field.maximum, field.zero, value, field.optional || (legacy && field.sinceVersion2))) return false;
        result.*field.member = static_cast<int>(value);
    }
    // Profiles written before the scale setting carry CursorSize instead.
    unsigned legacySize = 0;
    if (!integer(L"CursorSize", 24, 128, true, legacySize, true)) return false;
    if (legacySize && !values.count(L"CursorScale")) result.cursorScale = LegacyCursorScale(static_cast<int>(legacySize));
    for (const auto& field : kProfileColors) {
        value = result.*field.member;
        if (!integer(field.key, 0, 0xffffff, false, value, field.optional || (legacy && field.sinceVersion2))) return false;
        result.*field.member = value;
    }
    if (!integer(L"Buttons", 0, 31, false, value, false)) return false;
    result.buttonMask = value;
    for (const auto& field : kProfileStrings) {
        const auto found = values.find(field.key);
        if (found == values.end()) {
            if (field.optional || (legacy && field.sinceVersion2)) continue;
            error = std::wstring(L"The profile is missing ") + field.key + L"."; return false;
        }
        ++used;
        if (found->second.size() > field.maximum ||
            std::any_of(found->second.begin(), found->second.end(), [](wchar_t c) { return c < 32; })) {
            error = std::wstring(L"Invalid or oversized text for ") + field.key + L"."; return false;
        }
        result.*field.member = found->second;
    }
    // Format 1 profiles carried the shape switches that became styles, and
    // never had a settings version; apply the same upgrades as a preview-build file.
    bool hadHighlightStyle = true, hadClickStyle = true, highlightFilled = true, clickFilled = false;
    if (legacy) {
        hadHighlightStyle = values.count(L"HighlightStyle") != 0;
        hadClickStyle = values.count(L"ClickStyle") != 0;
        unsigned switchValue = 1;
        if (!integer(L"HighlightFilled", 0, 1, false, switchValue, true)) return false;
        highlightFilled = switchValue != 0;
        switchValue = 0;
        if (!integer(L"ClickFilled", 0, 1, false, switchValue, true)) return false;
        clickFilled = switchValue != 0;
    }
    // The settings-file version travels with exports; it is informational here.
    unsigned ignoredVersion = 0;
    if (!integer(L"SettingsVersion", 1, 99, false, ignoredVersion, true)) return false;
    if (used != values.size()) { error = L"The profile contains an unrecognized setting."; return false; }
    if ((result.uiDarkMode && (result.uiBackground != result.uiDarkBackground || result.uiAccent != result.uiDarkAccent)) ||
        (!result.uiDarkMode && (result.uiBackground != result.uiLightBackground || result.uiAccent != result.uiLightAccent))) {
        error = L"The profile contains conflicting active palette colors."; return false;
    }
    std::set<std::pair<UINT, UINT>> chords;
    for (auto member : {&Settings::hotkeyToggleAll, &Settings::hotkeyHighlight, &Settings::hotkeyClicks,
                        &Settings::hotkeySounds, &Settings::hotkeySettings, &Settings::hotkeyRestore,
                        &Settings::hotkeyKeyboardSounds, &Settings::hotkeyMagnifier, &Settings::hotkeyKeyDisplay}) {
        HotkeyChord chord; std::wstring canonical, detail;
        if (!ParseHotkey(result.*member, chord, canonical, detail)) {
            error = L"Invalid profile shortcut: " + detail; return false;
        }
        if (chord.key && !chords.emplace(chord.modifiers, chord.key).second) {
            error = L"The profile assigns the same shortcut to more than one action."; return false;
        }
        result.*member = std::move(canonical);
    }
    ApplyLegacyUpgrades(result, version, hadHighlightStyle, highlightFilled, hadClickStyle, clickFilled);
    output = std::move(result);
    return true;
}

bool ProfileFilePath(const std::wstring& input, std::wstring& result, std::wstring& error) {
    if (input.empty() || input.size() >= 32768 || input.find_first_of(L"\r\n") != std::wstring::npos ||
        input.find(L'\0') != std::wstring::npos || input.rfind(L"\\\\.\\", 0) == 0 ||
        input.rfind(L"\\\\?\\", 0) == 0 || input.rfind(L"\\??\\", 0) == 0) {
        error = L"Choose a regular profile file path."; return false;
    }
    std::vector<wchar_t> full(32768);
    const DWORD length = GetFullPathNameW(input.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length || length >= full.size()) { error = L"The profile path is too long or invalid."; return false; }
    result.assign(full.data(), length);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    // Recheck after Win32 path normalization: forward/mixed separator forms
    // must not turn a selected file into a device or pipe namespace.
    if (result.rfind(L"\\\\.\\", 0) == 0 || result.rfind(L"\\\\?\\", 0) == 0 ||
        result.rfind(L"\\??\\", 0) == 0) {
        error = L"Choose a regular profile file path."; return false;
    }
    const auto colon = result.find(L':');
    if (colon != std::wstring::npos && (colon != 1 || result.find(L':', colon + 1) != std::wstring::npos)) {
        error = L"Choose a regular profile file, not an alternate data stream."; return false;
    }
    return true;
}
} // namespace

bool SaveSettingsProfile(const std::wstring& requestedPath, const Settings& settings, std::wstring& error) {
    error.clear();
    std::wstring path;
    if (!ProfileFilePath(requestedPath, path, error)) return false;
    const auto data = SerializeSettings(settings, true);
    if (data.size() * sizeof(wchar_t) > kMaximumProfileBytes) { error = L"The profile is too large."; return false; }
    ProfileValues values; Settings verified; int version = 0;
    if (!ParseProfileText(data, values, version, error) || !DeserializeProfile(values, version, verified, error)) return false;
    return WriteSettingsData(path, data, error);
}

bool LoadSettingsProfile(const std::wstring& requestedPath, Settings& settings, std::wstring& error) {
    error.clear();
    std::wstring path;
    if (!ProfileFilePath(requestedPath, path, error)) return false;
    ProfileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        error = L"Could not open the profile (Windows error " + std::to_wstring(GetLastError()) + L")."; return false;
    }
    BY_HANDLE_FILE_INFORMATION information{}; LARGE_INTEGER size{};
    if (GetFileType(file.value) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.value, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaximumProfileBytes)) {
        error = L"Choose a regular, nonempty ClickShade profile smaller than 1 MiB."; return false;
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        error = L"The complete profile could not be read."; return false;
    }
    std::wstring text; ProfileValues values; Settings candidate; int version = 0;
    if (!DecodeProfile(bytes, text, error) || !ParseProfileText(text, values, version, error) ||
        !DeserializeProfile(values, version, candidate, error)) return false;
    settings = std::move(candidate);
    return true;
}

std::wstring SettingsProfileBackupPath(bool beforeImport) {
    const auto directory = SettingsDirectory();
    if (directory.empty()) return {};
    const auto profiles = directory + L"\\profiles";
    if (!EnsureDirectory(profiles)) return {};
    const DWORD attributes = GetFileAttributesW(profiles.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    return profiles + (beforeImport ? L"\\before-import.ini" : L"\\settings-backup.ini");
}
} // namespace clickshade
