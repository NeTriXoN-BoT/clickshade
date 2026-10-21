#pragma once

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace clickshade {

struct HotkeyChord {
    UINT modifiers = 0;
    UINT key = 0;
};

inline std::wstring HotkeyKeyName(UINT key) {
    if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9'))
        return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    switch (key) {
    case VK_SPACE:return L"Space"; case VK_TAB:return L"Tab"; case VK_RETURN:return L"Enter";
    case VK_ESCAPE:return L"Esc"; case VK_BACK:return L"Backspace"; case VK_INSERT:return L"Insert";
    case VK_DELETE:return L"Delete"; case VK_HOME:return L"Home"; case VK_END:return L"End";
    case VK_PRIOR:return L"PageUp"; case VK_NEXT:return L"PageDown";
    case VK_LEFT:return L"Left"; case VK_RIGHT:return L"Right";
    case VK_UP:return L"Up"; case VK_DOWN:return L"Down";
    default:return {};
    }
}

inline std::wstring FormatHotkey(const HotkeyChord& chord) {
    const auto key = HotkeyKeyName(chord.key);
    if (key.empty()) return {};
    std::wstring result;
    if (chord.modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (chord.modifiers & MOD_ALT) result += L"Alt+";
    if (chord.modifiers & MOD_SHIFT) result += L"Shift+";
    if (chord.modifiers & MOD_WIN) result += L"Win+";
    return result + key;
}

// Empty means unassigned. Only explicit combinations with Ctrl/Alt/Win may
// become global shortcuts; ordinary typing and Shift-only keys stay untouched.
inline bool ParseHotkey(const std::wstring& text, HotkeyChord& chord,
                        std::wstring& canonical, std::wstring& error) {
    chord = {}; canonical.clear(); error.clear();
    std::wstring normalized;
    for (wchar_t c : text) if (!std::iswspace(c)) normalized += std::towupper(c);
    if (normalized.empty()) return true;
    if (normalized.size() > 80) { error = L"This shortcut is too long."; return false; }
    std::size_t start = 0;
    while (start < normalized.size()) {
        const auto end = normalized.find(L'+', start);
        const auto token = normalized.substr(start, end == std::wstring::npos ? end : end - start);
        UINT modifier = 0;
        if (token == L"CTRL") modifier = MOD_CONTROL;
        else if (token == L"ALT") modifier = MOD_ALT;
        else if (token == L"SHIFT") modifier = MOD_SHIFT;
        else if (token == L"WIN") modifier = MOD_WIN;
        if (modifier) {
            if (chord.key || (chord.modifiers & modifier)) {
                error = L"Use each modifier once, followed by one key."; return false;
            }
            chord.modifiers |= modifier;
        } else {
            UINT key = 0;
            if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') ||
                                     (token[0] >= L'0' && token[0] <= L'9'))) key = token[0];
            for (UINT candidate = VK_F1; !key && candidate <= VK_F24; ++candidate)
                if (token == L"F" + std::to_wstring(candidate - VK_F1 + 1)) key = candidate;
            for (UINT candidate : {VK_SPACE, VK_TAB, VK_RETURN, VK_ESCAPE, VK_BACK, VK_INSERT,
                                   VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                   VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN}) {
                auto name = HotkeyKeyName(candidate);
                std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towupper(c); });
                if (token == name) key = candidate;
            }
            if (!key || chord.key || end != std::wstring::npos) {
                error = L"Use one letter, number, F1–F24, or a named navigation key."; return false;
            }
            chord.key = key;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start == normalized.size()) { error = L"Choose a key after the modifiers."; return false; }
    }
    if (!chord.key || !(chord.modifiers & (MOD_CONTROL | MOD_ALT | MOD_WIN))) {
        error = L"Include Ctrl, Alt, or Win and one key."; return false;
    }
    // RegisterHotKey reserves F12 for the Windows debugger even when a
    // debugger is not currently running. Reject it before changing bindings.
    if (chord.key == VK_F12) {
        error = L"Windows reserves F12 for debugging. Choose another key.";
        return false;
    }
    canonical = FormatHotkey(chord);
    return true;
}

} // namespace clickshade
