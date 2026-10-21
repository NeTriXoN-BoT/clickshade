import Cairo from 'cairo';
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {DEFAULTS, HIGHLIGHT_STYLES, CLICK_STYLES, KEY_MODIFIERS, parseIni, colorChannels, curve, breath,
    excluded, effectiveRate, nextDeadline, buttonEdges, keyCategory, keyDisplayWanted, keyChordParts,
    modifiersFromState, isModifierKeysym, modifierBit, lensOrigin, typedText, addKeyStroke} from './core.js';

const KEY_FADE_MS = 350;
const BACKSPACE = 0xff08;

const OBJECT_PATH = '/org/gnome/Shell/Extensions/ClickShade';
const DBUS_XML = `<node><interface name="org.gnome.Shell.Extensions.ClickShade1">
  <method name="SetActive"><arg name="active" type="b" direction="in"/></method>
  <method name="Reload"/>
  <method name="Restore"/>
  <method name="GetStatus">
    <arg name="running" type="b" direction="out"/>
    <arg name="supported" type="b" direction="out"/>
    <arg name="state" type="s" direction="out"/>
    <arg name="error" type="s" direction="out"/>
    <arg name="capabilities" type="s" direction="out"/>
  </method>
  <signal name="Click"><arg name="buttonMask" type="u"/></signal>
  <signal name="Key"><arg name="category" type="u"/></signal>
  <signal name="AllowedChanged"><arg name="allowed" type="b"/></signal>
</interface></node>`;

// How far a highlight style reaches beyond its nominal radius.
function highlightExtent(style, radius) {
    if (style === HIGHLIGHT_STYLES.CROSSHAIR) return radius * 1.75;
    if (style === HIGHLIGHT_STYLES.GLOW || style === HIGHLIGHT_STYLES.SPOTLIGHT) return radius * 1.4;
    return radius;
}

// An unreactive actor is painted by the compositor without acquiring pointer,
// keyboard, or window focus. DrawingArea caches its Cairo texture between
// queue_repaint calls, so a moving steady highlight only changes position.
class Circle {
    constructor() {
        this.actor = new St.DrawingArea({reactive: false, can_focus: false,
            track_hover: false, visible: false, name: 'ClickShade effect'});
        this.shape = null;
        this.actor.connect('repaint', area => this._paint(area));
        Main.layoutManager.addChrome(this.actor, {affectsInputRegion: false, trackFullscreen: false});
    }

    _paint(area) {
        const context = area.get_context();
        try {
            context.setOperator(Cairo.Operator.CLEAR);
            context.paint();
            if (!this.shape) return;
            context.setOperator(Cairo.Operator.OVER);
            const [width, height] = area.get_surface_size();
            const cx = width / 2, cy = height / 2;
            const {radius, thickness, color, opacity, style, click, progress} = this.shape;
            const [r, g, b] = colorChannels(color);
            const alpha = opacity / 100;
            const ring = (at, lineWidth, a) => {
                context.setSourceRGBA(r, g, b, a);
                context.setLineWidth(Math.min(lineWidth, at * 2));
                context.arc(cx, cy, Math.max(0.5, at - lineWidth / 2), 0, 2 * Math.PI);
                context.stroke();
            };
            const disc = (at, a) => {
                context.setSourceRGBA(r, g, b, a);
                context.arc(cx, cy, Math.max(0.5, at), 0, 2 * Math.PI);
                context.fill();
            };
            if (click) {
                switch (style) {
                case CLICK_STYLES.PULSE: disc(radius, alpha); break;
                case CLICK_STYLES.RIPPLE: ring(radius, thickness, alpha); ring(radius * 0.62, thickness, alpha * 0.55); break;
                case CLICK_STYLES.BURST: {
                    // Eight sparks fly outward and shrink; nothing is drawn at the center.
                    const spark = Math.max(1.5, thickness * 0.9 * (1 - progress * 0.5));
                    context.setSourceRGBA(r, g, b, alpha);
                    for (let point = 0; point < 8; ++point) {
                        const angle = point * Math.PI / 4 + Math.PI / 8;
                        context.arc(cx + Math.cos(angle) * radius, cy + Math.sin(angle) * radius, spark, 0, 2 * Math.PI);
                        context.fill();
                    }
                    break;
                }
                case CLICK_STYLES.NEON: ring(radius, thickness * 3.2, alpha * 0.28); ring(radius, thickness, alpha); break;
                case CLICK_STYLES.TARGET: {
                    // A scope closing in on the click: a ring with four inward ticks.
                    ring(radius, thickness, alpha);
                    context.setSourceRGBA(r, g, b, alpha);
                    context.setLineWidth(thickness);
                    for (let tick = 0; tick < 4; ++tick) {
                        const angle = tick * Math.PI / 2;
                        context.moveTo(cx + Math.cos(angle) * radius * 0.55, cy + Math.sin(angle) * radius * 0.55);
                        context.lineTo(cx + Math.cos(angle) * (radius - thickness / 2), cy + Math.sin(angle) * (radius - thickness / 2));
                    }
                    context.stroke();
                    break;
                }
                default: ring(radius, thickness, alpha); break;
                }
                return;
            }
            switch (style) {
            case HIGHLIGHT_STYLES.RING: ring(radius, thickness, alpha); break;
            case HIGHLIGHT_STYLES.GLOW:
            case HIGHLIGHT_STYLES.SPOTLIGHT: {
                // Shell draws no monitor-wide shade; the spotlight becomes a glow here.
                const extent = radius * 1.4;
                const glow = new Cairo.RadialGradient(cx, cy, 0, cx, cy, extent);
                glow.addColorStopRGBA(0, r, g, b, alpha * 0.9);
                glow.addColorStopRGBA(0.55, r, g, b, alpha * 0.45);
                glow.addColorStopRGBA(1, r, g, b, 0);
                context.setSource(glow);
                context.arc(cx, cy, extent, 0, 2 * Math.PI);
                context.fill();
                break;
            }
            case HIGHLIGHT_STYLES.CROSSHAIR:
                context.setSourceRGBA(r, g, b, alpha);
                context.setLineWidth(Math.max(1, thickness * 0.6));
                for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
                    context.moveTo(cx + dx * radius * 0.45, cy + dy * radius * 0.45);
                    context.lineTo(cx + dx * radius * 1.75, cy + dy * radius * 1.75);
                }
                context.stroke();
                ring(radius, thickness, alpha);
                break;
            default: disc(radius, alpha); break;
            }
        } finally { context.$dispose(); }
    }

    setShape(shape, maximumRadius = shape.radius) {
        this.shape = shape;
        const side = Math.ceil(maximumRadius * 2) + 8;
        if (this.actor.width !== side || this.actor.height !== side)
            this.actor.set_size(side, side);
        this.actor.queue_repaint();
    }

    move(x, y) {
        const left = Math.round(x - this.actor.width / 2);
        const top = Math.round(y - this.actor.height / 2);
        if (this.actor.x !== left || this.actor.y !== top) this.actor.set_position(left, top);
        this.actor.show();
    }

    hide() { this.actor.hide(); }
    destroy() { this.actor.destroy(); }
}

export default class ClickShadeExtension extends Extension {
    enable() {
        this._alive = true;
        this._active = true;
        this._config = {...DEFAULTS};
        this._lastError = '';
        this._lastAllowed = null;
        this._signals = [];
        this._timer = 0;
        this._reloadTimer = 0;
        this._idleHideTimer = 0;
        this._deadline = 0;
        this._loadSerial = (this._loadSerial ?? 0) + 1;
        this._cancel = new Gio.Cancellable();
        this._particles = [];
        this._highlight = null;
        this._haloSignature = '';
        this._down = 0;
        this._idle = false;
        this._idleWatch = 0;
        this._activeWatch = 0;
        this._rate = 60;
        this._view = null;
        this._identities = [];
        this._isExcluded = false;
        this._focus = null;
        this._focusSignal = 0;
        this._lastX = null;
        this._lastY = null;
        this._lastMove = GLib.get_monotonic_time();
        this._keyStack = null;
        this._keyRows = [];
        this._keyTimer = 0;
        this._chordModifiers = 0;
        this._chordUsed = false;
        this._lens = null;
        this._lensClone = null;
        this._supported = typeof global.get_pointer === 'function' && !!Main.layoutManager?.addChrome;
        try {
            // GNOME 49+ exposes the tracker through the backend. Older
            // supported releases expose CursorTracker.get_for_display.
            const backend = global.get_backend?.() ?? global.backend ??
                global.stage.get_context?.().get_backend?.();
            this._tracker = backend?.get_cursor_tracker?.() ??
                Meta.CursorTracker.get_for_display?.(global.display) ?? null;
            this._idleMonitor = backend?.get_core_idle_monitor?.() ??
                Meta.IdleMonitor.get_core?.() ?? null;
        } catch (_) { this._tracker = null; this._idleMonitor = null; }
        try {
            this._dbus = Gio.DBusExportedObject.wrapJSObject(DBUS_XML, this);
            this._dbus.export(Gio.DBus.session, OBJECT_PATH);
            this._connect(global.stage, 'captured-event', (_stage, event) => this._event(event));
            this._connect(global.display, 'notify::focus-window', () => this._focusChanged());
            this._connect(global.display, 'in-fullscreen-changed', () => this._wake(true));
            this._connect(Main.layoutManager, 'monitors-changed', () => {
                this._view = null; this._haloSignature = ''; this._wake(true);
            });
            this._connect(Main.sessionMode, 'updated', () => this._wake(true));
            this._connect(this._tracker, 'visibility-changed', () => this._wake(true), true);
            this._connect(St.ThemeContext.get_for_stage(global.stage), 'notify::scale-factor', () => {
                this._haloSignature = ''; this._wake(true);
            }, true);
            this._installIdleWatch();
            this._focusChanged();
            this._monitorConfig();
            this.Reload();
        } catch (error) {
            this._lastError = `Could not enable ClickShade: ${error.message}`;
            this._active = false;
            this._publishAllowed(false);
            this._hideAll();
        }
    }

    _connect(object, signal, callback, optional = false) {
        if (!object) return;
        try { this._signals.push([object, object.connect(signal, callback)]); }
        catch (error) { if (!optional) throw error; }
    }

    _monitorConfig() {
        const directory = GLib.build_filenamev([GLib.get_user_config_dir(), 'clickshade']);
        GLib.mkdir_with_parents(directory, 0o700);
        this._file = Gio.File.new_for_path(GLib.build_filenamev([directory, 'settings.ini']));
        this._monitor = Gio.File.new_for_path(directory).monitor_directory(Gio.FileMonitorFlags.NONE, this._cancel);
        this._connect(this._monitor, 'changed', (_monitor, file, other) => {
            if (!this._alive) return;
            if (file?.get_basename() !== 'settings.ini' && other?.get_basename() !== 'settings.ini') return;
            if (this._reloadTimer) return;
            this._reloadTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 60, () => {
                this._reloadTimer = 0;
                this.Reload();
                return GLib.SOURCE_REMOVE;
            });
        });
    }

    Reload() {
        if (!this._alive || !this._file) return;
        const serial = ++this._loadSerial;
        const cancellation = this._cancel;
        this._file.read_async(GLib.PRIORITY_DEFAULT, cancellation, (file, result) => {
            let stream;
            try { stream = file.read_finish(result); }
            catch (error) {
                if (this._alive && serial === this._loadSerial &&
                    !error.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND)) this._lastError = error.message;
                return;
            }
            if (!this._alive || serial !== this._loadSerial) {
                stream.close_async(GLib.PRIORITY_DEFAULT, null, null);
                return;
            }
            stream.read_bytes_async(131073, GLib.PRIORITY_DEFAULT, cancellation, (input, readResult) => {
                try {
                    const bytes = input.read_bytes_finish(readResult).get_data();
                    if (!this._alive || serial !== this._loadSerial) return;
                    if (bytes.length > 131072) throw new Error('settings.ini exceeds 128 KiB.');
                    const text = new TextDecoder('utf-8', {fatal: true}).decode(bytes);
                    const next = parseIni(text);
                    this._config = next;
                    this._lastError = '';
                    this._haloSignature = '';
                    this._view = null;
                    if (!next.ClickEffectsEnabled) this._clearParticles();
                    if (!next.MagnifierEnabled) this._hideLens();
                    if (!next.KeyDisplayEnabled) this._hideKeys();
                    this._focusChanged();
                } catch (error) {
                    if (this._alive && serial === this._loadSerial) this._lastError = error.message;
                } finally { input.close_async(GLib.PRIORITY_DEFAULT, null, null); }
            });
        });
    }

    _installIdleWatch() {
        if (!this._idleMonitor?.add_idle_watch) return;
        try {
            this._idleWatch = this._idleMonitor.add_idle_watch(1200, () => {
                if (!this._alive) return;
                this._idle = true;
                if (!this._particles.length && !this._breathing()) this._cancelTimer();
                this._scheduleIdleHide();
                if (!this._activeWatch)
                    this._activeWatch = this._idleMonitor.add_user_active_watch(() => {
                        this._activeWatch = 0;
                        if (!this._alive) return;
                        this._idle = false;
                        this._cancelIdleHide();
                        this._lastMove = GLib.get_monotonic_time();
                        this._wake();
                    });
            });
        } catch (_) { this._idleMonitor = null; }
    }

    _breathing() {
        return this._config.HighlightEnabled && this._config.HighlightBreathing &&
            this._config.HighlightStyle !== HIGHLIGHT_STYLES.SPOTLIGHT;
    }

    // Sampling stops when the session is idle, so the hide deadline gets its
    // own one-shot timer instead of waiting for the next sample.
    _scheduleIdleHide() {
        const limit = this._config.HighlightIdleHideMs;
        if (!limit || this._idleHideTimer || !this._highlight) return;
        const remaining = Math.max(1, Math.ceil((this._lastMove + limit * 1000 - GLib.get_monotonic_time()) / 1000));
        this._idleHideTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, remaining, () => {
            this._idleHideTimer = 0;
            if (this._alive && this._idle) this._highlight?.hide();
            return GLib.SOURCE_REMOVE;
        });
    }

    _cancelIdleHide() {
        if (this._idleHideTimer) GLib.Source.remove(this._idleHideTimer);
        this._idleHideTimer = 0;
    }

    _focusChanged() {
        if (this._focus && this._focusSignal) this._focus.disconnect(this._focusSignal);
        this._focusSignal = 0;
        this._focus = global.display.focus_window ?? global.display.get_focus_window?.() ?? null;
        const window = this._focus;
        this._identities = [];
        if (window) {
            this._identities = [window.get_wm_class?.(), window.get_gtk_application_id?.(),
                window.get_sandboxed_app_id?.(), Shell.WindowTracker.get_default().get_window_app(window)?.get_id()];
            try {
                const pid = window.get_pid();
                if (pid > 0) this._identities.push(GLib.path_get_basename(GLib.file_read_link(`/proc/${pid}/exe`)));
            } catch (_) { /* Sandboxed/remote windows may not expose a local executable. */ }
            try { this._focusSignal = window.connect('notify::fullscreen', () => this._wake(true)); }
            catch (_) { /* The display-level fullscreen signal remains connected. */ }
        }
        this._isExcluded = excluded(this._config.ExcludedProcesses, this._identities);
        this._isAllowed = excluded(this._config.AllowedProcesses, this._identities);
        this._wake(true);
    }

    // forKeys: the key display ignores a hidden pointer, because editors hide
    // it while you type, which is exactly when the keys should show.
    _pauseReason(forKeys = false) {
        if (!this._supported) return 'Required GNOME pointer/actor APIs are unavailable';
        if (!this._active || !this._config.Enabled) return 'Paused';
        if (Main.sessionMode.isLocked || Main.sessionMode.isGreeter) return 'Paused on the lock/login screen';
        if (this._isExcluded) return 'Paused for an excluded application';
        if (!this._isAllowed && this._config.PauseFullscreen && this._focus?.is_fullscreen?.()) return 'Paused for a fullscreen application';
        if (!forKeys && this._tracker?.get_pointer_visible && !this._tracker.get_pointer_visible()) return 'Paused while the pointer is hidden';
        return '';
    }

    _hasFeatures() {
        return this._config.HighlightEnabled || this._config.ClickEffectsEnabled ||
            this._config.ClickSoundsEnabled || this._config.KeyboardSoundsEnabled ||
            this._config.MagnifierEnabled || this._config.KeyDisplayEnabled;
    }

    _buttons(modifiers) {
        return ((modifiers & Clutter.ModifierType.BUTTON1_MASK) ? 1 : 0) |
               ((modifiers & Clutter.ModifierType.BUTTON3_MASK) ? 2 : 0) |
               ((modifiers & Clutter.ModifierType.BUTTON2_MASK) ? 4 : 0);
    }

    _event(event) {
        try {
            const type = event.type();
            if (type === Clutter.EventType.BUTTON_PRESS || type === Clutter.EventType.BUTTON_RELEASE) {
                const button = event.get_button();
                const bit = button === 1 ? 1 : button === 3 ? 2 : button === 2 ? 4 : 0;
                if (bit && type === Clutter.EventType.BUTTON_PRESS) {
                    if (!(this._down & bit) && !this._pauseReason()) {
                        const [x, y] = event.get_coords();
                        this._click(bit, x, y);
                    }
                    this._down |= bit;
                } else this._down &= ~bit;
                this._wake();
            } else if (type === Clutter.EventType.MOTION && this._idle) {
                this._idle = false;
                this._cancelIdleHide();
                this._lastMove = GLib.get_monotonic_time();
                this._wake();
            } else if (type === Clutter.EventType.KEY_PRESS || (Clutter.EventType.KEY_RELEASE !== undefined && type === Clutter.EventType.KEY_RELEASE)) {
                const pressed = type === Clutter.EventType.KEY_PRESS;
                const repeated = Boolean((event.get_flags?.() ?? 0) & (Clutter.EventFlags?.REPEATED ?? 0));
                // Keyboard sounds: only a coarse category leaves the Shell (key,
                // space, enter or backspace). Repeats are skipped and nothing is stored.
                if (pressed && this._config.KeyboardSoundsEnabled && !repeated && !this._pauseReason())
                    this._dbus?.emit_signal('Key', new GLib.Variant('(u)', [keyCategory(event.get_key_symbol())]));
                if (this._config.KeyDisplayEnabled) this._keyEvent(event, pressed, repeated);
            }
        } catch (error) { this._fail(error); }
        // Observation only: no pointer/keyboard grab and never stop propagation.
        return Clutter.EVENT_PROPAGATE;
    }

    _keyEvent(event, pressed, repeated) {
        const keysym = event.get_key_symbol();
        const modifiers = modifiersFromState(event.get_state?.() ?? 0);
        const modifier = isModifierKeysym(keysym);
        if (!pressed) {
            // A modifier tapped on its own is itself the label (Super opens the
            // overview); it shows once every modifier is up again. The event
            // state still carries the modifier being released.
            if (modifier && !(modifiers & ~modifierBit(keysym))) {
                if (!this._chordUsed && this._chordModifiers) {
                    if (this._chordModifiers & KEY_MODIFIERS.ALTGR) this._showKey(0xfe03, KEY_MODIFIERS.ALTGR);
                    else this._showKey(keysym, this._chordModifiers & ~modifierBit(keysym));
                }
                this._chordUsed = false;
                this._chordModifiers = 0;
            }
            return;
        }
        if (repeated) return;
        if (modifier) { this._chordModifiers |= modifierBit(keysym); return; }
        if (modifiers) this._chordUsed = true;
        this._showKey(keysym, modifiers, event.get_key_unicode?.() ?? 0);
    }

    _showKey(keysym, modifiers, unicode = 0) {
        if (!this._config.KeyDisplayEnabled || this._pauseReason(true)) return;
        if (!keyDisplayWanted(this._config.KeyDisplayMode, keysym, modifiers)) return;
        const now = GLib.get_monotonic_time();
        const shown = this._keyStack?.visible === true;
        const plain = !(modifiers & (KEY_MODIFIERS.CONTROL | KEY_MODIFIERS.ALT | KEY_MODIFIERS.SUPER | KEY_MODIFIERS.MOUSE));
        this._keyRows = addKeyStroke(shown ? this._keyRows : [], {
            caps: keyChordParts(keysym, modifiers), typed: typedText(unicode, modifiers),
            backspace: plain && keysym === BACKSPACE,
        }, now, this._config.KeyDisplayDurationMs * 1000);
        this._paintKeys();
    }

    _keyMonitor() {
        const layout = Main.layoutManager;
        const monitor = layout.focusMonitor ?? layout.primaryMonitor ?? layout.currentMonitor ?? {x: 0, y: 0, width: 1920, height: 1080, index: 0};
        const work = (monitor.index !== undefined && layout.getWorkAreaForMonitor?.(monitor.index)) || monitor;
        return work;
    }

    // The key display in the style of the Mac keystroke visualizers: each
    // shortcut is a row of keycaps on a rounded translucent panel with a soft
    // shadow; typed characters join into one row. Rows stack towards the
    // screen edge and fade out one by one.
    _paintKeys() {
        const now = GLib.get_monotonic_time();
        const hold = this._config.KeyDisplayDurationMs * 1000;
        // Rows past their hold were fading; a repaint lets them go.
        this._keyRows = this._keyRows.filter(row => now - row.updated < hold);
        if (!this._keyRows.length) { this._hideKeys(); return; }
        const scale = this._density() * this._config.KeyDisplaySize / 100;
        const px = (value, minimum = 1) => Math.max(minimum, Math.round(value * scale));
        if (!this._keyStack) {
            this._keyStack = new St.Widget({reactive: false, can_focus: false, track_hover: false, visible: false, name: 'ClickShade keys',
                layout_manager: new Clutter.BoxLayout({orientation: Clutter.Orientation.VERTICAL})});
            Main.layoutManager.addChrome(this._keyStack, {affectsInputRegion: false, trackFullscreen: false});
        }
        this._keyStack.layout_manager.spacing = px(8, 2);
        const [r, g, b] = colorChannels(this._config.KeyDisplayColor).map(channel => Math.round(channel * 255));
        const dark = r * 299 + g * 587 + b * 114 < 140000;
        const ink = dark ? '246, 248, 252' : '24, 26, 32';
        const panel = `background-color: rgba(${r}, ${g}, ${b}, ${(this._config.KeyDisplayOpacity / 100).toFixed(2)}); ` +
            `border: 1px solid ${dark ? 'rgba(255, 255, 255, 0.14)' : 'rgba(0, 0, 0, 0.10)'}; border-radius: ${px(16, 4)}px; ` +
            `box-shadow: 0 ${px(3)}px ${px(14, 3)}px rgba(0, 0, 0, 0.30); padding: ${px(11, 3)}px; spacing: ${px(7, 2)}px; ` +
            `color: rgb(${ink}); font-weight: bold;`;
        const cap = `background-color: ${dark ? 'rgba(255, 255, 255, 0.13)' : 'rgba(255, 255, 255, 0.85)'}; ` +
            `border-bottom: ${px(2)}px solid ${dark ? 'rgba(0, 0, 0, 0.30)' : 'rgba(0, 0, 0, 0.16)'}; border-radius: ${px(8, 3)}px; ` +
            `padding: ${px(6, 2)}px ${px(13, 4)}px; min-width: ${px(18, 6)}px; font-size: ${px(22, 8)}px; text-align: center;`;
        const typedStyle = `font-size: ${px(28, 10)}px; padding: 0 ${px(6, 2)}px;`;
        const countStyle = `font-size: ${px(19, 7)}px; color: rgba(${ink}, 0.62); padding-left: ${px(4)}px;`;
        const align = [Clutter.ActorAlign.CENTER, Clutter.ActorAlign.CENTER, Clutter.ActorAlign.START, Clutter.ActorAlign.END][this._config.KeyDisplayPosition] ?? Clutter.ActorAlign.CENTER;
        const label = (text, style) => {
            const actor = new St.Label({text, reactive: false, can_focus: false, track_hover: false, y_align: Clutter.ActorAlign.CENTER});
            actor.set_style(style);
            return actor;
        };
        this._keyStack.remove_all_transitions?.();
        this._keyStack.destroy_all_children();
        // The newest row sits nearest the screen edge.
        const ordered = this._config.KeyDisplayPosition === 1 ? [...this._keyRows].reverse() : this._keyRows;
        for (const row of ordered) {
            const actor = new St.BoxLayout({reactive: false, can_focus: false, track_hover: false, x_align: align, x_expand: false});
            actor.set_style(panel);
            if (row.caps.length && !row.typed) for (const text of row.caps) actor.add_child(label(text, cap));
            else actor.add_child(label(row.typed, typedStyle));
            if (row.repeat > 1) actor.add_child(label(`×${row.repeat}`, countStyle));
            row.actor = actor;
            row.fading = false; // Every row left after the filter is within its hold time.
            this._keyStack.add_child(actor);
        }
        this._keyStack.opacity = 255;
        this._keyStack.show();
        this._placeKeys();
        this._scheduleKeyFade(now);
    }

    // Anchors the stack at its screen edge, so rows leaving keep the rest in place.
    _placeKeys() {
        const [, , width, height] = this._keyStack.get_preferred_size();
        const work = this._keyMonitor();
        const margin = Math.round(40 * this._density());
        let x = Math.round(work.x + (work.width - width) / 2), y = Math.round(work.y + work.height - margin - height);
        if (this._config.KeyDisplayPosition === 1) y = work.y + margin;
        else if (this._config.KeyDisplayPosition === 2) x = work.x + margin;
        else if (this._config.KeyDisplayPosition === 3) x = Math.round(work.x + work.width - margin - width);
        this._keyStack.set_position(x, y);
    }

    // Wakes when the oldest row reaches its hold time.
    _scheduleKeyFade(now) {
        if (this._keyTimer) GLib.Source.remove(this._keyTimer);
        this._keyTimer = 0;
        const hold = this._config.KeyDisplayDurationMs * 1000;
        const next = Math.min(...this._keyRows.filter(row => !row.fading).map(row => row.updated + hold));
        if (!Number.isFinite(next)) return;
        this._keyTimer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, Math.max(1, Math.ceil((next - now) / 1000)), () => {
            this._keyTimer = 0;
            this._fadeKeys();
            return GLib.SOURCE_REMOVE;
        });
    }

    _fadeKeys() {
        if (!this._keyStack?.visible) return;
        const now = GLib.get_monotonic_time();
        const hold = this._config.KeyDisplayDurationMs * 1000;
        for (const row of this._keyRows) {
            if (row.fading || now - row.updated < hold) continue;
            row.fading = true;
            const done = () => {
                this._keyRows = this._keyRows.filter(other => other !== row);
                if (!this._keyRows.length) this._hideKeys();
                else { row.actor?.destroy(); row.actor = null; this._placeKeys(); }
            };
            if (typeof row.actor?.ease === 'function') row.actor.ease({opacity: 0, duration: KEY_FADE_MS, onComplete: done});
            else done();
        }
        if (this._keyRows.length) this._scheduleKeyFade(now);
    }

    _hideKeys() {
        if (this._keyTimer) GLib.Source.remove(this._keyTimer);
        this._keyTimer = 0;
        this._keyStack?.remove_all_transitions?.();
        this._keyStack?.destroy_all_children();
        this._keyStack?.hide();
        this._keyRows = [];
    }

    // A square lens beside the pointer: a scaled clone of the window layer,
    // which never contains the Shell chrome (or ClickShade's own actors).
    _updateLens(x, y) {
        if (!this._config.MagnifierEnabled || !global.window_group) { this._hideLens(); return; }
        const scale = this._density();
        const size = Math.round(this._config.MagnifierSize * scale);
        const zoom = this._config.MagnifierZoom / 100;
        if (!this._lens) {
            this._lens = new St.Widget({reactive: false, can_focus: false, track_hover: false, clip_to_allocation: true,
                visible: false, name: 'ClickShade magnifier'});
            this._lensClone = new Clutter.Clone({source: global.window_group});
            this._lens.add_child(this._lensClone);
            Main.layoutManager.addChrome(this._lens, {affectsInputRegion: false, trackFullscreen: false});
        }
        const border = Math.max(2, Math.round(3 * scale));
        const [r, g, b] = colorChannels(this._config.MagnifierBorderColor).map(channel => Math.round(channel * 255));
        const frame = this._config.MagnifierFrame ? `border: ${border}px solid rgb(${r}, ${g}, ${b});` : 'border: none;';
        this._lens.set_style(`${frame} border-radius: ${Math.round(size / 10)}px;`);
        this._lens.set_size(size, size);
        const monitor = Main.layoutManager.currentMonitor ?? Main.layoutManager.primaryMonitor ?? {x: 0, y: 0, width: 1920, height: 1080};
        const origin = lensOrigin(x, y, size, monitor);
        this._lens.set_position(origin.x, origin.y);
        this._lensClone.set_pivot_point(0, 0);
        this._lensClone.set_scale(zoom, zoom);
        this._lensClone.set_position(Math.round(size / 2 - x * zoom), Math.round(size / 2 - y * zoom));
        this._lens.show();
    }

    _hideLens() { this._lens?.hide(); }

    _density() {
        return Math.max(1, Math.min(4, St.ThemeContext.get_for_stage(global.stage).scale_factor || 1));
    }

    _updateRate(x, y) {
        const view = global.stage.get_view_at?.(Math.floor(x), Math.floor(y)) ?? null;
        if (view !== this._view || !this._view) {
            this._view = view;
            this._rate = effectiveRate(this._config.VisualRefreshRate, view?.get_refresh_rate?.());
        }
    }

    _sample(now) {
        const [x, y, modifiers] = global.get_pointer();
        this._updateRate(x, y);
        if (x !== this._lastX || y !== this._lastY) { this._lastX = x; this._lastY = y; this._lastMove = now; }
        const current = this._buttons(modifiers);
        const edges = buttonEdges(this._down, current);
        this._down = current;
        if (edges & 1) this._click(1, x, y);
        if (edges & 2) this._click(2, x, y);
        if (edges & 4) this._click(4, x, y);
        const limit = this._config.HighlightIdleHideMs;
        const resting = limit > 0 && now - this._lastMove >= limit * 1000;
        if (this._config.HighlightEnabled && !resting) {
            if (!this._highlight) this._highlight = new Circle();
            const breathing = this._breathing();
            if (!this._haloSignature || breathing) {
                const scale = this._density();
                const style = this._config.HighlightStyle;
                const swell = breathing ? breath(now) : {radius: 1, opacity: 1};
                const nominal = this._config.HighlightRadius * scale;
                const shape = {style, radius: nominal * swell.radius,
                    thickness: this._config.HighlightThickness * scale, color: this._config.HighlightColor,
                    opacity: Math.min(100, this._config.HighlightOpacity * swell.opacity)};
                this._highlight.setShape(shape, highlightExtent(style, nominal));
                this._haloSignature = 'ready';
            }
            this._highlight.move(x, y);
        } else this._highlight?.hide();
        this._updateLens(x, y);
    }

    _click(button, x, y) {
        if (!this._alive || this._pauseReason()) return;
        const enabled = button === 1 ? this._config.LeftClickEnabled : button === 2 ? this._config.RightClickEnabled : this._config.MiddleClickEnabled;
        if (this._config.ClickSoundsEnabled)
            this._dbus?.emit_signal('Click', new GLib.Variant('(u)', [button]));
        if (this._config.KeyDisplayEnabled && this._config.KeyDisplayMouse) {
            const modifiers = modifiersFromState(global.get_pointer()[2]);
            if (modifiers) this._chordUsed = true;
            this._showKey(button, modifiers | KEY_MODIFIERS.MOUSE);
        }
        if (!enabled || !this._config.ClickEffectsEnabled) return;
        if (this._particles.length === 8) this._particles.shift().circle.destroy();
        const scale = this._density();
        const particle = {circle: new Circle(), x, y, started: GLib.get_monotonic_time(),
            duration: this._config.ClickDurationMs * 1000, easing: this._config.ClickEasing,
            maximum: this._config.ClickRadius * scale, thickness: this._config.ClickThickness * scale,
            style: this._config.ClickStyle, opacity: this._config.ClickOpacity,
            color: button === 1 ? this._config.LeftClickColor : button === 2 ? this._config.RightClickColor : this._config.MiddleClickColor};
        this._particles.push(particle);
        this._drawParticle(particle, particle.started);
    }

    _drawParticle(particle, now) {
        const t = (now - particle.started) / particle.duration;
        if (t >= 1) return false;
        const [expansion, fade] = curve(t, particle.easing);
        // Target contracts onto the click; every other style expands from it.
        const radius = particle.style === CLICK_STYLES.TARGET
            ? Math.max(2, particle.maximum * (1 - 0.5 * expansion)) : particle.maximum * (0.28 + 0.72 * expansion);
        particle.circle.setShape({click: true, style: particle.style, radius,
            thickness: particle.thickness, color: particle.color, opacity: particle.opacity, progress: t},
            particle.maximum + particle.thickness * 1.6);
        particle.circle.actor.opacity = Math.round(fade * 255);
        particle.circle.move(particle.x, particle.y);
        return true;
    }

    _tick() {
        if (!this._alive) return;
        try {
            const paused = Boolean(this._pauseReason());
            this._publishAllowed(!paused);
            if (paused || !this._hasFeatures()) { this._hideAll(); return; }
            const now = GLib.get_monotonic_time();
            this._sample(now);
            this._particles = this._particles.filter(particle => {
                if (this._drawParticle(particle, now)) return true;
                particle.circle.destroy();
                return false;
            });
            this._deadline = nextDeadline(this._deadline, now, 1000000 / this._rate);
            this._schedule();
        } catch (error) { this._fail(error); }
    }

    _schedule() {
        if (this._timer || !this._alive || (!this._particles.length && this._idle && !this._breathing()) || this._pauseReason()) return;
        const now = GLib.get_monotonic_time();
        if (!this._deadline || this._deadline < now) this._deadline = now + 1000000 / this._rate;
        const milliseconds = Math.max(1, Math.ceil((this._deadline - now) / 1000));
        this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, milliseconds, () => {
            this._timer = 0;
            this._tick();
            return GLib.SOURCE_REMOVE;
        });
    }

    _wake(resetButtons = false) {
        if (!this._alive) return;
        const paused = Boolean(this._pauseReason());
        this._publishAllowed(!paused);
        if (this._supported && resetButtons) this._down = this._buttons(global.get_pointer()[2]);
        if (paused) { this._hideAll(); return; }
        this._tick();
    }

    _publishAllowed(allowed, force = false) {
        if (!this._alive || !this._dbus || (!force && this._lastAllowed === allowed)) return;
        this._lastAllowed = allowed;
        // The native audio worker must flush existing voices when a rule
        // pauses effects, as well as stop receiving new click notifications.
        try { this._dbus.emit_signal('AllowedChanged', new GLib.Variant('(b)', [allowed])); }
        catch (error) { this._lastError = `ClickShade audio rule notification failed: ${error.message}`; }
    }

    _cancelTimer() {
        if (this._timer) GLib.Source.remove(this._timer);
        this._timer = 0;
        this._deadline = 0;
    }

    _clearParticles() {
        for (const particle of this._particles) particle.circle.destroy();
        this._particles = [];
    }

    _hideAll() {
        this._cancelTimer();
        this._cancelIdleHide();
        this._highlight?.hide();
        // Typing hides the pointer in many editors: the lens and the key
        // display stay where they are; any other pause hides them.
        if (this._pauseReason(true)) { this._hideLens(); this._hideKeys(); }
        this._clearParticles();
    }

    _fail(error) {
        this._lastError = `Pointer effects paused: ${error.message}`;
        this._active = false;
        this._publishAllowed(false);
        this._hideAll();
    }

    SetActive(active) {
        this._active = Boolean(active);
        this._wake(true);
        // A newly connected GTK process needs the current rule state even
        // when the extension was already enabled before it subscribed.
        this._publishAllowed(!this._pauseReason(), true);
    }

    Restore() { this.SetActive(false); }

    GetStatus() {
        const pause = this._pauseReason();
        const state = pause || `GNOME effects active · ${Math.round(this._rate)} Hz sampling limit`;
        let capabilities = 'GNOME Shell highlight (filled, ring, glow, crosshair; the spotlight is drawn as a glow) with breathing and idle hide; ' +
            'left/right/middle click effects in ring, pulse, ripple, burst, neon and target styles; a square magnifier lens beside the pointer cloned from the window layer; ' +
            'an on-screen key display whose labels exist only while shown; fullscreen and application exclusions; ' +
            'click and keyboard sounds play in the running GTK app from coarse notifications (button mask; key, space, enter or backspace). ' +
            'Cursor replacement, custom cursor files, reverse/hold cursor modes, shake enlargement, trails, global shortcuts and ' +
            'confined-pointer detection are unavailable. Global clicks use sampled button state plus available Shell events; ' +
            'presses shorter than one sample can be missed. Compositor integration is experimental and not runtime-validated here.';
        if (!this._tracker?.get_pointer_visible) capabilities += ' Pointer visibility reporting is unavailable on this Shell build.';
        if (!this._idleMonitor) capabilities += ' Idle-watch API unavailable; sampling continues while enabled.';
        return [this._active && this._config.Enabled, this._supported, state, this._lastError, capabilities];
    }

    disable() {
        this._publishAllowed(false);
        this._alive = false;
        this._active = false;
        this._cancel?.cancel();
        this._hideAll();
        if (this._reloadTimer) GLib.Source.remove(this._reloadTimer);
        this._reloadTimer = 0;
        if (this._focus && this._focusSignal) this._focus.disconnect(this._focusSignal);
        this._focusSignal = 0;
        for (const [object, signal] of this._signals ?? []) object.disconnect(signal);
        this._signals = [];
        if (this._idleMonitor) {
            if (this._idleWatch) this._idleMonitor.remove_watch(this._idleWatch);
            if (this._activeWatch) this._idleMonitor.remove_watch(this._activeWatch);
        }
        this._idleWatch = this._activeWatch = 0;
        this._monitor?.cancel();
        this._monitor = null;
        this._highlight?.destroy();
        this._highlight = null;
        if (this._keyTimer) GLib.Source.remove(this._keyTimer);
        this._keyTimer = 0;
        this._keyStack?.destroy_all_children();
        this._keyStack?.destroy();
        this._keyStack = null;
        this._keyRows = [];
        this._lens?.destroy();
        this._lens = this._lensClone = null;
        this._dbus?.unexport();
        this._dbus = null;
        this._tracker = this._idleMonitor = this._focus = this._file = this._cancel = null;
        // The real pointer was never hidden or replaced, so no theme/state
        // restoration is needed and unrelated applications retain their cursor.
    }
}
