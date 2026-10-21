// Pure configuration/animation logic, shared with the headless regression suite.
// Colors deliberately match the Windows/Linux INI: r | g << 8 | b << 16.
export const HIGHLIGHT_STYLES = Object.freeze({FILLED: 0, RING: 1, GLOW: 2, CROSSHAIR: 3, SPOTLIGHT: 4});
export const CLICK_STYLES = Object.freeze({RING: 0, PULSE: 1, RIPPLE: 2, BURST: 3, NEON: 4, TARGET: 5});
export const KEY_CATEGORIES = Object.freeze({GENERIC: 0, SPACE: 1, ENTER: 2, BACKSPACE: 3});

export const DEFAULTS = Object.freeze({
    Enabled: true, HighlightEnabled: true, HighlightStyle: HIGHLIGHT_STYLES.FILLED,
    HighlightRadius: 24, HighlightOpacity: 35, HighlightThickness: 3,
    HighlightColor: 0x40d6ff, HighlightBreathing: false, HighlightIdleHideMs: 0,
    SpotlightDim: 55, SpotlightRadius: 140, ClickEffectsEnabled: true,
    LeftClickEnabled: true, RightClickEnabled: true, MiddleClickEnabled: false,
    ClickStyle: CLICK_STYLES.RING,
    ClickRadius: 42, ClickDurationMs: 360, ClickOpacity: 80, ClickThickness: 3,
    ClickEasing: 1, VisualRefreshRate: 0,
    LeftClickColor: 0xffc800, RightClickColor: 0x3131ff, MiddleClickColor: 0x14ff39,
    PauseFullscreen: true, PauseConfinedCursor: true,
    ExcludedProcesses: '', AllowedProcesses: '', ClickSoundsEnabled: false,
    KeyboardSoundsEnabled: false,
    MagnifierEnabled: false, MagnifierZoom: 200, MagnifierSize: 220, MagnifierBorderColor: 0xffffff, MagnifierFrame: true,
    KeyDisplayEnabled: false, KeyDisplayMode: 0, KeyDisplayMouse: false, KeyDisplayPosition: 0,
    KeyDisplaySize: 100, KeyDisplayDurationMs: 1500, KeyDisplayOpacity: 90, KeyDisplayColor: 0x261f1c,
    // Preview builds wrote these two switches instead of the style numbers.
    HighlightFilled: true, ClickFilled: false,
});

const RANGES = Object.freeze({
    HighlightStyle: [0, 4], ClickStyle: [0, 5],
    HighlightRadius: [8, 100], HighlightOpacity: [5, 100], HighlightThickness: [1, 12],
    SpotlightDim: [10, 90], SpotlightRadius: [40, 400],
    ClickRadius: [12, 120], ClickDurationMs: [100, 1500], ClickOpacity: [5, 100],
    ClickThickness: [1, 12], ClickEasing: [0, 2],
    HighlightColor: [0, 0xffffff], LeftClickColor: [0, 0xffffff], RightClickColor: [0, 0xffffff],
    MiddleClickColor: [0, 0xffffff],
    MagnifierZoom: [125, 800], MagnifierSize: [80, 600], MagnifierBorderColor: [0, 0xffffff],
    KeyDisplayMode: [0, 1], KeyDisplayPosition: [0, 3], KeyDisplaySize: [60, 250],
    KeyDisplayDurationMs: [300, 6000], KeyDisplayOpacity: [20, 100], KeyDisplayColor: [0, 0xffffff],
});

// Modifier flags for key labels; MOUSE marks a mouse button instead of a key.
export const KEY_MODIFIERS = Object.freeze({CONTROL: 1, SHIFT: 2, ALT: 4, SUPER: 8, ALTGR: 16, MOUSE: 32});
// Clutter.ModifierType bits, spelled out so this module stays importable in Node.
const CLUTTER_SHIFT = 1, CLUTTER_CONTROL = 4, CLUTTER_MOD1 = 8, CLUTTER_MOD4 = 64, CLUTTER_MOD5 = 128;
const MODIFIER_KEYSYMS = new Map([
    [0xffe3, KEY_MODIFIERS.CONTROL], [0xffe4, KEY_MODIFIERS.CONTROL], [0xffe1, KEY_MODIFIERS.SHIFT], [0xffe2, KEY_MODIFIERS.SHIFT],
    [0xffe9, KEY_MODIFIERS.ALT], [0xffea, KEY_MODIFIERS.ALT], [0xffe7, KEY_MODIFIERS.ALT], [0xffe8, KEY_MODIFIERS.ALT],
    [0xffeb, KEY_MODIFIERS.SUPER], [0xffec, KEY_MODIFIERS.SUPER], [0xffed, KEY_MODIFIERS.SUPER], [0xffee, KEY_MODIFIERS.SUPER],
    [0xfe03, KEY_MODIFIERS.ALTGR], [0xff7e, KEY_MODIFIERS.ALTGR],
]);
const NAMED_KEYSYMS = new Map([
    [0xff1b, 'Esc'], [0xff09, 'Tab'], [0xfe20, 'Tab'], [0xffe5, 'Caps Lock'], [0xff0d, 'Enter'], [0xff8d, 'Enter'],
    [0xff08, 'Backspace'], [0x020, 'Space'], [0xff63, 'Insert'], [0xff9e, 'Insert'], [0xffff, 'Delete'], [0xff9f, 'Delete'],
    [0xff50, 'Home'], [0xff95, 'Home'], [0xff57, 'End'], [0xff9c, 'End'], [0xff55, 'Page Up'], [0xff9a, 'Page Up'],
    [0xff56, 'Page Down'], [0xff9b, 'Page Down'], [0xff51, '←'], [0xff96, '←'], [0xff52, '↑'], [0xff97, '↑'],
    [0xff53, '→'], [0xff98, '→'], [0xff54, '↓'], [0xff99, '↓'], [0xff9d, 'Clear'], [0xff61, 'Print Screen'],
    [0xff14, 'Scroll Lock'], [0xff13, 'Pause'], [0xff7f, 'Num Lock'], [0xff67, 'Menu'], [0xffaa, 'Num *'], [0xffab, 'Num +'],
    [0xffad, 'Num -'], [0xffae, 'Num .'], [0xffac, 'Num .'], [0xffaf, 'Num /'],
    [0x1008ff12, 'Mute'], [0x1008ff11, 'Volume Down'], [0x1008ff13, 'Volume Up'], [0x1008ff17, 'Next Track'],
    [0x1008ff16, 'Previous Track'], [0x1008ff15, 'Stop'], [0x1008ff14, 'Play/Pause'], [0x1008ff26, 'Back'], [0x1008ff27, 'Forward'],
]);

export function isModifierKeysym(keysym) { return MODIFIER_KEYSYMS.has(keysym); }
export function modifierBit(keysym) { return MODIFIER_KEYSYMS.get(keysym) ?? 0; }

// Clutter event state → the shared modifier flags (lock modifiers ignored).
export function modifiersFromState(state) {
    return ((state & CLUTTER_CONTROL) ? KEY_MODIFIERS.CONTROL : 0) | ((state & CLUTTER_SHIFT) ? KEY_MODIFIERS.SHIFT : 0) |
        ((state & CLUTTER_MOD1) ? KEY_MODIFIERS.ALT : 0) | ((state & CLUTTER_MOD4) ? KEY_MODIFIERS.SUPER : 0) |
        ((state & CLUTTER_MOD5) ? KEY_MODIFIERS.ALTGR : 0);
}

// Shortcuts-only mode: chords with Ctrl, Alt or Super, function keys, Esc,
// Print and a tapped modifier. AltGr characters count as typing.
export function keyDisplayWanted(mode, keysym, modifiers) {
    if (modifiers & KEY_MODIFIERS.MOUSE) return true;
    if (mode !== 0) return true;
    if (modifiers & KEY_MODIFIERS.ALTGR) return false;
    if (modifiers & (KEY_MODIFIERS.CONTROL | KEY_MODIFIERS.ALT | KEY_MODIFIERS.SUPER)) return true;
    if (keysym >= 0xffbe && keysym <= 0xffe0) return true; // F1..F35
    return keysym === 0xff1b || keysym === 0xff61 || isModifierKeysym(keysym);
}

export function keyName(keysym, modifiers = 0) {
    if (modifiers & KEY_MODIFIERS.MOUSE)
        return {1: 'Left click', 2: 'Right click', 4: 'Middle click', 8: 'Back button', 16: 'Forward button'}[keysym] ?? 'Mouse';
    if (keysym >= 0xffbe && keysym <= 0xffe0) return `F${keysym - 0xffbe + 1}`;
    if (keysym >= 0xffb0 && keysym <= 0xffb9) return `Num ${keysym - 0xffb0}`;
    const bit = modifierBit(keysym);
    if (bit === KEY_MODIFIERS.CONTROL) return 'Ctrl';
    if (bit === KEY_MODIFIERS.SHIFT) return 'Shift';
    if (bit === KEY_MODIFIERS.ALT) return (modifiers & KEY_MODIFIERS.ALTGR) ? 'AltGr' : keysym === 0xffea || keysym === 0xffe8 ? 'Right Alt' : 'Alt';
    if (bit === KEY_MODIFIERS.SUPER) return 'Super';
    if (bit === KEY_MODIFIERS.ALTGR) return 'AltGr';
    const named = NAMED_KEYSYMS.get(keysym);
    if (named) return named;
    if (keysym >= 0x20 && keysym <= 0x7e) return String.fromCharCode(keysym).toUpperCase();
    if (keysym >= 0xa0 && keysym <= 0xff) return String.fromCharCode(keysym).toUpperCase();
    if ((keysym & 0xff000000) === 0x01000000) return String.fromCodePoint(keysym & 0x00ffffff).toUpperCase();
    return 'Key';
}

// The keys of a chord in display order (Super, Ctrl, Alt, Shift, then the
// key); a modifier that is itself the key is not repeated.
export function keyChordParts(keysym, modifiers) {
    let own = (modifiers & KEY_MODIFIERS.MOUSE) ? 0 : modifierBit(keysym);
    if (own & KEY_MODIFIERS.ALT) own |= KEY_MODIFIERS.ALTGR;
    const shown = modifiers & ~own;
    const parts = [];
    if (shown & KEY_MODIFIERS.SUPER) parts.push('Super');
    if (shown & KEY_MODIFIERS.ALTGR) parts.push('AltGr');
    else {
        if (shown & KEY_MODIFIERS.CONTROL) parts.push('Ctrl');
        if (shown & KEY_MODIFIERS.ALT) parts.push('Alt');
    }
    if (shown & KEY_MODIFIERS.SHIFT) parts.push('Shift');
    parts.push(keyName(keysym, modifiers));
    return parts;
}

// "Super + Shift + S" style text of the same chord.
export function keyChordLabel(keysym, modifiers) { return keyChordParts(keysym, modifiers).join(' + '); }

const SHORTCUT_MODIFIERS = KEY_MODIFIERS.CONTROL | KEY_MODIFIERS.ALT | KEY_MODIFIERS.SUPER | KEY_MODIFIERS.MOUSE;

// The character a key types: unicode is Clutter's get_key_unicode() for the
// event (it already applies Shift, Caps Lock and AltGr). Shortcuts, control
// characters and keys without text type nothing.
export function typedText(unicode, modifiers) {
    if (modifiers & SHORTCUT_MODIFIERS) return '';
    if (!unicode || unicode < 0x20 || unicode === 0x7f || (unicode >= 0x80 && unicode < 0xa0) || unicode > 0x10ffff) return '';
    return String.fromCodePoint(unicode);
}

export const KEY_ROW_LIMIT = 3;
const TYPED_LIMIT = 120;

// Adds one key event to the key display rows (oldest first) and returns the
// new rows. Typed characters join the row still being written, Backspace
// edits it, and a shortcut repeated within a second counts up.
export function addKeyStroke(rows, {caps, typed = '', backspace = false}, now, holdMicros) {
    const result = rows.map(row => ({...row}));
    const last = result.at(-1);
    const typing = Boolean(last?.typed) && now - last.updated < holdMicros;
    if (typing && typed) {
        last.typed = Array.from(last.typed + typed).slice(-TYPED_LIMIT).join('');
        last.updated = now;
    } else if (typing && backspace) {
        last.typed = Array.from(last.typed).slice(0, -1).join('');
        if (last.typed) last.updated = now; else result.pop();
    } else if (typed && typed !== ' ') {
        result.push({caps: [], typed, repeat: 1, updated: now});
    } else if (last && !last.typed && last.caps.join('\n') === caps.join('\n') && now - last.updated < 1000000) {
        last.repeat += 1;
        last.updated = now;
    } else {
        result.push({caps: [...caps], typed: '', repeat: 1, updated: now});
    }
    while (result.length > KEY_ROW_LIMIT) result.shift();
    return result;
}

// Where a lens of the given size goes for a pointer at (x, y): above and to
// the right, flipped to stay inside the monitor.
export function lensOrigin(x, y, size, monitor) {
    const offset = Math.round(size * 0.75);
    let cx = x + offset, cy = y - offset;
    if (cx + size / 2 > monitor.x + monitor.width) cx = x - offset;
    if (cy - size / 2 < monitor.y) cy = y + offset;
    return {x: Math.round(cx - size / 2), y: Math.round(cy - size / 2)};
}

export function normalize(values = {}) {
    const result = {...DEFAULTS};
    for (const [key, fallback] of Object.entries(DEFAULTS)) {
        if (!Object.hasOwn(values, key)) continue;
        const value = values[key];
        if (typeof fallback === 'boolean') {
            if (typeof value === 'boolean') result[key] = value;
            else if (/^(1|true|yes)$/i.test(String(value))) result[key] = true;
            else if (/^(0|false|no)$/i.test(String(value))) result[key] = false;
        } else if (key === 'ExcludedProcesses' || key === 'AllowedProcesses') {
            result[key] = String(value).slice(0, 8192);
        } else {
            if (!/^[+-]?\d+$/.test(String(value).trim())) continue;
            const number = Number(value);
            if (!Number.isSafeInteger(number)) continue;
            if (key === 'VisualRefreshRate')
                result[key] = number === 0 ? 0 : Math.max(30, Math.min(360, number));
            else if (key === 'HighlightIdleHideMs')
                result[key] = number <= 0 ? 0 : Math.max(500, Math.min(10000, number));
            else if (RANGES[key])
                result[key] = Math.max(RANGES[key][0], Math.min(RANGES[key][1], number));
        }
    }
    // A preview-build settings file has the shape switches but no style numbers.
    if (!Object.hasOwn(values, 'HighlightStyle') && Object.hasOwn(values, 'HighlightFilled'))
        result.HighlightStyle = result.HighlightFilled ? HIGHLIGHT_STYLES.FILLED : HIGHLIGHT_STYLES.RING;
    if (!Object.hasOwn(values, 'ClickStyle') && Object.hasOwn(values, 'ClickFilled'))
        result.ClickStyle = result.ClickFilled ? CLICK_STYLES.PULSE : CLICK_STYLES.RING;
    return result;
}

export function parseIni(text) {
    if (typeof text !== 'string' || text.length > 131072)
        throw new Error('ClickShade settings must be a UTF-8 INI smaller than 128 KiB.');
    const values = Object.create(null);
    let active = false;
    for (const raw of text.replace(/^﻿/, '').split(/\r?\n/)) {
        const line = raw.trim();
        if (!line || line.startsWith(';') || line.startsWith('#')) continue;
        if (line.startsWith('[')) { active = line === '[ClickShade]'; continue; }
        if (!active) continue;
        const separator = line.indexOf('=');
        if (separator < 0) continue;
        const key = line.slice(0, separator).trim();
        if (Object.hasOwn(DEFAULTS, key)) values[key] = line.slice(separator + 1).trim();
    }
    return normalize(values);
}

export function colorChannels(color) {
    return [(color & 255) / 255, ((color >>> 8) & 255) / 255, ((color >>> 16) & 255) / 255];
}

export function curve(fraction, easing) {
    const t = Math.max(0, Math.min(1, fraction));
    const remaining = 1 - t;
    if (easing === 1) return [1 - remaining ** 3, remaining ** 2];
    if (easing === 2) {
        const expansion = t * t * (3 - 2 * t);
        return [expansion, 1 - expansion];
    }
    return [t, remaining];
}

// Breathing: a slow 2.6 s swell of radius and opacity, as on Windows and X11.
export function breath(microseconds) {
    const phase = Math.sin(microseconds / 1e6 * 2 * Math.PI / 2.6);
    return {radius: 0.92 + 0.08 * phase, opacity: 0.76 + 0.24 * phase};
}

export function excluded(configured, identities) {
    const names = new Set(identities.filter(Boolean).map(name =>
        String(name).toLowerCase().replace(/\.desktop$/, '')));
    return String(configured).split(/[;,\n]/).some(item => {
        const name = item.trim().toLowerCase().replace(/\.desktop$/, '');
        return name.length > 0 && names.has(name);
    });
}

export function effectiveRate(requested, monitorRate) {
    const selected = requested || (Number.isFinite(monitorRate) && monitorRate > 1 ? monitorRate : 60);
    return Math.max(30, Math.min(360, selected));
}

// Maintain phase through sub-frame timer rounding; skip whole missed frames.
// Scheduling only one source and never looping through stale deadlines avoids
// a burst of catch-up work after a busy compositor/main thread.
export function nextDeadline(previous, now, interval) {
    return previous > 0 && now >= previous && now - previous < interval
        ? previous + interval : now + interval;
}

export function buttonEdges(previous, current) {
    return (current & ~previous) & 7;
}

// Keysyms shared by Clutter and X11: space, Return, KP_Enter, BackSpace.
export function keyCategory(keysym) {
    if (keysym === 0x020) return KEY_CATEGORIES.SPACE;
    if (keysym === 0xff0d || keysym === 0xff8d) return KEY_CATEGORIES.ENTER;
    if (keysym === 0xff08) return KEY_CATEGORIES.BACKSPACE;
    return KEY_CATEGORIES.GENERIC;
}
