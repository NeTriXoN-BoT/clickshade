#!/usr/bin/env python3
"""Generate ClickShade's original click and typing sounds with the standard library.

Every file is synthesized deterministically here: nothing is recorded, sampled
from an operating system or taken from a commercial library. Mouse styles live
in sounds/<style>/<style>-{left,right,middle}.wav; keyboard sets live in
sounds/keyboard/<set>/ with key-1..key-4.wav plus space.wav, enter.wav and
backspace.wav.

The six original styles (soft, glass, wood, tick, bubble, arcade) use the
same formulas as before, so regenerating leaves those files unchanged.
"""
from pathlib import Path
import math
import random
import struct
import wave

RATE = 48000
ROOT = Path(__file__).resolve().parents[1] / 'sounds'

# ------------------------------------------------------------ original styles
LEGACY_STYLES = {
    'soft': (0.055, 1700.0),
    'glass': (0.130, 2100.0),
    'wood': (0.075, 760.0),
    'tick': (0.035, 2600.0),
    'bubble': (0.100, 610.0),
    'arcade': (0.095, 1120.0),
}
BUTTONS = {'left': (1.0, 17), 'right': (0.80, 29), 'middle': (1.20, 43)}


def write_wave(path: Path, samples) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    for value in samples:
        data.extend(struct.pack('<h', round(max(-1.0, min(1.0, value)) * 32767)))
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(bytes(data))


def legacy(style: str, pitch: float, duration: float, seed: int):
    randomizer = random.Random(seed)
    previous = 0.0
    count = round(duration * RATE)
    out = []
    for index in range(count):
        time = index / RATE
        noise = randomizer.uniform(-1.0, 1.0)
        transient = noise - previous
        previous = noise
        if style == 'soft':
            value = 0.13 * transient * math.exp(-time * 430) + 0.16 * math.sin(2 * math.pi * pitch * time) * math.exp(-time * 160)
        elif style == 'glass':
            value = (0.17 * math.sin(2 * math.pi * pitch * time) + 0.065 * math.sin(2 * math.pi * pitch * 2.71 * time)) * math.exp(-time * 48)
        elif style == 'wood':
            value = 0.18 * math.sin(2 * math.pi * pitch * time) * math.exp(-time * 100) + 0.09 * noise * math.exp(-time * 240)
        elif style == 'tick':
            value = (0.15 * math.sin(2 * math.pi * pitch * time) + 0.09 * transient) * math.exp(-time * 260)
        elif style == 'bubble':
            phase = 2 * math.pi * pitch * (time + 4.5 * time * time)
            value = (0.19 * math.sin(phase) + 0.04 * math.sin(2 * phase)) * math.exp(-time * 70)
        else:
            phase = 2 * math.pi * pitch * (time + 2.0 * time * time)
            value = (0.15 * math.sin(phase) + 0.045 * math.sin(3 * phase)) * math.exp(-time * 75)
        envelope = min(1.0, index / 16.0) * min(1.0, (count - 1 - index) / 120.0)
        out.append(value * envelope)
    return out


# ----------------------------------------------------------- synthesis tools
class Noise:
    def __init__(self, seed: int):
        self.random = random.Random(seed)

    def white(self):
        return self.random.uniform(-1.0, 1.0)


class Resonator:
    """Two-pole band-pass resonator: a struck body, a plastic shell, a spring."""

    def __init__(self, frequency: float, decay: float):
        # decay: seconds for the amplitude to fall to 1/e.
        r = math.exp(-1.0 / (decay * RATE))
        self.a1 = -2.0 * r * math.cos(2 * math.pi * frequency / RATE)
        self.a2 = r * r
        self.gain = 1.0 - r
        self.y1 = self.y2 = 0.0

    def tick(self, x: float) -> float:
        y = self.gain * x - self.a1 * self.y1 - self.a2 * self.y2
        self.y2, self.y1 = self.y1, y
        return y


class OnePole:
    def __init__(self, frequency: float, high_pass: bool = False):
        self.k = 1.0 - math.exp(-2 * math.pi * frequency / RATE)
        self.state = 0.0
        self.high_pass = high_pass

    def tick(self, x: float) -> float:
        self.state += self.k * (x - self.state)
        return x - self.state if self.high_pass else self.state


def envelope(time: float, attack: float, decay: float) -> float:
    if time < 0:
        return 0.0
    rise = min(1.0, time / attack) if attack > 0 else 1.0
    return rise * math.exp(-time / decay)


def normalize(samples, peak: float):
    maximum = max(abs(v) for v in samples) or 1.0
    scale = peak / maximum
    count = len(samples)
    return [v * scale * min(1.0, (count - 1 - i) / 96.0) * min(1.0, i / 6.0) for i, v in enumerate(samples)]


def transient(seed, duration, *, bursts, bodies, noise_hp=1800.0, noise_lp=14000.0, thump=None, peak=0.7):
    """A percussive mechanical sound.

    bursts: list of (start, length, level, brightness) broadband clicks.
    bodies: list of (frequency, decay, level, start) resonators excited by the bursts.
    thump:  optional (frequency, decay, level) low body knock under the first burst.
    """
    noise = Noise(seed)
    count = round(duration * RATE)
    hp = OnePole(noise_hp, high_pass=True)
    lp = OnePole(noise_lp)
    resonators = [(Resonator(f, d), level, start) for (f, d, level, start) in bodies]
    out = []
    for index in range(count):
        time = index / RATE
        raw = lp.tick(hp.tick(noise.white()))
        excitation = 0.0
        for (start, length, level, brightness) in bursts:
            if time >= start:
                shaped = raw if brightness >= 1.0 else raw * brightness + noise.white() * (1 - brightness) * 0.3
                excitation += level * shaped * envelope(time - start, 0.0003, length)
        value = excitation
        for resonator, level, start in resonators:
            drive = excitation if time >= start else 0.0
            value += level * resonator.tick(drive)
        if thump:
            frequency, decay, level = thump
            value += level * math.sin(2 * math.pi * frequency * time) * envelope(time, 0.001, decay)
        out.append(value)
    return normalize(out, peak)


# ------------------------------------------------------------- mouse styles
def mouse_classic(button_scale: float, seed: int):
    # A real microswitch: a sharp pressed click, a small bounce ~6 ms later and
    # a short plastic-shell resonance around 2.4 kHz.
    pitch = 2400.0 * button_scale
    return transient(seed, 0.06,
                     bursts=[(0.0, 0.0012, 1.0, 1.0), (0.0062, 0.0007, 0.42, 1.0)],
                     bodies=[(pitch, 0.0045, 0.9, 0.0), (pitch * 1.9, 0.0025, 0.35, 0.0), (430.0 * button_scale, 0.012, 0.25, 0.0)],
                     noise_hp=1500.0, thump=(170.0 * button_scale, 0.015, 0.08), peak=0.72)


def mouse_mechanical(button_scale: float, seed: int):
    # Heavier switch with a deeper shell and a longer settle.
    pitch = 1500.0 * button_scale
    return transient(seed, 0.085,
                     bursts=[(0.0, 0.0016, 1.0, 1.0), (0.009, 0.0012, 0.55, 0.8), (0.021, 0.0008, 0.22, 0.6)],
                     bodies=[(pitch, 0.008, 0.9, 0.0), (pitch * 2.6, 0.003, 0.3, 0.0), (310.0 * button_scale, 0.02, 0.35, 0.0)],
                     noise_hp=900.0, thump=(120.0 * button_scale, 0.025, 0.14), peak=0.75)


def mouse_pop(button_scale: float, seed: int):
    # A rounded pop: fast pitch drop with a soft airy tail.
    noise = Noise(seed)
    count = round(0.07 * RATE)
    lp = OnePole(3000.0)
    out = []
    for index in range(count):
        time = index / RATE
        frequency = (900.0 + 1400.0 * math.exp(-time * 260)) * button_scale
        phase = 2 * math.pi * frequency * time
        value = math.sin(phase) * envelope(time, 0.0008, 0.014)
        value += 0.25 * lp.tick(noise.white()) * envelope(time, 0.0005, 0.006)
        out.append(value)
    return normalize(out, 0.7)


def mouse_laser(button_scale: float, seed: int):
    # Clean sci-fi zap: descending chirp with a harmonic and a crisp onset.
    noise = Noise(seed)
    count = round(0.11 * RATE)
    out = []
    phase = 0.0
    for index in range(count):
        time = index / RATE
        frequency = (2600.0 * math.exp(-time * 24) + 180.0) * button_scale
        phase += 2 * math.pi * frequency / RATE
        value = (0.8 * math.sin(phase) + 0.25 * math.sin(2 * phase) + 0.08 * math.sin(3 * phase)) * envelope(time, 0.002, 0.03)
        value += 0.18 * noise.white() * envelope(time, 0.0002, 0.0015)
        out.append(value)
    return normalize(out, 0.66)


def mouse_retro(button_scale: float, seed: int):
    # 8-bit blip: square wave steps with a tiny noise onset.
    noise = Noise(seed)
    count = round(0.075 * RATE)
    out = []
    phase = 0.0
    for index in range(count):
        time = index / RATE
        step = 0 if time < 0.025 else 1 if time < 0.05 else 2
        frequency = [880.0, 1175.0, 1568.0][step] * button_scale
        phase += 2 * math.pi * frequency / RATE
        square = 1.0 if math.sin(phase) >= 0 else -1.0
        value = 0.6 * square * envelope(time, 0.001, 0.05) + 0.1 * noise.white() * envelope(time, 0.0002, 0.002)
        out.append(value)
    return normalize(out, 0.62)


MOUSE_STYLES = {
    'classic': mouse_classic,
    'mechanical': mouse_mechanical,
    'pop': mouse_pop,
    'laser': mouse_laser,
    'retro': mouse_retro,
}


# ----------------------------------------------------------- keyboard sets
def key_classic(kind: str, variant: int, seed: int):
    # Office rubber-dome keyboard: a muffled thud with a soft plastic tap.
    scale = {'key': 1.0, 'space': 0.72, 'enter': 0.85, 'backspace': 1.05}[kind]
    detune = 1.0 + 0.03 * (variant - 1.5)
    bursts = [(0.0, 0.0018, 0.8, 0.5), (0.028, 0.0012, 0.5, 0.4)]
    if kind == 'space':
        bursts.append((0.034, 0.0015, 0.5, 0.4))  # Stabilizer rattle.
    return transient(seed, 0.09, bursts=bursts,
                     bodies=[(330.0 * scale * detune, 0.016, 1.0, 0.0), (1150.0 * scale * detune, 0.004, 0.35, 0.0)],
                     noise_hp=400.0, noise_lp=6500.0, thump=(95.0 * scale, 0.03, 0.22), peak=0.55)


def key_mechanical_blue(kind: str, variant: int, seed: int):
    # Clicky switch: bright metallic click jacket, then the bottom-out clack.
    scale = {'key': 1.0, 'space': 0.7, 'enter': 0.82, 'backspace': 1.0}[kind]
    detune = 1.0 + 0.025 * (variant - 1.5)
    bursts = [(0.0, 0.0009, 1.0, 1.0), (0.016, 0.0014, 0.75, 0.8)]
    if kind == 'space':
        bursts.append((0.021, 0.0012, 0.45, 0.7))
    return transient(seed, 0.1, bursts=bursts,
                     bodies=[(4300.0 * detune, 0.0025, 0.8, 0.0), (2200.0 * detune, 0.004, 0.4, 0.0),
                             (620.0 * scale * detune, 0.012, 0.8, 0.016), (1500.0 * scale * detune, 0.005, 0.3, 0.016)],
                     noise_hp=1200.0, noise_lp=16000.0, thump=(110.0 * scale, 0.022, 0.16), peak=0.6)


def key_mechanical_brown(kind: str, variant: int, seed: int):
    # Tactile switch: no click jacket, a deep "thock" with a damped case.
    scale = {'key': 1.0, 'space': 0.68, 'enter': 0.8, 'backspace': 1.0}[kind]
    detune = 1.0 + 0.03 * (variant - 1.5)
    bursts = [(0.0, 0.0014, 0.55, 0.6), (0.014, 0.0022, 1.0, 0.5)]
    if kind == 'space':
        bursts.append((0.019, 0.0018, 0.5, 0.5))
    return transient(seed, 0.1, bursts=bursts,
                     bodies=[(480.0 * scale * detune, 0.02, 1.0, 0.014), (1250.0 * scale * detune, 0.006, 0.35, 0.014), (2900.0 * detune, 0.002, 0.18, 0.0)],
                     noise_hp=350.0, noise_lp=9000.0, thump=(88.0 * scale, 0.035, 0.3), peak=0.6)


def key_laptop(kind: str, variant: int, seed: int):
    # Low-travel chiclet: very short, light, slightly papery.
    scale = {'key': 1.0, 'space': 0.8, 'enter': 0.9, 'backspace': 1.0}[kind]
    detune = 1.0 + 0.04 * (variant - 1.5)
    return transient(seed, 0.05,
                     bursts=[(0.0, 0.0011, 1.0, 0.7), (0.007, 0.0009, 0.45, 0.6)],
                     bodies=[(1500.0 * scale * detune, 0.0035, 0.8, 0.0), (650.0 * scale * detune, 0.008, 0.45, 0.0)],
                     noise_hp=700.0, noise_lp=11000.0, thump=(140.0 * scale, 0.012, 0.1), peak=0.5)


def key_typewriter(kind: str, variant: int, seed: int):
    # Manual typewriter: hammer strike ring plus a mechanical clack.
    scale = {'key': 1.0, 'space': 0.6, 'enter': 0.7, 'backspace': 1.0}[kind]
    detune = 1.0 + 0.02 * (variant - 1.5)
    bursts = [(0.0, 0.0012, 1.0, 1.0), (0.011, 0.0025, 0.9, 0.7), (0.04, 0.0015, 0.4, 0.6)]
    if kind == 'enter':
        bursts += [(0.07, 0.003, 0.9, 0.5), (0.1, 0.002, 0.5, 0.5)]  # Carriage return lever.
    return transient(seed, 0.16 if kind == 'enter' else 0.12, bursts=bursts,
                     bodies=[(3600.0 * detune, 0.006, 0.9, 0.0), (5400.0 * detune, 0.003, 0.4, 0.0),
                             (760.0 * scale * detune, 0.015, 0.8, 0.011), (260.0 * scale, 0.03, 0.4, 0.011)],
                     noise_hp=600.0, noise_lp=15000.0, thump=(105.0 * scale, 0.03, 0.2), peak=0.62)


KEYBOARD_SETS = {
    'classic': key_classic,
    'mechanical-blue': key_mechanical_blue,
    'mechanical-brown': key_mechanical_brown,
    'laptop': key_laptop,
    'typewriter': key_typewriter,
}


def main() -> None:
    generated = 0
    for style_index, (style, (duration, pitch)) in enumerate(LEGACY_STYLES.items()):
        for button, (multiplier, seed) in BUTTONS.items():
            write_wave(ROOT / style / f'{style}-{button}.wav', legacy(style, pitch * multiplier, duration, seed + style_index * 101))
            generated += 1
    for style_index, (style, synth) in enumerate(MOUSE_STYLES.items()):
        for button, (multiplier, seed) in BUTTONS.items():
            write_wave(ROOT / style / f'{style}-{button}.wav', synth(multiplier, seed + 1000 + style_index * 97))
            generated += 1
    for set_index, (name, synth) in enumerate(KEYBOARD_SETS.items()):
        folder = ROOT / 'keyboard' / name
        for variant in range(4):
            write_wave(folder / f'key-{variant + 1}.wav', synth('key', variant, 5000 + set_index * 131 + variant * 7))
            generated += 1
        for kind in ('space', 'enter', 'backspace'):
            write_wave(folder / f'{kind}.wav', synth(kind, 1, 7000 + set_index * 131 + len(kind)))
            generated += 1
    print(f'Generated {generated} original PCM WAV files in {ROOT}')


if __name__ == '__main__':
    main()
