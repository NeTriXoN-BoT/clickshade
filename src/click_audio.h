#pragma once

#include "settings.h"
#include <memory>
#include <string>

namespace clickshade {

// File access, decoding and mixing live on one dedicated worker that feeds a
// shared-mode WASAPI stream (waveOut when WASAPI is unavailable). Click, Key
// and Preview only bump bounded atomic counters and signal an event; they
// never touch files, decoders or the audio device.
//
// Sources: left/right/middle mouse files and a keyboard set (a folder with
// key*/space/enter/backspace files, or one file used for every key).
class ClickAudio {
public:
    ClickAudio();
    ~ClickAudio();
    ClickAudio(const ClickAudio&) = delete;
    ClickAudio& operator=(const ClickAudio&) = delete;

    bool Start(const Settings& settings, std::wstring& error);
    void Stop();
    void UpdateSettings(const Settings& settings);
    void SetAllowed(bool allowed) noexcept;
    void Click(unsigned buttonMask) noexcept;
    // Category only (KeyCategory); no key identity reaches this object.
    void Key(unsigned category) noexcept;
    // Preview ignores the enable checkboxes, but respects SetAllowed.
    void Preview(unsigned buttonMask) noexcept;
    void PreviewKey() noexcept;
    // True once the worker runs and has prepared the current sources; requests
    // sent while a reload is in progress are dropped.
    bool Ready() const noexcept;
    std::wstring LastError() const;
    // "WASAPI shared", "waveOut" or empty while no output is open.
    std::wstring OutputDescription() const;
    void SetNotifyWindow(HWND target, UINT message) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickshade
