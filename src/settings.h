#pragma once
#include "runtime.h"

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

// Startup registration. The ordinary entry is a per-user Run value. When
// "elevated" is requested the entry becomes a per-user Task Scheduler task
// that starts ClickShade with the highest available privileges at sign-in,
// which is the only way an elevated app can start without a UAC prompt.
// Creating or removing that task itself requires an elevated process.
bool ConfigureStartup(bool enabled, bool elevated, std::wstring& error);
bool IsStartupEnabled();
bool IsStartupElevated();

// Process facts and elevation helpers.
bool IsProcessElevated() noexcept;
// Starts a new elevated instance that waits for this process to exit. The
// caller quits on success. A declined UAC prompt returns false with a message.
bool RelaunchElevated(std::wstring& error);

std::wstring ExecutablePath();
std::wstring ExecutableDirectory();
std::wstring SoundDirectory();
std::wstring KeyboardSoundDirectory();
std::wstring SettingsPath();
std::wstring SettingsDirectory();
std::wstring CrashReportDirectory();
bool PortableMode();
}
