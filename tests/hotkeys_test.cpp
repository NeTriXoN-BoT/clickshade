#include "hotkeys.h"
#include <cstdio>
#include <string>

int wmain() {
    using namespace clickshade;
    unsigned passed=0,failed=0;
    const auto check=[&](bool condition,const char* name) {
        if (condition) ++passed;
        else { ++failed; std::printf("FAIL %s\n",name); }
    };
    HotkeyChord chord; std::wstring canonical,error;
    check(ParseHotkey(L"",chord,canonical,error)&&!chord.key&&canonical.empty(),"empty is unassigned");
    check(ParseHotkey(L" alt + ctrl + shift + win + f10 ",chord,canonical,error)&&
          canonical==L"Ctrl+Alt+Shift+Win+F10"&&chord.key==VK_F10&&
          chord.modifiers==(MOD_CONTROL|MOD_ALT|MOD_SHIFT|MOD_WIN),"canonical modifiers and case");
    for (UINT key=VK_F1;key<=VK_F24;++key) {
        const std::wstring text=L"Ctrl+"+HotkeyKeyName(key);
        if (key == VK_F12)
            check(!ParseHotkey(text,chord,canonical,error)&&!error.empty()&&canonical.empty(),
                  "Windows-reserved F12 is rejected before registration");
        else
            check(ParseHotkey(text,chord,canonical,error)&&chord.key==key&&canonical==text,"supported function keys");
    }
    for (UINT key : {VK_SPACE,VK_TAB,VK_RETURN,VK_ESCAPE,VK_BACK,VK_INSERT,VK_DELETE,
                     VK_HOME,VK_END,VK_PRIOR,VK_NEXT,VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN}) {
        const std::wstring text=L"Win+"+HotkeyKeyName(key);
        check(ParseHotkey(text,chord,canonical,error)&&canonical==text&&chord.key==key,"named key roundtrip");
    }
    for (wchar_t key : std::wstring(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
        const std::wstring text=L"Alt+"+std::wstring(1,key);
        check(ParseHotkey(text,chord,canonical,error)&&canonical==text,"letter and digit roundtrip");
    }
    for (const wchar_t* text : {L"A",L"Shift+A",L"F10",L"Ctrl",L"Ctrl+",L"+A",L"Ctrl++A",
            L"Ctrl+Ctrl+A",L"Ctrl+A+B",L"Ctrl+A+Shift",L"Ctrl+F0",L"Ctrl+F25",L"Ctrl+F01",
            L"Ctrl+Mouse1",L"Ctrl+???",L"Ctrl+Enter+",L"Ctrl+F12x",L"Alt+F12",L"Win+Shift+F12"}) {
        check(!ParseHotkey(text,chord,canonical,error)&&!error.empty(),"invalid or typing-intercepting chord rejected");
    }
    check(!ParseHotkey(std::wstring(100,L'A'),chord,canonical,error),"overlong input bounded");
    check(FormatHotkey({MOD_CONTROL,VK_SHIFT}).empty(),"modifier is not a primary key");
    std::printf("Hotkey parser: %u passed, %u failed.\n",passed,failed);
    return failed?1:0;
}
