#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace clickshade {

// WinEvents are delivered while the installing thread retrieves messages.
// Even events discarded by a callback can starve input/rendering if a busy
// application emits many accessibility notifications. Keep that delivery on
// this idle, blocking receiver; consumers only coalesce their own work signals.
class WinEventListener {
public:
    struct Range { DWORD first, last; };
    using Callback = void (*)(void*, DWORD, HWND, LONG, LONG, DWORD);

    WinEventListener() = default;
    ~WinEventListener() { Stop(); }
    WinEventListener(const WinEventListener&) = delete;
    WinEventListener& operator=(const WinEventListener&) = delete;

    bool Start(std::initializer_list<Range> ranges, Callback callback,
               void* context, std::wstring& error) {
        Stop();
        error.clear();
        if (!callback || ranges.size() == 0) {
            error = L"No Windows event subscription was supplied.";
            return false;
        }
        ranges_.assign(ranges.begin(), ranges.end());
        callback_ = callback;
        context_ = context;
        ownerThread_ = GetCurrentThreadId();
        stopping_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready_ = false;
            failure_ = ERROR_SUCCESS;
        }
        try { worker_ = std::thread([this] { Run(); }); }
        catch (...) {
            error = L"Could not start the Windows event receiver.";
            return false;
        }
        DWORD failure = ERROR_SUCCESS;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            readyCondition_.wait(lock, [this] { return ready_; });
            failure = failure_;
        }
        if (failure != ERROR_SUCCESS) {
            Stop();
            error = L"Could not subscribe to Windows desktop events (Windows error " +
                    std::to_wstring(failure) + L").";
            return false;
        }
        return true;
    }

    void Stop() noexcept {
        stopping_.store(true);
        if (const DWORD id = receiverThread_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
        if (worker_.joinable()) worker_.join();
        receiverThread_.store(0);
        callback_ = nullptr;
        context_ = nullptr;
        ranges_.clear();
    }

private:
    std::thread worker_;
    std::atomic<DWORD> receiverThread_{0};
    std::atomic<bool> stopping_{false};
    std::vector<Range> ranges_;
    std::vector<HWINEVENTHOOK> hooks_; // Receiver thread only.
    std::mutex mutex_;
    std::condition_variable readyCondition_;
    bool ready_ = false;
    DWORD failure_ = ERROR_SUCCESS;
    DWORD ownerThread_ = 0;
    Callback callback_ = nullptr;
    void* context_ = nullptr;
    inline static thread_local WinEventListener* current_ = nullptr;

    static void CALLBACK EventProc(HWINEVENTHOOK, DWORD event, HWND window,
                                   LONG object, LONG child, DWORD sourceThread, DWORD) noexcept {
        WinEventListener* self = current_;
        if (!self) return;
        if (self->stopping_.load()) {
            // A continuous event producer can keep GetMessage dispatching
            // callbacks before it reaches WM_QUIT. Unsubscribe here, on the
            // installing thread, so shutdown never waits for that producer.
            self->Unsubscribe();
            PostQuitMessage(0);
            return;
        }
        if (sourceThread == self->ownerThread_) return;
        // Preserve the old skip-own-thread semantics for the *consumer*, not
        // just the newly introduced receiver thread. Never let exceptions cross
        // this Windows callback boundary.
        try { self->callback_(self->context_, event, window, object, child, sourceThread); }
        catch (...) {}
    }

    void Unsubscribe() noexcept {
        while (!hooks_.empty()) {
            const HWINEVENTHOOK hook = hooks_.back();
            hooks_.pop_back();
            UnhookWinEvent(hook);
        }
    }

    void Run() noexcept {
        current_ = this;
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        receiverThread_.store(GetCurrentThreadId());
        DWORD failure = ERROR_SUCCESS;
        try {
            hooks_.reserve(ranges_.size());
            for (const Range range : ranges_) {
                HWINEVENTHOOK hook = SetWinEventHook(range.first, range.last, nullptr,
                    EventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);
                if (!hook) {
                    failure = GetLastError();
                    if (!failure) failure = ERROR_GEN_FAILURE;
                    break;
                }
                hooks_.push_back(hook);
            }
        } catch (...) { failure = ERROR_NOT_ENOUGH_MEMORY; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = failure;
            ready_ = true;
        }
        readyCondition_.notify_one();
        if (failure == ERROR_SUCCESS) {
            while (!stopping_.load()) {
                const BOOL result = GetMessageW(&message, nullptr, 0, 0);
                if (result <= 0) break;
                // No windows or application work belong to this receiver.
                // Getting messages delivers the out-of-context WinEvents.
            }
        }
        Unsubscribe();
        receiverThread_.store(0);
        current_ = nullptr;
    }
};

} // namespace clickshade
