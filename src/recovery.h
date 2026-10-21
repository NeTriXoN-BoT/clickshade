#pragma once
#include "runtime.h"

namespace clickshade {
class RecoveryGuard {
public:
    ~RecoveryGuard();
    bool Start(std::wstring& error);
    void StopCleanly();
private:
    HANDLE cleanExit_ = nullptr;
    HANDLE process_ = nullptr;
};
// Called only in the secondary recovery process. No window, timers or input.
int RunRecoveryGuard(HANDLE parent, HANDLE cleanExit);
}
