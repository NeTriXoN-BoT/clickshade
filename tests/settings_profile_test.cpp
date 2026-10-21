#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned passed=0, failed=0;
void Check(bool condition,const char* label) {
    std::printf("%s %s\n",condition?"PASS":"FAIL",label);
    condition?++passed:++failed;
}
void Require(bool condition,const char* label) {
    Check(condition,label); if(!condition) throw std::runtime_error(label);
}
struct Fixture {
    std::wstring directory;
    Fixture() {
        wchar_t temporary[32768]{};
        const DWORD length=GetTempPathW(static_cast<DWORD>(std::size(temporary)),temporary);
        if(!length||length>=std::size(temporary)) throw std::runtime_error("temp folder");
        directory=std::wstring(temporary)+L"ClickShade-profile-test-"+std::to_wstring(GetCurrentProcessId());
        if(!CreateDirectoryW(directory.c_str(),nullptr)) throw std::runtime_error("test folder creation");
    }
    ~Fixture() {
        WIN32_FIND_DATAW item{};
        const HANDLE search=FindFirstFileW((directory+L"\\*").c_str(),&item);
        if(search!=INVALID_HANDLE_VALUE) {
            do { if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((directory+L"\\"+item.cFileName).c_str()); }
            while(FindNextFileW(search,&item));
            FindClose(search);
        }
        RemoveDirectoryW(directory.c_str());
    }
    std::wstring Path(const wchar_t* name) const { return directory+L"\\"+name; }
};
std::vector<unsigned char> Read(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("test read open");
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(file,&length)||length.QuadPart<0||length.QuadPart>2*1024*1024) {
        CloseHandle(file); throw std::runtime_error("test read length");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length.QuadPart)); DWORD count=0;
    const bool okay=ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)&&count==bytes.size();
    CloseHandle(file); if(!okay) throw std::runtime_error("test read data"); return bytes;
}
void Write(const std::wstring& path,const void* data,std::size_t length) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("test write open");
    DWORD count=0; const bool okay=WriteFile(file,data,static_cast<DWORD>(length),&count,nullptr)&&count==length;
    CloseHandle(file); if(!okay) throw std::runtime_error("test write data");
}
std::wstring Wide(const std::vector<unsigned char>& bytes) {
    std::wstring result;
    for(std::size_t n=0;n+1<bytes.size();n+=2) result.push_back(static_cast<wchar_t>(bytes[n]|(bytes[n+1]<<8)));
    return result;
}
std::wstring Replace(std::wstring text,const std::wstring& from,const std::wstring& to) {
    const auto offset=text.find(from); if(offset==std::wstring::npos) throw std::runtime_error("missing fixture key");
    text.replace(offset,from.size(),to); return text;
}
clickshade::Settings Example() {
    clickshade::Settings s;
    s.enabled=false; s.darkCursorEnabled=false; s.cursorStyle=3; s.cursorReverse=true;
    s.cursorScale=400; s.cursorOpacity=55; s.cursorShadow=true; s.cursorColors=true;
    s.cursorFillColor=RGB(13,14,15); s.cursorOutlineColor=RGB(16,17,18); s.cursorHold=false; s.cursorFlashMs=500;
    s.shakeStyle=2; s.shakeSensitivity=200; s.shakeMaximumScale=150;
    s.releasedCursorPath=L"C:\\profiles\\entrée 鼠标\\arrow=normal.cur";
    s.pressedCursorPath=L"D:\\custom\\arrow-pressed.cur";
    s.buttonMask=31; s.pauseFullscreen=false; s.pauseConfinedCursor=false;
    s.startWithWindows=true; s.startMinimized=true;
    s.excludedProcesses=L"game.exe; player.exe"; s.allowedProcesses=L"editor.exe; taskmgr.exe";
    s.highlightEnabled=false; s.highlightStyle=clickshade::kHighlightCrosshair; s.highlightRadius=100;
    s.highlightOpacity=5; s.highlightThickness=12; s.highlightColor=RGB(8,22,91);
    s.highlightBreathing=true; s.highlightIdleHideMs=3000; s.spotlightDim=12; s.spotlightRadius=41;
    s.runAsAdministrator=true; s.captureVisible=false; s.uiAnimatedAccent=false;
    s.middleClickEnabled=true; s.middleClickColor=RGB(1,2,3);
    s.keyboardSoundsEnabled=true; s.keyboardSoundPath=L"C:\\sounds\\keyboard\\mechanical"; s.keyboardVolume=11;
    s.hotkeyKeyboardSounds=L"Ctrl+Alt+6";
    s.trailEnabled=true; s.trailStyle=4; s.trailRadius=16; s.trailOpacity=100;
    s.trailDurationMs=700; s.trailColor=RGB(81,19,233);
    s.clickEffectsEnabled=false; s.leftClickEnabled=false; s.rightClickEnabled=false;
    s.clickStyle=clickshade::kClickBurst; s.clickRadius=120; s.clickDurationMs=1500; s.clickOpacity=5;
    s.clickThickness=12; s.clickEasing=2; s.visualRefreshRate=30;
    s.leftClickColor=RGB(21,32,43); s.rightClickColor=RGB(43,32,21);
    s.clickSoundsEnabled=true; s.soundVolume=0;
    s.leftSoundPath=L"C:\\sounds\\left.wav"; s.rightSoundPath=L"C:\\sounds\\right.m4a";
    s.middleSoundPath=L"C:\\sounds\\middle.aac";
    s.uiDarkMode=false; s.uiBackground=RGB(240,230,220); s.uiAccent=RGB(10,20,30);
    s.uiLightBackground=s.uiBackground; s.uiLightAccent=s.uiAccent;
    s.uiDarkBackground=RGB(1,2,3); s.uiDarkAccent=RGB(99,88,77); s.uiScalePercent=200;
    s.hotkeyToggleAll=L"Ctrl+Alt+1"; s.hotkeyHighlight=L"Ctrl+Alt+2";
    s.hotkeyClicks=L"Ctrl+Alt+3"; s.hotkeySounds=L"Ctrl+Alt+4";
    s.hotkeySettings=L"Ctrl+Alt+5"; s.hotkeyRestore=L"Ctrl+Alt+F24";
    s.magnifierEnabled=true; s.magnifierZoom=800; s.magnifierSize=80; s.magnifierShape=1; s.magnifierPosition=1;
    s.magnifierBorderColor=RGB(7,8,9); s.magnifierFrame=false;
    s.keyDisplayEnabled=true; s.keyDisplayMode=1; s.keyDisplayMouse=true; s.keyDisplayPosition=3; s.keyDisplaySize=250;
    s.keyDisplayDurationMs=6000; s.keyDisplayOpacity=20; s.keyDisplayColor=RGB(10,11,12);
    s.hotkeyMagnifier=L"Ctrl+Alt+7"; s.hotkeyKeyDisplay=L"Ctrl+Alt+8";
    return s;
}
void Run() {
    using namespace clickshade;
    Fixture fixture; const auto profile=fixture.Path(L"export.ini"); std::wstring error;
    const bool startup=IsStartupEnabled();
    const Settings original=Example();
    Require(SaveSettingsProfile(profile,original,error),"a complete nondefault profile exports successfully");
    const auto bytes=Read(profile); const auto text=Wide(bytes);
    Check(text.find(L"FormatVersion=2\r\nPlatform=Windows\r\n")!=std::wstring::npos,
          "export carries explicit Windows platform and version metadata");
    Settings imported;
    Require(LoadSettingsProfile(profile,imported,error),"the exported profile passes strict import validation");
    const auto second=fixture.Path(L"roundtrip.ini");
    Require(SaveSettingsProfile(second,imported,error),"loaded settings can be exported again");
    Check(Read(second)==bytes,"every serialized preference round-trips without loss");
    Check(imported.middleSoundPath==original.middleSoundPath&&imported.shakeStyle==2&&
          imported.trailStyle==4&&imported.visualRefreshRate==30,
          "middle audio, locator, professional trail and 30 Hz options survive import");
    Check(imported.highlightStyle==clickshade::kHighlightCrosshair&&imported.clickStyle==clickshade::kClickBurst&&
          imported.middleClickEnabled&&imported.middleClickColor==RGB(1,2,3)&&imported.keyboardSoundsEnabled&&
          imported.keyboardSoundPath==original.keyboardSoundPath&&imported.keyboardVolume==11&&!imported.captureVisible&&
          imported.runAsAdministrator&&!imported.uiAnimatedAccent&&imported.highlightBreathing&&imported.highlightIdleHideMs==3000&&
          imported.spotlightDim==12&&imported.spotlightRadius==41&&imported.hotkeyKeyboardSounds==L"Ctrl+Alt+6",
          "style, middle click, keyboard sound, capture, admin, spotlight and idle-hide options round-trip");
    Check(imported.magnifierEnabled&&imported.magnifierZoom==800&&imported.magnifierSize==80&&imported.magnifierShape==1&&
          imported.magnifierPosition==1&&imported.magnifierBorderColor==RGB(7,8,9)&&!imported.magnifierFrame&&imported.keyDisplayEnabled&&imported.keyDisplayMode==1&&
          imported.keyDisplayMouse&&imported.keyDisplayPosition==3&&imported.keyDisplaySize==250&&imported.keyDisplayDurationMs==6000&&
          imported.keyDisplayOpacity==20&&imported.keyDisplayColor==RGB(10,11,12)&&imported.hotkeyMagnifier==L"Ctrl+Alt+7"&&
          imported.hotkeyKeyDisplay==L"Ctrl+Alt+8",
          "magnifier and key display options and their shortcuts round-trip");
    Check(imported.cursorOpacity==55&&imported.cursorShadow&&imported.cursorColors&&imported.cursorFillColor==RGB(13,14,15)&&
          imported.cursorOutlineColor==RGB(16,17,18), "cursor opacity, shadow and colors round-trip");
    {
        // A format 2 profile written before the presenter tools existed has
        // none of their keys; it must still import with their defaults.
        std::wstring older=text;
        for(const wchar_t* key:{L"MagnifierEnabled",L"MagnifierZoom",L"MagnifierSize",L"MagnifierShape",L"MagnifierPosition",
                L"MagnifierBorderColor",L"MagnifierFrame",L"KeyDisplayEnabled",L"KeyDisplayMode",L"KeyDisplayMouse",L"KeyDisplayPosition",L"KeyDisplaySize",
                L"KeyDisplayDurationMs",L"KeyDisplayOpacity",L"KeyDisplayColor",L"HotkeyMagnifier",L"HotkeyKeyDisplay"}) {
            const std::wstring prefix=std::wstring(key)+L"=";
            const auto at=older.find(L"\r\n"+prefix);
            if(at==std::wstring::npos) throw std::runtime_error("missing presenter fixture key");
            older.erase(at,older.find(L"\r\n",at+2)-at);
        }
        const auto olderPath=fixture.Path(L"older-format2.ini");
        Write(olderPath,older.data(),older.size()*sizeof(wchar_t));
        Settings fromOlder;
        Check(LoadSettingsProfile(olderPath,fromOlder,error)&&!fromOlder.magnifierEnabled&&!fromOlder.keyDisplayEnabled&&
              fromOlder.magnifierZoom==Settings{}.magnifierZoom&&fromOlder.keyDisplayDurationMs==Settings{}.keyDisplayDurationMs&&
              fromOlder.hotkeyMagnifier.empty()&&fromOlder.shakeStyle==2,
              "a format 2 profile without the presenter keys imports with their defaults");
    }
    {
        // A preview-build profile has no SettingsVersion or style keys and
        // carries the old shape switches; import must upgrade it exactly like
        // a preview-build settings file.
        const auto remove=[](std::wstring content,const wchar_t* key) {
            const std::wstring prefix=std::wstring(key)+L"=";
            const auto at=content.find(L"\r\n"+prefix);
            if(at==std::wstring::npos) throw std::runtime_error("missing legacy fixture key");
            const auto end=content.find(L"\r\n",at+2);
            return content.erase(at,end-at);
        };
        std::wstring legacy=Replace(text,L"FormatVersion=2",L"FormatVersion=1");
        for(const wchar_t* key:{L"SettingsVersion",L"RunAsAdministrator",L"CaptureVisible",L"HighlightBreathing",L"HighlightIdleHideMs",
                L"SpotlightDim",L"SpotlightRadius",L"MiddleClickEnabled",L"MiddleClickColor",L"KeyboardSoundsEnabled",L"KeyboardSoundPath",
                L"KeyboardVolume",L"UiAnimatedAccent",L"HotkeyKeyboardSounds",L"CursorOpacity",L"CursorShadow",
                L"CursorColors",L"CursorFillColor",L"CursorOutlineColor"}) legacy=remove(legacy,key);
        legacy=Replace(legacy,L"HighlightStyle=3",L"HighlightFilled=0");
        legacy=Replace(legacy,L"CursorScale=400",L"CursorSize=128");
        legacy=Replace(legacy,L"ClickStyle=3",L"ClickFilled=1");
        legacy=Replace(legacy,L"LeftClickColor=2826261",L"LeftClickColor=16754505");
        const auto legacyPath=fixture.Path(L"legacy.ini");
        Write(legacyPath,legacy.data(),legacy.size()*sizeof(wchar_t));
        Settings upgraded;
        Check(LoadSettingsProfile(legacyPath,upgraded,error),"a complete format 1 preview-build profile still imports");
        Check(upgraded.highlightStyle==clickshade::kHighlightRing&&upgraded.clickStyle==clickshade::kClickPulse,
              "legacy filled switches become the ring and pulse styles");
        Check(upgraded.cursorScale==400, "a legacy 128-pixel cursor size becomes a 400 percent cursor scale");
        Check(upgraded.cursorOpacity==Settings{}.cursorOpacity&&!upgraded.cursorShadow&&!upgraded.cursorColors,
              "a profile without the cursor look options keeps the plain look");
        Check(upgraded.leftClickColor==Settings{}.leftClickColor&&upgraded.rightClickColor==RGB(43,32,21),
              "the untouched preview-build left color upgrades to the neon default while a custom right color is kept");
        Check(upgraded.captureVisible&&!upgraded.runAsAdministrator&&upgraded.keyboardVolume==Settings{}.keyboardVolume&&
              !upgraded.middleClickEnabled&&upgraded.hotkeyKeyboardSounds.empty(),
              "options missing from a format 1 profile take their defaults");
    }
    Check(imported.releasedCursorPath==original.releasedCursorPath,
          "Unicode paths containing spaces and equals signs are preserved");
    Check(IsStartupEnabled()==startup&&imported.startWithWindows,
          "loading a recorded startup preference never alters actual Windows startup");
    const auto bad=fixture.Path(L"invalid.ini");
    const auto reject=[&](const std::wstring& data,const char* label) {
        Write(bad,data.data(),data.size()*sizeof(wchar_t));
        Settings sentinel; sentinel.middleSoundPath=L"unchanged"; sentinel.cursorScale=177;
        error.clear();
        Check(!LoadSettingsProfile(bad,sentinel,error)&&!error.empty()&&
              sentinel.middleSoundPath==L"unchanged"&&sentinel.cursorScale==177,label);
    };
    reject(Replace(text,L"Platform=Windows",L"Platform=Linux"),"a Linux profile is rejected before any output mutation");
    reject(Replace(text,L"FormatVersion=2",L"FormatVersion=3"),"unsupported future format versions are rejected");
    reject(Replace(text,L"ClickStyle=3",L"ClickStyle=6"),"unknown click styles are rejected");
    reject(Replace(text,L"SpotlightDim=12",L"SpotlightDim=9"),"out-of-range spotlight dimming is rejected");
    reject(Replace(text,L"Enabled=0\r\n",L""),"partial profiles with a missing required field are rejected");
    reject(text+L"Enabled=1\r\n","duplicate fields cannot override validated values");
    reject(text+L"UnknownAction=run.exe\r\n","unrecognized settings are rejected rather than interpreted");
    reject(text+L"[Extra]\r\nCommand=anything\r\n","unrecognized INI sections are rejected");
    reject(Replace(text,L"Enabled=0\r\n",L"Enabled=2\r\n"),"booleans must be exactly 0 or 1");
    reject(Replace(text,L"Buttons=31",L"Buttons=32"),"unknown mouse button bits are rejected");
    reject(Replace(text,L"VisualRefreshRate=30",L"VisualRefreshRate=29"),"manual frame caps below 30 are rejected");
    reject(Replace(text,L"TrailStyle=4",L"TrailStyle=5"),"unsupported trail styles are rejected");
    reject(Replace(text,L"MagnifierZoom=800",L"MagnifierZoom=801"),"out-of-range magnifier zoom is rejected");
    reject(Replace(text,L"KeyDisplayPosition=3",L"KeyDisplayPosition=4"),"unknown key display positions are rejected");
    reject(Replace(text,L"HotkeyKeyDisplay=Ctrl+Alt+8",L"HotkeyKeyDisplay=Ctrl+Alt+7"),"a presenter shortcut cannot duplicate another action's chord");
    reject(Replace(text,L"CursorScale=400",L"CursorScale=4294967296"),"overflowed numeric fields are rejected");
    reject(Replace(text,L"CursorScale=400",L"CursorScale=601"),"a cursor scale above 600 percent is rejected");
    reject(Replace(text,L"CursorOpacity=55",L"CursorOpacity=29"),"a cursor opacity below 30 percent is rejected");
    reject(Replace(text,L"HotkeyHighlight=Ctrl+Alt+2",L"HotkeyHighlight=Ctrl+Alt+1"),"duplicate shortcut assignments are rejected together");
    reject(Replace(text,L"HotkeyHighlight=Ctrl+Alt+2",L"HotkeyHighlight=A"),"ordinary typing cannot become a global shortcut by import");
    reject(Replace(text,L"UiBackground=14477040",L"UiBackground=1"),"inconsistent active and saved palettes are rejected");
    auto nul=text; nul.insert(nul.size()/2,1,L'\0'); reject(nul,"embedded NUL text is rejected");
    auto broken=text; broken.push_back(static_cast<wchar_t>(0xd800)); reject(broken,"unpaired UTF-16 surrogate data is rejected");
    std::vector<unsigned char> oversized(1024*1024+1,'x'); Write(bad,oversized.data(),oversized.size());
    Check(!LoadSettingsProfile(bad,imported,error),"oversized input is rejected before parsing");
    Write(bad,nullptr,0);
    Check(!LoadSettingsProfile(bad,imported,error),"empty profile files are rejected");
    const unsigned char invalidUtf8[]{0xc0,0xaf}; Write(bad,invalidUtf8,sizeof(invalidUtf8));
    Check(!LoadSettingsProfile(bad,imported,error),"invalid UTF-8 encodings are rejected");
    Check(!LoadSettingsProfile(fixture.directory,imported,error),"directories cannot be imported as profiles");
    Check(!LoadSettingsProfile(L"\\\\.\\NUL",imported,error),"device paths are rejected before opening");
    Check(!LoadSettingsProfile(L"//./pipe/ClickShade-profile-test",imported,error)&&
          error.find(L"regular profile file path")!=std::wstring::npos,
          "forward-slash device namespaces are rejected after canonicalization without opening");
    Check(!LoadSettingsProfile(L"\\/./pipe\\ClickShade-profile-test",imported,error)&&
          error.find(L"regular profile file path")!=std::wstring::npos,
          "mixed-separator device namespaces cannot bypass regular-file validation");
    {
        // Windows 11 removed the legacy device-name reservation for names such
        // as NUL.ini; older builds still refuse them. Both outcomes are safe as
        // long as no device receives the data.
        const auto reserved=fixture.Path(L"NUL.ini");
        const bool saved=SaveSettingsProfile(reserved,original,error);
        const DWORD attributes=GetFileAttributesW(reserved.c_str());
        Check(!saved||(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_DIRECTORY)),
              "a legacy DOS device name is either rejected or written as an ordinary disk file");
    }
    Check(!SaveSettingsProfile(fixture.directory,original,error),"export cannot replace a directory");
    const auto backup=fixture.Path(L"settings-backup.ini");
    Require(SaveSettingsProfile(backup,original,error),"a complete manual backup is written");
    Settings changed=original; changed.soundVolume=61;
    Require(SaveSettingsProfile(backup,changed,error)&&LoadSettingsProfile(backup,imported,error),
            "a stable backup can be replaced and read back");
    Check(imported.soundVolume==61,"the recoverable backup contains the latest requested preferences");
    HANDLE locked=CreateFileW(profile.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Require(locked!=INVALID_HANDLE_VALUE,"an existing profile is locked to simulate a failed atomic replacement");
    const bool replaced=SaveSettingsProfile(profile,changed,error); CloseHandle(locked);
    Check(!replaced&&!error.empty()&&Read(profile)==bytes,"failed replacement preserves the entire old profile");
    WIN32_FIND_DATAW temporary{};
    const HANDLE staging=FindFirstFileW((profile+L".tmp.*").c_str(),&temporary);
    Check(staging==INVALID_HANDLE_VALUE,"failed export removes its staging file without history debris");
    if(staging!=INVALID_HANDLE_VALUE) FindClose(staging);
    const int utf8Size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::vector<char> utf8(static_cast<std::size_t>(utf8Size));
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),utf8.data(),utf8Size,nullptr,nullptr);
    Write(bad,utf8.data(),utf8.size());
    Check(LoadSettingsProfile(bad,imported,error)&&imported.releasedCursorPath==original.releasedCursorPath,
          "valid UTF-8 profiles preserve the same Unicode preferences");
    Settings longPaths=original;
    longPaths.leftSoundPath=std::wstring(32000,L'a'); longPaths.rightSoundPath=std::wstring(32000,L'b');
    longPaths.middleSoundPath=std::wstring(32000,L'c'); longPaths.releasedCursorPath=std::wstring(32000,L'd');
    longPaths.pressedCursorPath=std::wstring(32000,L'e');
    Check(SaveSettingsProfile(bad,longPaths,error)&&LoadSettingsProfile(bad,imported,error)&&
          imported.middleSoundPath==longPaths.middleSoundPath&&imported.pressedCursorPath==longPaths.pressedCursorPath,
          "all bounded long path fields fit the profile size budget without truncation");
    changed.middleSoundPath=L"bad\nEnabled=1";
    Check(!SaveSettingsProfile(second,changed,error),"export rejects multiline text that could inject a second setting");
    Check(IsStartupEnabled()==startup,"the entire profile test leaves startup registration untouched");
}
}

int wmain() {
    try { Run(); }
    catch(const std::exception& error) { std::printf("FAIL fixture: %s\n",error.what()); ++failed; }
    std::printf("RESULT %u passed, %u failed; profile storage only, no live runtime or startup changes.\n",passed,failed);
    return failed?1:0;
}
