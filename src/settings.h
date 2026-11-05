#pragma once
#include "runtime.h"
#include <vector>

namespace clickshade {
// Preferences are an ordinary per-user INI (or a portable INI beside the
// executable when a ClickShade.portable marker file exists). Loading tolerates
// missing keys and upgrades older files; saving is atomic.
Settings LoadSettings();
bool SaveSettings(const Settings& settings, std::wstring& error);
// Profiles are complete, platform-labelled preferences. Loading does not
// change live settings or register startup/shortcuts; the GUI applies them.
bool SaveSettingsProfile(const std::wstring& path, const Settings& settings, std::wstring& error);
bool LoadSettingsProfile(const std::wstring& path, Settings& settings, std::wstring& error);
std::wstring SettingsProfileBackupPath(bool beforeImport);
// Sound and cursor paths that lead to another computer (UNC, WebDAV, a mapped
// network drive) or into a device namespace. Opening one signs in to that
// computer with the user's Windows account, so the GUI lists those of an
// imported profile and keeps them only when the user explicitly agrees.
struct ProfilePathField { const wchar_t* label; std::wstring Settings::* member; };
bool IsNetworkOrDevicePath(const std::wstring& path);
std::vector<ProfilePathField> NetworkProfilePaths(const Settings& settings);

// Startup registration. The ordinary entry is a per-user Run value. When
// "elevated" is requested the entry becomes a per-user Task Scheduler task
// that starts ClickShade with the highest available privileges at sign-in,
// which is the only way an elevated app can start without a UAC prompt.
// Creating or removing that task itself requires an elevated process.
bool ConfigureStartup(bool enabled, bool elevated, std::wstring& error);
bool IsStartupEnabled();
bool IsStartupElevated();
// True when this executable's elevated task came from an older build that
// left Task Scheduler's below-normal priority, so it should be registered again.
bool IsStartupTaskOutdated();
// The elevated task runs ClickShade.exe at high integrity without a prompt,
// so it is only offered when no program without administrator rights can
// change what that start runs: the file, its folder and every parent folder
// are owned by Administrators, SYSTEM or TrustedInstaller and closed to the
// user's standard rights, on a fixed local drive with no link in the path.
// Program Files qualifies; a ZIP extracted to Downloads or the Desktop does not.
bool ExecutableLocationProtected();
// Elevated instances only: removes a ClickShade startup task that launches
// another copy from an unprotected (or missing) location. removed reports
// whether a task was deleted.
bool RemoveUnsafeStartupTask(bool& removed, std::wstring& error);

// Process facts and elevation helpers.
bool IsProcessElevated() noexcept;
// Starts a new elevated instance that waits for this process to exit. The
// caller quits on success. A declined UAC prompt returns false with a message.
// extraArguments (such as --tray) are passed on to the new instance.
bool RelaunchElevated(std::wstring& error, const std::wstring& extraArguments = {});

std::wstring ExecutablePath();
std::wstring ExecutableDirectory();
std::wstring SoundDirectory();
std::wstring KeyboardSoundDirectory();
std::wstring SettingsPath();
std::wstring SettingsDirectory();
std::wstring CrashReportDirectory();
// The same folder without creating it (the crash guard creates it on a crash).
std::wstring CrashReportPath();
bool PortableMode();
}
