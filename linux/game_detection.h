#pragma once

#include <array>
#include <string>
#include <vector>

namespace clickshade::linuxapp {

// What a foreground window's process looks like from /proc and WM_CLASS.
struct ProcessIdentity {
    std::string executable;                 // /proc/<pid>/exe target
    std::vector<std::string> arguments;     // /proc/<pid>/cmdline
    std::vector<std::string> ancestors;     // comm names of the parent processes
    std::vector<std::string> windowClasses; // WM_CLASS instance and class
};

namespace detail {
inline std::string LowerAscii(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
inline std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
inline bool StartsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
inline bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
// Library folders of Steam, Heroic, Lutris and the itch app. Paths compare
// with forward slashes and in lower case.
inline bool InGameFolder(std::string path) {
    path = LowerAscii(std::move(path));
    for (char& c : path) if (c == '\\') c = '/';
    static constexpr std::array<const char*, 6> folders{{
        "/steamapps/common/", "/games/heroic/", "/.local/share/lutris/", "/.itch/apps/", "/itch/apps/", "/itch-apps/"}};
    for (const char* folder : folders) if (path.find(folder) != std::string::npos) return true;
    return false;
}
} // namespace detail

// Fullscreen pause is meant for games: a fullscreen browser, video player or
// presentation keeps its effects. A game runs from a Steam, Heroic, Lutris or
// itch library folder, is a Wine/Proton program, runs under gamescope, or
// carries the steam_app_ window class Steam gives its games.
inline bool LooksLikeGame(const ProcessIdentity& process) {
    using namespace detail;
    const auto executable = LowerAscii(BaseName(process.executable));
    if (InGameFolder(process.executable)) return true;
    if (executable == "wine" || executable == "wine64" || executable == "wine-preloader" || executable == "wine64-preloader") return true;
    if (StartsWith(executable, "gamescope")) return true;
    // An interpreted game names its script as the first argument; Wine shows
    // the Windows program itself there. Later arguments are mostly options.
    for (std::size_t i = 0; i < process.arguments.size() && i < 2; ++i) {
        if (InGameFolder(process.arguments[i])) return true;
        if (EndsWith(LowerAscii(process.arguments[i]), ".exe")) return true;
    }
    for (const auto& ancestor : process.ancestors) if (StartsWith(LowerAscii(ancestor), "gamescope")) return true;
    for (const auto& name : process.windowClasses) if (StartsWith(LowerAscii(name), "steam_app_")) return true;
    return false;
}

} // namespace clickshade::linuxapp
