// Headless checks of the fullscreen game classification. Only games pause in
// fullscreen; desktop applications keep their effects.
#include "../game_detection.h"

#include <cstdio>

using namespace clickshade::linuxapp;
namespace {
unsigned passed = 0, failed = 0;
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    if (value) ++passed; else ++failed;
}
ProcessIdentity Native(const char* executable) { return {executable, {executable}, {"steam", "systemd"}, {}}; }
} // namespace

int main() {
    Check(LooksLikeGame(Native("/home/user/.local/share/Steam/steamapps/common/Portal 2/portal2_linux")),
        "a native Steam game runs from steamapps/common");
    Check(LooksLikeGame(Native("/mnt/Games/SteamLibrary/SteamApps/common/Game/game.x86_64")),
        "a second Steam library folder in any letter case is recognized");
    Check(LooksLikeGame({"/usr/bin/python3.12", {"python3", "/home/user/.steam/steam/steamapps/common/Game/main.py"}, {}, {}}),
        "an interpreted game is recognized by its script folder");
    Check(LooksLikeGame({"/opt/proton/files/bin/wine64-preloader", {"C:\\windows\\system32\\steam.exe"}, {}, {}}),
        "a Proton program runs in wine64-preloader");
    Check(LooksLikeGame({"/usr/bin/wine-preloader", {}, {}, {}}), "32-bit Wine programs run in wine-preloader");
    Check(LooksLikeGame({"/usr/lib/wine/wine64", {}, {}, {}}), "new-style Wine programs run in wine64");
    Check(LooksLikeGame({"", {"Z:\\home\\user\\Games\\Setup\\Game.EXE", "-windowed"}, {}, {}}),
        "a Windows program shows its .exe among the first arguments");
    Check(LooksLikeGame({"/usr/bin/gamescope", {"gamescope", "-f", "--", "%command%"}, {}, {}}),
        "a gamescope window is a game");
    Check(LooksLikeGame({"/usr/lib/game/bin/game", {"game"}, {"reaper", "gamescope-wl"}, {}}),
        "a process started inside gamescope is a game");
    Check(LooksLikeGame({"", {}, {}, {"steam_app_1091500", "steam_app_1091500"}}),
        "Steam's steam_app_ window class marks a game without a process");
    Check(LooksLikeGame(Native("/home/user/Games/Heroic/Hades/Hades.x86_64")), "Heroic's default game folder is recognized");
    Check(LooksLikeGame(Native("/home/user/.local/share/lutris/runners/dosbox/bin/dosbox")), "Lutris runners are recognized");
    Check(LooksLikeGame(Native("/home/user/.config/itch/apps/celeste/Celeste")), "itch app installs are recognized");
    Check(LooksLikeGame(Native("/data/itch-apps/game/game")), "an extra itch install location is recognized");

    Check(!LooksLikeGame({"/usr/lib/firefox/firefox", {"/usr/lib/firefox/firefox", "-new-window"}, {"systemd"}, {"Navigator", "firefox"}}),
        "a fullscreen browser is not a game");
    Check(!LooksLikeGame({"/usr/bin/mpv", {"mpv", "--fs", "holiday.mkv"}, {"bash"}, {"mpv", "mpv"}}),
        "a fullscreen video player is not a game");
    Check(!LooksLikeGame({"/usr/lib/libreoffice/program/soffice.bin", {"/usr/lib/libreoffice/program/soffice.bin", "--impress", "talk.odp"}, {}, {"libreoffice", "libreoffice-impress"}}),
        "a presentation is not a game");
    Check(!LooksLikeGame({"/home/user/.local/share/Steam/ubuntu12_32/steam", {"steam", "-silent"}, {}, {"steam", "Steam"}}),
        "the Steam client itself is not a game");
    Check(!LooksLikeGame({"/usr/bin/vlc", {"vlc", "--fullscreen", "setup.exe.mp4", "later/notes.exe"}, {}, {"vlc", "vlc"}}),
        "an .exe named only among later arguments does not make a game");
    Check(!LooksLikeGame({}), "an unknown process is not a game");

    std::printf("\nGame detection: %u passed, %u failed.\n", passed, failed);
    return failed ? 1 : 0;
}
