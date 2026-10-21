#pragma once
// Key-display helpers shared by the input thread (which decides whether a
// press is shown) and the renderer (which formats the label). Every function
// works on one event at a time; nothing is stored, logged or written.

#include "runtime.h"
#include <cwctype>
#include <string>
#include <vector>

namespace clickshade {

inline bool IsModifierKey(unsigned virtualKey) noexcept {
    switch (virtualKey) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

inline unsigned ModifierBit(unsigned virtualKey) noexcept {
    switch (virtualKey) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return kModControl;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return kModShift;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return kModAlt;
    case VK_LWIN: case VK_RWIN: return kModWin;
    default: return 0;
    }
}

// Shortcuts-only mode (0) shows chords with Ctrl, Alt or Win, function keys,
// Esc, Print Screen and a tapped modifier on its own (Win opening Start).
// AltGr chords type characters on many layouts, so they count as typing.
// Mouse labels have their own switch and pass here unconditionally.
inline bool KeyDisplayWanted(int mode, unsigned virtualKey, unsigned modifiers) noexcept {
    if (modifiers & kModMouse) return true;
    if (mode != 0) return true;
    if (modifiers & kModAltGr) return false;
    if (modifiers & (kModControl | kModAlt | kModWin)) return true;
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) return true;
    return virtualKey == VK_ESCAPE || virtualKey == VK_SNAPSHOT || IsModifierKey(virtualKey);
}

// A short label for one key. Letters and symbols come from the given keyboard
// layout; named keys use fixed names so labels look the same on every layout.
inline std::wstring KeyName(unsigned virtualKey, unsigned scanCode, bool extended, unsigned modifiers, HKL layout) {
    if (modifiers & kModMouse) {
        switch (virtualKey) {
        case VK_LBUTTON: return L"Left click";
        case VK_RBUTTON: return L"Right click";
        case VK_MBUTTON: return L"Middle click";
        case VK_XBUTTON1: return L"Back button";
        case VK_XBUTTON2: return L"Forward button";
        default: return L"Mouse";
        }
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
        return std::wstring(L"Num ") + static_cast<wchar_t>(L'0' + (virtualKey - VK_NUMPAD0));
    switch (virtualKey) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return L"Ctrl";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return L"Shift";
    case VK_MENU: case VK_LMENU: case VK_RMENU:
        return (modifiers & kModAltGr) ? L"AltGr" : (extended || virtualKey == VK_RMENU) ? L"Right Alt" : L"Alt";
    case VK_LWIN: case VK_RWIN: return L"Win";
    case VK_ESCAPE: return L"Esc";
    case VK_TAB: return L"Tab";
    case VK_CAPITAL: return L"Caps Lock";
    case VK_RETURN: return L"Enter";
    case VK_BACK: return L"Backspace";
    case VK_SPACE: return L"Space";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"←";
    case VK_UP: return L"↑";
    case VK_RIGHT: return L"→";
    case VK_DOWN: return L"↓";
    case VK_CLEAR: return L"Clear";
    case VK_SNAPSHOT: return L"Print Screen";
    case VK_SCROLL: return L"Scroll Lock";
    case VK_PAUSE: return L"Pause";
    case VK_NUMLOCK: return L"Num Lock";
    case VK_APPS: return L"Menu";
    case VK_MULTIPLY: return L"Num *";
    case VK_ADD: return L"Num +";
    case VK_SUBTRACT: return L"Num -";
    case VK_DECIMAL: return L"Num .";
    case VK_DIVIDE: return L"Num /";
    case VK_VOLUME_MUTE: return L"Mute";
    case VK_VOLUME_DOWN: return L"Volume Down";
    case VK_VOLUME_UP: return L"Volume Up";
    case VK_MEDIA_NEXT_TRACK: return L"Next Track";
    case VK_MEDIA_PREV_TRACK: return L"Previous Track";
    case VK_MEDIA_STOP: return L"Stop";
    case VK_MEDIA_PLAY_PAUSE: return L"Play/Pause";
    case VK_BROWSER_BACK: return L"Back";
    case VK_BROWSER_FORWARD: return L"Forward";
    case VK_SLEEP: return L"Sleep";
    default: break;
    }
    // Printable keys: the layout's base character, shown in upper case. Dead
    // keys set the high bit; the character itself is still the right label.
    const UINT character = MapVirtualKeyExW(virtualKey, MAPVK_VK_TO_CHAR, layout) & 0xffffu;
    if (character >= 0x20 && character != 0x7f)
        return std::wstring(1, static_cast<wchar_t>(std::towupper(static_cast<wint_t>(character))));
    wchar_t name[64]{};
    const LONG parameter = static_cast<LONG>((scanCode & 0xffu) << 16) | (extended ? (1L << 24) : 0L);
    if (scanCode && GetKeyNameTextW(parameter, name, 64) > 0) return name;
    return L"Key";
}

// The keys of a chord in display order: modifiers first (Windows' own order:
// Win, Ctrl, Alt, Shift), then the key. A modifier that is itself the key is
// not repeated, and AltGr replaces its synthetic Ctrl and Alt pair.
inline std::vector<std::wstring> KeyChordParts(unsigned virtualKey, unsigned scanCode, bool extended, unsigned modifiers, HKL layout) {
    std::vector<std::wstring> parts;
    unsigned own = (modifiers & kModMouse) ? 0u : ModifierBit(virtualKey);
    if (own & kModAlt) own |= kModAltGr; // "AltGr" is the key's own name then.
    const unsigned shown = modifiers & ~own;
    if (shown & kModWin) parts.emplace_back(L"Win");
    if (shown & kModAltGr) parts.emplace_back(L"AltGr");
    else {
        if (shown & kModControl) parts.emplace_back(L"Ctrl");
        if (shown & kModAlt) parts.emplace_back(L"Alt");
    }
    if (shown & kModShift) parts.emplace_back(L"Shift");
    parts.push_back(KeyName(virtualKey, scanCode, extended, modifiers, layout));
    return parts;
}

// The character a key types with the given Shift, AltGr and Caps Lock state,
// or nothing for shortcuts, mouse buttons, dead keys and control keys. The
// keyboard state of the system is never changed (ToUnicodeEx flag 4).
inline std::wstring TypedText(unsigned virtualKey, unsigned scanCode, unsigned modifiers, HKL layout) {
    if (modifiers & (kModControl | kModAlt | kModWin | kModMouse)) return {};
    if (virtualKey == VK_SPACE) return L" ";
    if (IsModifierKey(virtualKey) || virtualKey == VK_CAPITAL) return {};
    BYTE state[256]{};
    if (modifiers & kModShift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (modifiers & kModAltGr) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    if (modifiers & kModCapsLock) state[VK_CAPITAL] = 0x01;
    wchar_t buffer[8]{};
    const int count = ToUnicodeEx(virtualKey, scanCode, state, buffer, 8, 0x4, layout);
    if (count <= 0) return {};
    std::wstring text(buffer, static_cast<std::size_t>(count));
    for (wchar_t c : text)
        if (c < 0x20 || c == 0x7f) return {};
    return text;
}

} // namespace clickshade
