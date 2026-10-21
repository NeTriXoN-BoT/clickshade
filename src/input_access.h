#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <cstdint>

namespace clickshade {

struct InputTokenFacts {
    DWORD integrity = 0;
    bool integrityKnown = false;
    bool uiAccess = false;
};

// Inspect only the small security fields needed to decide whether a zero
// GetAsyncKeyState result is trustworthy. No privilege is enabled or requested.
inline InputTokenFacts ReadInputTokenFacts(HANDLE process) noexcept {
    InputTokenFacts result;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return result;
    DWORD returned = 0;
    DWORD uiAccess = 0;
    if (GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof(uiAccess), &returned))
        result.uiAccess = uiAccess != 0;
    alignas(TOKEN_MANDATORY_LABEL)
        std::array<unsigned char, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> bytes{};
    if (GetTokenInformation(token, TokenIntegrityLevel, bytes.data(),
                            static_cast<DWORD>(bytes.size()), &returned) &&
        returned >= sizeof(TOKEN_MANDATORY_LABEL) && returned <= bytes.size()) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(bytes.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(bytes.data());
        const auto sid = reinterpret_cast<std::uintptr_t>(label->Label.Sid);
        const auto end = begin + returned;
        if (sid >= begin + sizeof(TOKEN_MANDATORY_LABEL) && sid <= end && end - sid >= 8) {
            const auto* raw = reinterpret_cast<const unsigned char*>(sid);
            const unsigned count = raw[1];
            const std::size_t length = 8u + static_cast<std::size_t>(count) * sizeof(DWORD);
            if (count > 0 && count <= SID_MAX_SUB_AUTHORITIES && length <= end - sid &&
                IsValidSid(label->Label.Sid)) {
                result.integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
                result.integrityKnown = true;
            }
        }
    }
    CloseHandle(token);
    return result;
}

inline bool CanReconcileAsyncButtons(const InputTokenFacts& own,
                                    const InputTokenFacts& foreground) noexcept {
    // UIPI can make GetAsyncKeyState return zero for a still-held button.
    // Unknown or higher integrity is not evidence of release; Raw Input up
    // events, device removal and desktop/session cleanup remain authoritative.
    return own.uiAccess || (own.integrityKnown && foreground.integrityKnown &&
                            foreground.integrity <= own.integrity);
}

} // namespace clickshade
