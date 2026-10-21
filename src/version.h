#pragma once

// Single source for the product version string used by the GUI, crash
// reports and the --version output. The resource script, manifest, CMake and
// the release packager carry the same number; change them together.
#define CLICKSHADE_VERSION "1.0"
#define CLICKSHADE_VERSION_W L"1.0"

namespace clickshade {
inline constexpr char kClickShadeVersion[] = CLICKSHADE_VERSION;
inline constexpr wchar_t kClickShadeVersionW[] = CLICKSHADE_VERSION_W;
// Filled in when the public repository exists; shown as a link in About.
inline constexpr wchar_t kClickShadeHomepage[] = L"https://github.com/NeTriXoN-BoT/clickshade";
}
