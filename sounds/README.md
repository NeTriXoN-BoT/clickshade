# Click and keyboard sounds

These 68 small, original sounds are synthesized by `tools/generate_example_sounds.py`
and licensed with ClickShade under the MIT license. They contain no recordings,
borrowed operating-system sounds, or personal information.

## Mouse sets

| Folder | Character | Duration |
| --- | --- | --- |
| `soft/` | A gentle neutral click | 55 ms |
| `glass/` | A clear, short chime | 130 ms |
| `wood/` | A muted wooden tap | 75 ms |
| `tick/` | A crisp, brief tick | 35 ms |
| `bubble/` | A light upward pop | 100 ms |
| `arcade/` | A subtle electronic blip | 95 ms |
| `classic/` | The familiar mouse click: a sharp press transient with a short body | 45 ms |
| `mechanical/` | A heavier switch with a little body resonance | 60 ms |
| `pop/` | A rounded, bubbly pop | 70 ms |
| `laser/` | A quick downward zap | 90 ms |
| `retro/` | A square-wave console blip | 80 ms |

Each folder contains separate `-left.wav`, `-right.wav`, and `-middle.wav`
variations, so each dropdown in the app lists only the files for its button.

## Keyboard sets

| Folder | Character |
| --- | --- |
| `keyboard/classic/` | The classic typing sound: short, dry membrane keys |
| `keyboard/mechanical-blue/` | Clicky switches with a bright tactile click |
| `keyboard/mechanical-brown/` | Tactile switches with a softer bump |
| `keyboard/laptop/` | Low-profile scissor keys |
| `keyboard/typewriter/` | A mechanical typewriter with a heavier strike |

A set holds `key-1.wav` to `key-4.wav` (the app shuffles up to six variants so fast
typing never repeats one sample) plus `space.wav`, `enter.wav` and `backspace.wav`.
A folder of your own with the same names works too, and so does a single WAV used
for every key.

Every template is mono 48 kHz, 16-bit PCM WAV, with a short attack and fade to
avoid hard waveform boundaries. Start with a comfortable volume in the app; sounds
are optional and disabled by default.

The dropdowns discover files in this directory and its style folders. Browse opens
this directory beside the running executable; you can navigate elsewhere to
choose your own sound. Keep the `sounds/` folder beside the app when moving a
portable installation.

There is no ten-second playback limit. Long files are read in bounded buffers on
an audio worker; up to sixteen sounds overlap on Windows and twelve on Linux, with
separate live volumes for mouse and keyboard. PCM and floating-point RIFF WAV work
on both platforms. Windows additionally uses installed Media Foundation codecs for
supported compressed audio. Linux uses WAV. The file format's own size limits still
apply; ordinary RIFF WAV has 32-bit chunk lengths.

To regenerate every bundled template without third-party dependencies:

```sh
python3 tools/generate_example_sounds.py
```
