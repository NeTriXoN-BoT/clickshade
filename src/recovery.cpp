#include "recovery.h"
#include "settings.h"
#include "cursor_engine.h"
#include <vector>

namespace clickshade {
RecoveryGuard::~RecoveryGuard() { StopCleanly(); }

bool RecoveryGuard::Start(std::wstring& error) {
    if (process_) return true;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    cleanExit_ = CreateEventW(&sa, TRUE, FALSE, nullptr);
    HANDLE parent{};
    if (!cleanExit_ || !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
        GetCurrentProcess(), &parent, SYNCHRONIZE, TRUE, 0)) {
        if (cleanExit_) CloseHandle(cleanExit_);
        cleanExit_ = nullptr;
        error = L"Could not prepare the cursor recovery guard.";
        return false;
    }
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attributeBytes)) {
        CloseHandle(parent); CloseHandle(cleanExit_); cleanExit_ = nullptr;
        error = L"Could not initialize recovery process attributes.";
        return false;
    }
    HANDLE handles[] = {parent, cleanExit_};
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof(handles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        CloseHandle(parent); CloseHandle(cleanExit_); cleanExit_ = nullptr;
        error = L"Could not restrict recovery process handle inheritance.";
        return false;
    }
    const auto exe = ExecutablePath();
    auto command = L"\"" + exe + L"\" --guard " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(cleanExit_));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD code = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(parent);
    SetHandleInformation(cleanExit_, HANDLE_FLAG_INHERIT, 0);
    if (!ok) {
        CloseHandle(cleanExit_); cleanExit_ = nullptr;
        error = L"Could not start the recovery guard (Windows error " + std::to_wstring(code) + L").";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
}

void RecoveryGuard::StopCleanly() {
    // The caller stops the cursor runtime and restores originals first.
    if (cleanExit_) SetEvent(cleanExit_);
    if (process_) { WaitForSingleObject(process_, 3000); CloseHandle(process_); process_ = nullptr; }
    if (cleanExit_) { CloseHandle(cleanExit_); cleanExit_ = nullptr; }
}

int RunRecoveryGuard(HANDLE parent, HANDLE cleanExit) {
    if (!parent || !cleanExit || parent == INVALID_HANDLE_VALUE || cleanExit == INVALID_HANDLE_VALUE)
        return 2;
    HANDLE handles[] = {cleanExit, parent};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    bool restored = true;
    if (result == WAIT_OBJECT_0 + 1) restored = CursorEngine::RestoreSystemScheme();
    CloseHandle(parent);
    CloseHandle(cleanExit);
    return result == WAIT_FAILED || !restored ? 1 : 0;
}
} // namespace clickshade
