#pragma once

#include "settings.h"
#include <memory>
#include <string>

namespace clickshade::linuxapp {

// Caches the start of PCM/float WAV files and streams bounded source windows on
// a dedicated worker. Clip duration does not determine memory use. Optional libpulse
// output is loaded at runtime and works with PulseAudio or PipeWire's Pulse
// server. Click/Key/Preview never read files, decode audio, or wait for playback.
// The stream is corked while nothing plays, so the sound server can suspend
// the device, and closed while both sound groups are switched off.
//
// Mouse buttons and the keyboard are separate groups with their own volume. A
// keyboard set is a folder with key*.wav variants plus optional space.wav,
// enter.wav and backspace.wav, or a single WAV used for every key. Only the
// coarse KeyCategory reaches this class; no key identity is ever stored.
class LinuxAudio {
public:
    LinuxAudio();
    ~LinuxAudio();
    LinuxAudio(const LinuxAudio&) = delete;
    LinuxAudio& operator=(const LinuxAudio&) = delete;
    bool Start(const Settings&, std::string& error);
    void Stop() noexcept;
    void Update(const Settings&);
    // Mouse and keyboard sounds have separate allowances: an application that
    // hides the pointer pauses click sounds while typing sounds continue.
    void SetAllowed(bool mouse, bool keyboard) noexcept;
    void SetAllowed(bool value) noexcept { SetAllowed(value, value); }
    void Click(unsigned buttonMask) noexcept;
    void Key(unsigned category) noexcept;
    // Previews ignore the sound toggles, but still obey their group's allowance.
    void Preview(unsigned buttonMask) noexcept;
    void PreviewKey() noexcept;
    std::string LastError() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clickshade::linuxapp
