#include "ui_common.h"
#include <cwctype>

namespace clickshade::ui {
namespace {
bool AudioExtension(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* extension = name.c_str() + dot;
    for (const wchar_t* candidate : {L".wav", L".mp3", L".wma", L".flac", L".m4a", L".aac"})
        if (_wcsicmp(extension, candidate) == 0) return true;
    return false;
}

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}

struct CloseFind {
    HANDLE handle;
    explicit CloseFind(HANDLE value) : handle(value) {}
    ~CloseFind() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
    CloseFind(const CloseFind&) = delete;
    CloseFind& operator=(const CloseFind&) = delete;
};

// A file named "<style>-left.wav" belongs to the left dropdown only; files
// without a button word are offered to every button.
unsigned ButtonsFor(const std::wstring& fileName) {
    const auto lower = Lower(fileName);
    const auto stem = lower.substr(0, lower.find_last_of(L'.'));
    const auto hasToken = [&](const wchar_t* token) {
        const auto at = stem.rfind(token);
        if (at == std::wstring::npos) return false;
        const bool boundaryBefore = at == 0 || stem[at - 1] == L'-' || stem[at - 1] == L'_' || stem[at - 1] == L' ';
        const bool boundaryAfter = at + wcslen(token) == stem.size();
        return boundaryBefore && boundaryAfter;
    };
    unsigned buttons = 0;
    if (hasToken(L"left")) buttons |= 1;
    if (hasToken(L"right")) buttons |= 2;
    if (hasToken(L"middle")) buttons |= 4;
    return buttons ? buttons : 7;
}
} // namespace

std::wstring PathLeaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool SameFilePath(std::wstring first, std::wstring second) {
    if (first.empty() || second.empty()) return first.empty() && second.empty();
    std::replace(first.begin(), first.end(), L'/', L'\\');
    std::replace(second.begin(), second.end(), L'/', L'\\');
    while (!first.empty() && first.back() == L'\\') first.pop_back();
    while (!second.empty() && second.back() == L'\\') second.pop_back();
    return CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()), second.c_str(),
                                static_cast<int>(second.size()), TRUE) == CSTR_EQUAL;
}

std::vector<SoundChoice> FindSoundChoices() {
    std::vector<SoundChoice> choices;
    const auto root = SoundDirectory();
    if (root.empty()) return choices;
    struct Folder { std::wstring path, label; unsigned depth; };
    std::vector<Folder> pending{{root, L"", 0}};
    unsigned inspected = 0;
    while (!pending.empty() && choices.size() < 256 && inspected < 4096) {
        const Folder folder = std::move(pending.back()); pending.pop_back();
        WIN32_FIND_DATAW item{};
        const HANDLE search = FindFirstFileW((folder.path + L"\\*").c_str(), &item);
        if (search == INVALID_HANDLE_VALUE) continue;
        CloseFind close{search};
        do {
            ++inspected;
            if (item.cFileName[0] == L'.' && (!item.cFileName[1] ||
                (item.cFileName[1] == L'.' && !item.cFileName[2]))) continue;
            if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const std::wstring name = item.cFileName;
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Keyboard sets have their own dropdown.
                if (folder.depth == 0 && _wcsicmp(name.c_str(), L"keyboard") == 0) continue;
                if (folder.depth < 2 && pending.size() < 256)
                    pending.push_back({folder.path + L"\\" + name, folder.label.empty() ? name : folder.label + L" / " + name, folder.depth + 1});
                continue;
            }
            if (!AudioExtension(name)) continue;
            std::wstring label = folder.label.empty() ? name : folder.label + L" / " + name;
            if (!label.empty()) label[0] = static_cast<wchar_t>(std::towupper(label[0]));
            choices.push_back({folder.path + L"\\" + name, std::move(label), ButtonsFor(name)});
        } while (choices.size() < 256 && inspected < 4096 && FindNextFileW(search, &item));
    }
    std::sort(choices.begin(), choices.end(), [](const SoundChoice& a, const SoundChoice& b) {
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return choices;
}

std::vector<KeyboardSetChoice> FindKeyboardSets() {
    std::vector<KeyboardSetChoice> sets;
    const auto root = KeyboardSoundDirectory();
    if (root.empty()) return sets;
    WIN32_FIND_DATAW item{};
    const HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) return sets;
    CloseFind close{search};
    unsigned inspected = 0;
    do {
        if (item.cFileName[0] == L'.') continue;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring name = item.cFileName;
        const std::wstring full = root + L"\\" + name;
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // A set needs at least one key*.wav (or other supported) file.
            WIN32_FIND_DATAW key{};
            const HANDLE keys = FindFirstFileW((full + L"\\key*").c_str(), &key);
            if (keys == INVALID_HANDLE_VALUE) continue;
            FindClose(keys);
            std::wstring label = name;
            std::replace(label.begin(), label.end(), L'-', L' ');
            if (!label.empty()) label[0] = static_cast<wchar_t>(std::towupper(label[0]));
            sets.push_back({full, label});
        } else if (AudioExtension(name)) {
            sets.push_back({full, name + L" (single sound)"});
        }
    } while (++inspected < 512 && sets.size() < 128 && FindNextFileW(search, &item));
    std::sort(sets.begin(), sets.end(), [](const KeyboardSetChoice& a, const KeyboardSetChoice& b) {
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return sets;
}

} // namespace clickshade::ui
