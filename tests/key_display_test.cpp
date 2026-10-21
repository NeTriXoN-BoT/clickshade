// Headless checks for the key display's labels and its shortcuts-only policy.
#include "key_display.h"

#include <cstdio>
#include <string>

namespace {
int passed = 0, failed = 0;

void Check(bool condition, const char* description) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    condition ? ++passed : ++failed;
}

std::string Narrow(const std::wstring& text) {
    std::string result;
    for (wchar_t c : text) result += c < 128 ? static_cast<char>(c) : '?';
    return result;
}

// "Win + Shift + S" style text of the keycaps the display draws for a chord.
std::wstring ChordText(unsigned key, unsigned scan, bool extended, unsigned modifiers, HKL layout) {
    std::wstring result;
    for (const auto& part : clickshade::KeyChordParts(key, scan, extended, modifiers, layout)) {
        if (!result.empty()) result += L" + ";
        result += part;
    }
    return result;
}

bool Label(unsigned key, unsigned scan, bool extended, unsigned modifiers, const wchar_t* expected, HKL layout) {
    const auto actual = ChordText(key, scan, extended, modifiers, layout);
    if (actual != expected) std::printf("INFO expected \"%s\" but got \"%s\"\n", Narrow(expected).c_str(), Narrow(actual).c_str());
    return actual == expected;
}
} // namespace

int wmain() {
    using namespace clickshade;
    const HKL layout = GetKeyboardLayout(0);
    Check(Label('A', 0x1e, false, 0, L"A", layout), "a letter key is labelled with its upper-case character");
    Check(Label('7', 0x08, false, 0, L"7", layout), "a digit key is labelled with the digit");
    Check(Label(VK_F5, 0x3f, false, 0, L"F5", layout), "function keys use their F names");
    Check(Label(VK_F24, 0, false, 0, L"F24", layout), "the full function-key range is covered");
    Check(Label(VK_RETURN, 0x1c, false, 0, L"Enter", layout) && Label(VK_RETURN, 0x1c, true, 0, L"Enter", layout),
          "both Enter keys share one label");
    Check(Label(VK_RIGHT, 0x4d, true, 0, L"→", layout) && Label(VK_UP, 0x48, true, 0, L"↑", layout),
          "arrow keys are shown as arrows");
    Check(Label(VK_NUMPAD5, 0x4c, false, 0, L"Num 5", layout) && Label(VK_ADD, 0x4e, false, 0, L"Num +", layout),
          "numeric keypad keys are marked as Num");
    Check(Label(VK_SNAPSHOT, 0x37, true, 0, L"Print Screen", layout) && Label(VK_ESCAPE, 0x01, false, 0, L"Esc", layout) &&
          Label(VK_SPACE, 0x39, false, 0, L"Space", layout) && Label(VK_BACK, 0x0e, false, 0, L"Backspace", layout),
          "named keys use fixed English names");
    Check(Label('S', 0x1f, false, kModControl | kModShift, L"Ctrl + Shift + S", layout),
          "modifiers are listed before the key in Ctrl, Alt, Shift order");
    Check(Label('S', 0x1f, false, kModWin | kModShift, L"Win + Shift + S", layout), "the Windows key comes first");
    Check(Label(VK_DELETE, 0x53, true, kModControl | kModAlt, L"Ctrl + Alt + Delete", layout), "three-key chords read naturally");
    Check(Label('Q', 0x10, false, kModAltGr, L"AltGr + Q", layout), "AltGr replaces its synthetic Ctrl and Alt pair");
    Check(Label(VK_LWIN, 0x5b, true, 0, L"Win", layout) && Label(VK_CONTROL, 0x1d, false, 0, L"Ctrl", layout),
          "a tapped modifier is labelled by itself");
    Check(Label(VK_SHIFT, 0x36, false, kModControl | kModShift, L"Ctrl + Shift", layout),
          "a released modifier chord does not repeat the key's own modifier");
    Check(Label(VK_RMENU, 0x38, true, kModAltGr, L"AltGr", layout), "a tapped AltGr is labelled AltGr once");
    Check(Label(VK_LBUTTON, 0, false, kModMouse, L"Left click", layout) &&
          Label(VK_RBUTTON, 0, false, kModMouse | kModControl, L"Ctrl + Right click", layout) &&
          Label(VK_XBUTTON2, 0, false, kModMouse, L"Forward button", layout),
          "mouse buttons get their own labels, with modifiers");
    Check(!KeyName(0xe7, 0, false, 0, layout).empty(), "an unknown key still produces a nonempty label");
    {
        const auto parts = KeyChordParts('S', 0x1f, false, kModWin | kModControl | kModShift, layout);
        Check(parts.size() == 4 && parts[0] == L"Win" && parts[1] == L"Ctrl" && parts[2] == L"Shift" && parts[3] == L"S",
              "keycaps list the modifiers in Windows order, then the key");
        const auto altGr = KeyChordParts('Q', 0x10, false, kModAltGr, layout);
        Check(altGr.size() == 2 && altGr[0] == L"AltGr", "AltGr is one keycap, not Ctrl and Alt");
        Check(KeyChordParts(VK_LWIN, 0x5b, true, 0, layout).size() == 1, "a tapped modifier is a single keycap");
    }
    Check(TypedText('A', 0x1e, 0, layout) == L"a" && TypedText('A', 0x1e, kModShift, layout) == L"A",
          "typed letters follow Shift");
    Check(TypedText('A', 0x1e, kModCapsLock, layout) == L"A" && TypedText('A', 0x1e, kModCapsLock | kModShift, layout) == L"a",
          "typed letters follow Caps Lock, and Shift reverses it");
    Check(TypedText(VK_SPACE, 0x39, 0, layout) == L" ", "Space types a space");
    Check(TypedText('A', 0x1e, kModControl, layout).empty() && TypedText('A', 0x1e, kModAlt, layout).empty() &&
          TypedText('A', 0x1e, kModWin, layout).empty(), "shortcuts type nothing");
    Check(TypedText(VK_RETURN, 0x1c, 0, layout).empty() && TypedText(VK_BACK, 0x0e, 0, layout).empty() &&
          TypedText(VK_ESCAPE, 0x01, 0, layout).empty() && TypedText(VK_TAB, 0x0f, 0, layout).empty(),
          "Enter, Backspace, Esc and Tab are keys, not typed characters");
    Check(TypedText(VK_F5, 0x3f, 0, layout).empty() && TypedText(VK_LEFT, 0x4b, 0, layout).empty() &&
          TypedText(VK_LSHIFT, 0x2a, kModShift, layout).empty() && TypedText(VK_CAPITAL, 0x3a, 0, layout).empty(),
          "function, arrow and modifier keys type nothing");
    Check(TypedText(VK_LBUTTON, 0, kModMouse, layout).empty(), "mouse buttons type nothing");
    Check(TypedText('7', 0x08, 0, layout) == L"7", "digits type themselves");

    Check(KeyDisplayWanted(0, 'S', kModControl) && KeyDisplayWanted(0, 'S', kModAlt | kModShift) && KeyDisplayWanted(0, 'S', kModWin),
          "shortcuts-only shows chords with Ctrl, Alt or Win");
    Check(!KeyDisplayWanted(0, 'S', 0) && !KeyDisplayWanted(0, 'S', kModShift) && !KeyDisplayWanted(0, VK_SPACE, 0) &&
          !KeyDisplayWanted(0, VK_RETURN, 0) && !KeyDisplayWanted(0, VK_BACK, 0),
          "shortcuts-only hides typing, including shifted letters, Space, Enter and Backspace");
    Check(KeyDisplayWanted(0, VK_F2, 0) && KeyDisplayWanted(0, VK_F11, kModShift) && KeyDisplayWanted(0, VK_ESCAPE, 0) &&
          KeyDisplayWanted(0, VK_SNAPSHOT, 0), "shortcuts-only shows function keys, Esc and Print Screen on their own");
    Check(!KeyDisplayWanted(0, 'Q', kModAltGr) && !KeyDisplayWanted(0, '2', kModAltGr | kModShift),
          "shortcuts-only treats AltGr characters as typing");
    Check(KeyDisplayWanted(0, VK_LWIN, 0) && KeyDisplayWanted(0, VK_CONTROL, 0), "shortcuts-only shows a tapped modifier");
    Check(KeyDisplayWanted(0, VK_LBUTTON, kModMouse), "mouse labels bypass the shortcuts filter (they have their own switch)");
    Check(KeyDisplayWanted(1, 'S', 0) && KeyDisplayWanted(1, VK_SPACE, 0) && KeyDisplayWanted(1, 'Q', kModAltGr),
          "every-key mode shows everything");
    Check(IsModifierKey(VK_LSHIFT) && IsModifierKey(VK_RWIN) && IsModifierKey(VK_MENU) && !IsModifierKey('A') && !IsModifierKey(VK_CAPITAL),
          "modifier detection covers Ctrl, Shift, Alt and Win variants only");
    Check(ModifierBit(VK_RCONTROL) == kModControl && ModifierBit(VK_LMENU) == kModAlt && ModifierBit(VK_TAB) == 0,
          "modifier bits map left and right variants to one flag");
    std::printf("RESULT %d passed, %d failed; labels and policy only, no input observation.\n", passed, failed);
    return failed ? 1 : 0;
}
