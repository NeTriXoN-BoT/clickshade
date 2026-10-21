#include "crash_guard.h"
#include "settings.h"
#include "version.h"

#include <dbghelp.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <mutex>
#include <vector>

namespace clickshade {
namespace {
constexpr unsigned kMaximumReports = 5;
constexpr unsigned kMaximumRestartGeneration = 3;
constexpr wchar_t kNoticeFile[] = L"last-crash.txt";

// Everything the fault path needs is prepared up front so a crash inside the
// allocator or a corrupted heap still produces a report and a restart.
struct PreparedState {
    wchar_t reportDirectory[MAX_PATH * 2]{};
    wchar_t executable[MAX_PATH * 2]{};
    wchar_t restartArguments[256]{};
    bool restart = false;
    unsigned generation = 0;
    std::atomic<bool> handling{false};
};
PreparedState state;
std::mutex noticeMutex;
std::wstring internalError;
std::wstring loadedNoticeSummary, loadedNoticePath;
bool noticeLoaded = false;

struct TextSink {
    HANDLE file = INVALID_HANDLE_VALUE;
    void Line(const wchar_t* text) {
        if (file == INVALID_HANDLE_VALUE || !text) return;
        char narrow[2048];
        const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow) - 3, nullptr, nullptr);
        if (count <= 0) return;
        narrow[count - 1] = '\r'; narrow[count] = '\n';
        DWORD written = 0;
        WriteFile(file, narrow, static_cast<DWORD>(count + 1), &written, nullptr);
    }
    void Format(const wchar_t* format, ...) {
        wchar_t line[1024];
        va_list arguments;
        va_start(arguments, format);
        _vsnwprintf(line, 1023, format, arguments);
        va_end(arguments);
        line[1023] = 0;
        Line(line);
    }
};

const wchar_t* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return L"access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return L"misaligned data access";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return L"floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return L"invalid floating-point operation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return L"illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return L"in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return L"privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return L"stack overflow";
    case 0xE06D7363: return L"unhandled C++ exception";
    case 0xC0000409: return L"stack buffer overrun / fast fail";
    default: return L"exception";
    }
}

void ModuleOf(const void* address, wchar_t* name, std::size_t count, std::uintptr_t& offset) {
    name[0] = 0; offset = 0;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &module) || !module) {
        wcsncpy(name, L"?", count - 1);
        return;
    }
    wchar_t path[MAX_PATH * 2]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH * 2);
    const wchar_t* base = path;
    for (DWORD i = 0; i < length; ++i) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    wcsncpy(name, length ? base : L"?", count - 1);
    name[count - 1] = 0;
    offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
}

struct DbgHelp {
    HMODULE library = nullptr;
    using StackWalkFn = BOOL (WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                       PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
    using FunctionTableFn = PVOID (WINAPI*)(HANDLE, DWORD64);
    using ModuleBaseFn = DWORD64 (WINAPI*)(HANDLE, DWORD64);
    using MiniDumpFn = BOOL (WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    using SymInitializeFn = BOOL (WINAPI*)(HANDLE, PCWSTR, BOOL);
    StackWalkFn stackWalk = nullptr;
    FunctionTableFn functionTable = nullptr;
    ModuleBaseFn moduleBase = nullptr;
    MiniDumpFn miniDump = nullptr;
    SymInitializeFn symInitialize = nullptr;
    template <class Function> void Load(Function& target, const char* name) {
        const FARPROC address = GetProcAddress(library, name);
        static_assert(sizeof(address) == sizeof(target), "Windows function pointer size");
        std::memcpy(&target, &address, sizeof(target));
    }
    DbgHelp() {
        library = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return;
        Load(stackWalk, "StackWalk64");
        Load(functionTable, "SymFunctionTableAccess64");
        Load(moduleBase, "SymGetModuleBase64");
        Load(miniDump, "MiniDumpWriteDump");
        Load(symInitialize, "SymInitializeW");
    }
};

void WriteStack(TextSink& sink, const CONTEXT* faultContext, DbgHelp& dbg) {
    sink.Line(L"");
    sink.Line(L"Stack (module+offset, innermost first):");
    if (faultContext && dbg.stackWalk && dbg.functionTable && dbg.moduleBase) {
        if (dbg.symInitialize) dbg.symInitialize(GetCurrentProcess(), nullptr, TRUE);
        CONTEXT context = *faultContext;
        STACKFRAME64 frame{};
#if defined(_M_X64) || defined(__x86_64__)
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset = context.Rip; frame.AddrFrame.Offset = context.Rbp; frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
        const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
        frame.AddrPC.Offset = context.Pc; frame.AddrFrame.Offset = context.Fp; frame.AddrStack.Offset = context.Sp;
#else
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset = context.Eip; frame.AddrFrame.Offset = context.Ebp; frame.AddrStack.Offset = context.Esp;
#endif
        frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
        for (unsigned depth = 0; depth < 48; ++depth) {
            if (!dbg.stackWalk(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                               dbg.functionTable, dbg.moduleBase, nullptr) || !frame.AddrPC.Offset) break;
            wchar_t module[128]; std::uintptr_t offset = 0;
            ModuleOf(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(frame.AddrPC.Offset)), module, 128, offset);
            sink.Format(L"  %2u  %s+0x%llX", depth, module, static_cast<unsigned long long>(offset));
        }
        return;
    }
    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT depth = 0; depth < count; ++depth) {
        wchar_t module[128]; std::uintptr_t offset = 0;
        ModuleOf(frames[depth], module, 128, offset);
        sink.Format(L"  %2u  %s+0x%llX", depth, module, static_cast<unsigned long long>(offset));
    }
}

void PruneOldReports() {
    struct Entry { FILETIME written; wchar_t name[MAX_PATH]; };
    std::vector<Entry> entries;
    wchar_t pattern[MAX_PATH * 2];
    _snwprintf(pattern, MAX_PATH * 2 - 1, L"%s\\clickshade-crash-*", state.reportDirectory);
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW(pattern, &item);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        Entry entry{item.ftLastWriteTime, {}};
        wcsncpy(entry.name, item.cFileName, MAX_PATH - 1);
        entries.push_back(entry);
    } while (FindNextFileW(search, &item) && entries.size() < 512);
    FindClose(search);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return CompareFileTime(&a.written, &b.written) > 0;
    });
    // Each crash writes a .txt and possibly a .dmp with the same stem.
    for (std::size_t i = kMaximumReports * 2; i < entries.size(); ++i) {
        wchar_t path[MAX_PATH * 2];
        _snwprintf(path, MAX_PATH * 2 - 1, L"%s\\%s", state.reportDirectory, entries[i].name);
        DeleteFileW(path);
    }
}

void StartReplacement(const wchar_t* reportPath) {
    if (!state.restart || state.generation >= kMaximumRestartGeneration || !state.executable[0]) {
        wchar_t message[1024];
        _snwprintf(message, 1023, L"ClickShade stopped after an internal error and will not restart automatically%s.\n\n"
            L"A report was saved to:\n%s\n\nStart ClickShade again from its folder. Your cursors have been restored.",
            state.generation >= kMaximumRestartGeneration ? L" because it already restarted several times in a row" : L"",
            reportPath && reportPath[0] ? reportPath : L"(the report folder could not be created)");
        message[1023] = 0;
        MessageBoxW(nullptr, message, L"ClickShade", MB_ICONERROR | MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }
    wchar_t command[MAX_PATH * 2 + 512];
    _snwprintf(command, MAX_PATH * 2 + 511, L"\"%s\" --after-crash %u --replace %lu %s", state.executable,
               state.generation + 1, GetCurrentProcessId(), state.restartArguments);
    command[MAX_PATH * 2 + 511] = 0;
    STARTUPINFOW info{}; info.cb = sizeof(info);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(state.executable, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &info, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void HandleFatal(const wchar_t* kind, const wchar_t* detail, EXCEPTION_POINTERS* pointers) {
    if (state.handling.exchange(true)) {
        // A second fault while reporting: give the first handler a moment,
        // then stop hard rather than recursing.
        Sleep(5000);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    // The user's cursors come first; nothing below may delay this call.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);

    SYSTEMTIME now{}; GetSystemTime(&now);
    wchar_t stem[MAX_PATH * 2]{}, reportPath[MAX_PATH * 2]{}, dumpPath[MAX_PATH * 2]{};
    if (state.reportDirectory[0]) {
        _snwprintf(stem, MAX_PATH * 2 - 1, L"%s\\clickshade-crash-%04u%02u%02u-%02u%02u%02u", state.reportDirectory,
                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        _snwprintf(reportPath, MAX_PATH * 2 - 1, L"%s.txt", stem);
        _snwprintf(dumpPath, MAX_PATH * 2 - 1, L"%s.dmp", stem);
    }
    TextSink sink;
    if (reportPath[0]) sink.file = CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    wchar_t summary[512]{};
    DbgHelp dbg;
    sink.Format(L"ClickShade %s crash report", kClickShadeVersionW);
    sink.Format(L"Time (UTC): %04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    sink.Format(L"Kind: %s", kind);
    if (detail && detail[0]) sink.Format(L"Detail: %s", detail);
    sink.Format(L"Thread: %lu  Process: %lu  Restart generation: %u", GetCurrentThreadId(), GetCurrentProcessId(), state.generation);
    if (pointers && pointers->ExceptionRecord) {
        const auto& record = *pointers->ExceptionRecord;
        wchar_t module[128]; std::uintptr_t offset = 0;
        ModuleOf(record.ExceptionAddress, module, 128, offset);
        sink.Format(L"Exception: 0x%08lX (%s) at %s+0x%llX", record.ExceptionCode, ExceptionName(record.ExceptionCode),
                    module, static_cast<unsigned long long>(offset));
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            sink.Format(L"Access: %s address 0x%llX", record.ExceptionInformation[0] == 1 ? L"write to" :
                        record.ExceptionInformation[0] == 8 ? L"execute at" : L"read from",
                        static_cast<unsigned long long>(record.ExceptionInformation[1]));
        _snwprintf(summary, 511, L"%s in %s+0x%llX", ExceptionName(record.ExceptionCode), module, static_cast<unsigned long long>(offset));
    } else {
        _snwprintf(summary, 511, L"%s%s%s", kind, detail && detail[0] ? L": " : L"", detail ? detail : L"");
    }
    summary[511] = 0;
    sink.Line(L"This report contains no settings, selected file paths, screen contents or input.");
    WriteStack(sink, pointers ? pointers->ContextRecord : nullptr, dbg);
    if (sink.file != INVALID_HANDLE_VALUE) CloseHandle(sink.file);
    if (dumpPath[0] && dbg.miniDump) {
        HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
            // MiniDumpNormal: thread stacks and module list only.
            dbg.miniDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal,
                         pointers ? &exception : nullptr, nullptr, nullptr);
            CloseHandle(dump);
        }
    }
    if (state.reportDirectory[0]) {
        wchar_t noticePath[MAX_PATH * 2];
        _snwprintf(noticePath, MAX_PATH * 2 - 1, L"%s\\%s", state.reportDirectory, kNoticeFile);
        TextSink notice;
        notice.file = CreateFileW(noticePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        notice.Line(reportPath);
        notice.Line(summary);
        if (notice.file != INVALID_HANDLE_VALUE) CloseHandle(notice.file);
        PruneOldReports();
    }
    StartReplacement(reportPath);
    TerminateProcess(GetCurrentProcess(), 3);
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* pointers) {
    HandleFatal(L"Unhandled exception", nullptr, pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnTerminate() {
    wchar_t detail[512]{};
    try {
        if (auto current = std::current_exception()) std::rethrow_exception(current);
    } catch (const std::exception& error) {
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, detail, 511);
    } catch (...) {
        wcsncpy(detail, L"non-standard exception object", 511);
    }
    HandleFatal(L"Uncaught C++ exception", detail, nullptr);
}

#if defined(_MSC_VER)
void __cdecl OnPureCall() { HandleFatal(L"Pure virtual function call", nullptr, nullptr); }
void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    HandleFatal(L"Invalid CRT parameter", nullptr, nullptr);
}
#endif
} // namespace

void InstallCrashGuard(const CrashGuardOptions& options) {
    state.restart = options.restartAfterCrash;
    state.generation = options.restartGeneration;
    const auto directory = CrashReportDirectory();
    wcsncpy(state.reportDirectory, directory.c_str(), MAX_PATH * 2 - 1);
    const auto executable = ExecutablePath();
    wcsncpy(state.executable, executable.c_str(), MAX_PATH * 2 - 1);
    // Keep the tray-only start mode across a restart so a crash during an
    // unattended session does not pop a settings window onto the screen.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    state.restartArguments[0] = 0;
    for (int n = 1; argv && n < argc; ++n)
        if (wcscmp(argv[n], L"--tray") == 0) wcsncpy(state.restartArguments, L"--tray", 255);
    if (argv) LocalFree(argv);
    SetUnhandledExceptionFilter(TopLevelFilter);
    std::set_terminate(OnTerminate);
#if defined(_MSC_VER)
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
#endif
    // Fail-fast dialogs would hide the fault behind a Windows box the user
    // cannot act on; the handler above restores cursors and explains instead.
    SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

void ResetRestartGeneration() noexcept { state.generation = 0; }

CrashNotice TakeCrashNotice() {
    std::lock_guard<std::mutex> lock(noticeMutex);
    CrashNotice notice;
    if (!noticeLoaded) {
        noticeLoaded = true;
        if (state.generation > 0 && state.reportDirectory[0]) {
            wchar_t noticePath[MAX_PATH * 2];
            _snwprintf(noticePath, MAX_PATH * 2 - 1, L"%s\\%s", state.reportDirectory, kNoticeFile);
            HANDLE file = CreateFileW(noticePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                char buffer[4096]{};
                DWORD read = 0;
                if (ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr) && read) {
                    wchar_t wide[4096]{};
                    const int count = MultiByteToWideChar(CP_UTF8, 0, buffer, static_cast<int>(read), wide, 4095);
                    std::wstring text(wide, count > 0 ? count : 0);
                    const auto first = text.find(L"\r\n");
                    if (first != std::wstring::npos) {
                        loadedNoticePath = text.substr(0, first);
                        auto rest = text.substr(first + 2);
                        const auto second = rest.find(L"\r\n");
                        loadedNoticeSummary = second == std::wstring::npos ? rest : rest.substr(0, second);
                    }
                }
                CloseHandle(file);
                DeleteFileW(noticePath);
            }
        }
    }
    if (!loadedNoticeSummary.empty() || !internalError.empty()) {
        notice.present = true;
        notice.reportPath = loadedNoticePath;
        notice.summary = !loadedNoticeSummary.empty() ? loadedNoticeSummary : internalError;
    }
    return notice;
}

void NoteInternalError(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(noticeMutex);
    internalError = text;
}

} // namespace clickshade
