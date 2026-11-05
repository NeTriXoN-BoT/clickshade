// Headless regression tests. The Shell APIs are mocked; this does not claim
// native GNOME/Mutter runtime validation. Run: node tests.mjs
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';

const coreText = await readFile(new URL('./core.js', import.meta.url), 'utf8');
const core = await import(`data:text/javascript;base64,${Buffer.from(coreText).toString('base64')}`);
let checks = 0;
function check(condition, message) {
    assert.ok(condition, message);
    ++checks;
    console.log(`PASS ${message}`);
}

check(core.parseIni('[Other]\nEnabled=0\n[ClickShade]\nEnabled=1\n').Enabled,
    'INI parsing applies only the shared ClickShade section');
const config = core.parseIni('\ufeff[ClickShade]\r\nHighlightRadius=999\r\nClickDurationMs=-5\n' +
    'VisualRefreshRate=240\nClickEasing=2\nHighlightColor=4200191\n__proto__=x\n');
check(config.HighlightRadius === 100 && config.ClickDurationMs === 100,
    'shared size and duration limits reject unbounded rendering requests');
check(config.VisualRefreshRate === 240 && config.ClickEasing === 2,
    'refresh limit and easing survive shared INI parsing');
check(core.parseIni('[ClickShade]\nHighlightRadius=24oops\n').HighlightRadius === 24,
    'malformed numeric settings retain their safe default');
check(core.normalize({VisualRefreshRate: 500}).VisualRefreshRate === 360 &&
    core.normalize({VisualRefreshRate: 1}).VisualRefreshRate === 30,
    'manual sampling rate stays within 30–360 fps');
check(core.effectiveRate(30, 144) === 30 && core.effectiveRate(47, 144) === 47 &&
    core.effectiveRate(0, 48) === 48, 'manual low frame limits and compositor-controlled low-refresh Auto are retained');
assert.throws(() => core.parseIni('a'.repeat(131073)));
check(true, 'oversized settings are rejected');
check(core.colorChannels(0x00332211).every((x, i) => x === [17, 34, 51][i] / 255),
    'color channel packing matches Windows COLORREF and the GTK app');
for (let easing = 0; easing < 3; ++easing) {
    check(JSON.stringify(core.curve(0, easing)) === '[0,1]' &&
        JSON.stringify(core.curve(1, easing)) === '[1,0]',
        `easing ${easing} has exact start/end expansion and opacity`);
    let previous = -1;
    for (let i = 0; i <= 100; ++i) {
        const [expansion, fade] = core.curve(i / 100, easing);
        assert.ok(expansion >= previous && expansion >= 0 && expansion <= 1 && fade >= 0 && fade <= 1);
        previous = expansion;
    }
}
check(core.curve(0.2, 1)[0] > core.curve(0.2, 0)[0] &&
    core.curve(0.2, 0)[0] > core.curve(0.2, 2)[0], 'the three easing curves remain visibly distinct');
check(core.excluded('other;ORG.GNOME.Terminal.desktop; demo', ['org.gnome.Terminal']) &&
    !core.excluded('term', ['terminal']), 'application exclusions are exact, case-insensitive identities');
check(core.parseIni('[ClickShade]\nAllowedProcesses=demo;editor\n').AllowedProcesses === 'demo;editor' &&
    core.normalize({AllowedProcesses: 'a'.repeat(9000)}).AllowedProcesses.length === 8192,
    'desktop-app exceptions share bounded settings parsing');
check(core.buttonEdges(0, 3) === 3 && core.buttonEdges(3, 3) === 0 &&
    core.buttonEdges(3, 0) === 0, 'button sampling triggers only newly pressed left/right buttons');
const legacy = core.parseIni('[ClickShade]\nHighlightFilled=0\nClickFilled=1\n');
check(legacy.HighlightStyle === core.HIGHLIGHT_STYLES.RING && legacy.ClickStyle === core.CLICK_STYLES.PULSE,
    'a preview-build settings file maps its shape switches onto the styles');
const styled = core.parseIni('[ClickShade]\nHighlightStyle=9\nClickStyle=-1\nHighlightFilled=1\nHighlightIdleHideMs=20\nSpotlightDim=5\n');
check(styled.HighlightStyle === 4 && styled.ClickStyle === 0 && styled.HighlightIdleHideMs === 500 && styled.SpotlightDim === 10,
    'explicit style numbers win over legacy switches and stay within their ranges');
check(core.parseIni('[ClickShade]\nClickStyle=5\n').ClickStyle === core.CLICK_STYLES.TARGET &&
    core.parseIni('[ClickShade]\nClickStyle=9\n').ClickStyle === core.CLICK_STYLES.TARGET,
    'the target click style parses and is the highest click style');
check(core.keyCategory(0x20) === core.KEY_CATEGORIES.SPACE && core.keyCategory(0xff0d) === core.KEY_CATEGORIES.ENTER &&
    core.keyCategory(0xff8d) === core.KEY_CATEGORIES.ENTER && core.keyCategory(0xff08) === core.KEY_CATEGORIES.BACKSPACE &&
    core.keyCategory(0x61) === core.KEY_CATEGORIES.GENERIC, 'keysyms collapse to the four coarse key categories');
let breathOk = true;
for (let t = 0; t < 3e6; t += 50000) {
    const swell = core.breath(t);
    breathOk = breathOk && swell.radius >= 0.84 && swell.radius <= 1 && swell.opacity >= 0.52 && swell.opacity <= 1;
}
check(breathOk, 'breathing stays within a bounded swell of radius and opacity');
check(core.nextDeadline(1000, 1100, 500) === 1500 &&
    core.nextDeadline(1000, 5000, 500) === 5500,
    'frame deadlines retain normal timer phase and skip a stalled compositor backlog');
check(core.effectiveRate(0, 144) === 144 && core.effectiveRate(0, NaN) === 60 &&
    core.effectiveRate(0, 500) === 360, 'automatic rate uses a valid bounded monitor refresh or 60 Hz fallback');
check(core.BREATH_INTERVAL_US >= 30000 && core.BREATH_INTERVAL_US <= 40000,
    'breathing has its own pace of about 30 repaints a second');
{
    const {typingHold, TYPING_START_US: start, TYPING_HOLD_US: hold} = core;
    check(start === 1500000 && hold === 3000000, 'the typing hold starts within 1.5 s of a key and lasts 3 s past the last key');
    check(typingHold(false, false, true, 1000, 1000 + start) && !typingHold(false, false, true, 1000, 1000 + start + 1) &&
        !typingHold(false, false, true, 0, 5000),
        'a pointer that hides soon after a key press starts a typing hold; a later or keyless hide does not');
    check(typingHold(true, true, true, 1000, 1000 + hold - 1) && !typingHold(true, true, true, 1000, 1000 + hold) &&
        !typingHold(false, true, true, 1000, 1100),
        'the hold ends 3 s after the last key, and a pointer hidden before the key starts none');
    check(!typingHold(true, true, false, 1000, 1100), 'showing the pointer ends the typing hold');
}
check(core.parseCommandLine('/usr/bin/demo\0--flag\0\0').join('|') === '/usr/bin/demo|--flag' &&
    core.parseCommandLine('').length === 0, 'process command lines split on NUL bytes');
{
    const game = options => core.isGameWindow({home: '/home/test', ...options});
    check(game({wmClass: 'steam_app_620'}) && game({wmClass: 'gamescope'}), 'Steam game classes and gamescope count as games');
    check(game({exe: '/home/test/.local/share/Steam/steamapps/common/Portal 2/portal2_linux'}) &&
        game({exe: '/mnt/games/SteamLibrary/SteamApps/common/Demo/demo.x86_64'}),
        'programs in any Steam library folder count as games');
    check(game({exe: '/opt/wine-staging/bin/wine64-preloader'}) && game({exe: '/usr/bin/wine-preloader'}) &&
        game({argv: ['C:\\Program Files\\Demo\\demo.exe', '-windowed']}) && game({exe: '/usr/bin/gamescope'}),
        'Wine/Proton processes and gamescope count as games');
    check(game({exe: '/home/test/Games/Heroic/Demo/demo'}) && game({exe: '/srv/Games/Heroic/Demo/demo'}) &&
        game({exe: '/home/test/.local/share/lutris/runners/retroarch/retroarch'}) &&
        game({exe: '/home/test/.config/itch/apps/demo/demo.x86_64'}) && game({argv: ['/home/test/Games/demo/run.x86_64']}),
        'programs in Heroic, Lutris and itch game folders count as games');
    check(!game({wmClass: 'firefox', exe: '/usr/lib/firefox/firefox', argv: ['/usr/lib/firefox/firefox', 'https://example.com/setup.exe']}) &&
        !game({wmClass: 'org.gnome.Totem', exe: '/usr/bin/totem'}) &&
        !game({wmClass: 'libreoffice-impress', exe: '/usr/lib/libreoffice/program/soffice.bin'}) &&
        !game({wmClass: 'steam', exe: '/home/test/.local/share/Steam/ubuntu12_32/steam'}) &&
        !game({exe: '/opt/Heroic/heroic'}) && !game({}),
        'browsers, video players, presentations and the game launchers themselves are no games');
}

let nextSignal = 1;
class Signals {
    constructor() { this.signals = new Map(); }
    connect(name, callback) { const id = nextSignal++; this.signals.set(id, {name, callback}); return id; }
    disconnect(id) { assert.ok(this.signals.delete(id), 'disconnect owns a connected signal'); }
    emit(name, ...args) {
        let answer;
        for (const value of [...this.signals.values()])
            if (value.name === name) answer = value.callback(this, ...args);
        return answer;
    }
}
let now = 1000, timerId = 1, configText = '[ClickShade]\nVisualRefreshRate=240\nClickSoundsEnabled=1\n';
const timers = new Map(), actors = new Set(), exported = [], emitted = [];
const pointer = {x: 100, y: 120, modifiers: 0};
const theme = new Signals(); theme.scale_factor = 1;
let callbacks = 0;
function advance(milliseconds) {
    const until = now + milliseconds * 1000;
    for (;;) {
        let selected = null;
        for (const item of timers)
            if (item[1].at <= until && (!selected || item[1].at < selected[1].at)) selected = item;
        if (!selected) break;
        timers.delete(selected[0]);
        now = selected[1].at;
        selected[1].callback();
        if (++callbacks > 30000) throw new Error('unbounded scheduling');
    }
    now = until;
}
// Processes behind the mock windows: /proc/<pid>/exe and /proc/<pid>/cmdline.
const home = '/home/test';
const processes = new Map();
let procReads = 0;
function procEntry(path) {
    ++procReads;
    const [, pid, kind] = /^\/proc\/(\d+)\/(exe|cmdline)$/.exec(path) ?? [];
    const value = processes.get(Number(pid))?.[kind];
    if (value === undefined) throw new Error(`Cannot read ${path}`);
    return value;
}
const GLib = {
    PRIORITY_DEFAULT: 0, SOURCE_REMOVE: false,
    get_monotonic_time: () => now,
    timeout_add: (_priority, interval, callback) => {
        const id = timerId++; timers.set(id, {at: now + interval * 1000, callback}); return id;
    },
    Source: {remove: id => timers.delete(id)},
    Variant: class { constructor(_signature, values) { this.values = values; } },
    build_filenamev: parts => parts.join('/'), get_user_config_dir: () => '/test/config', get_home_dir: () => home,
    mkdir_with_parents: () => 0, path_get_basename: path => path.split('/').at(-1),
    file_read_link: path => procEntry(path),
    file_get_contents: path => [true, new TextEncoder().encode(procEntry(path))],
};
class Area extends Signals {
    constructor(properties) {
        super(); Object.assign(this, properties); this.width = this.height = this.x = this.y = 0;
        this.opacity = 255; this.paintCount = 0; actors.add(this);
    }
    set_size(width, height) { this.width = width; this.height = height; }
    set_position(x, y) { this.x = x; this.y = y; }
    queue_repaint() { ++this.paintCount; this.emit('repaint'); }
    get_surface_size() { return [this.width, this.height]; }
    get_context() {
        return {setOperator() {}, paint() {}, setSourceRGBA() {}, setSource() {}, arc() {}, fill() {},
            moveTo() {}, lineTo() {}, setLineWidth() {}, stroke() {}, $dispose() {}};
    }
    show() { this.visible = true; }
    hide() { this.visible = false; }
    destroy() { this.signals.clear(); actors.delete(this); this.destroyed = true; }
}
class Label extends Area {
    constructor(properties) { super(properties); this.text = properties.text; }
    set_style(style) { this.style = style; }
}
class Box extends Area {
    constructor(properties) { super(properties); this.children = []; }
    add_child(child) { this.children.push(child); }
    set_style(style) { this.style = style; }
    destroy_all_children() { for (const child of this.children) child.destroy(); this.children = []; }
    get_preferred_size() { return [0, 0, 90 * this.children.length, 40]; }
    remove_all_transitions() {}
    destroy() { this.destroy_all_children(); super.destroy(); }
}
class Widget extends Area {
    constructor(properties) { super(properties); this.children = []; }
    add_child(child) { this.child = child; this.children.push(child); }
    set_style(style) { this.style = style; }
    destroy_all_children() { for (const child of this.children) child.destroy?.(); this.children = []; }
    get_preferred_size() { return [0, 0, 300, 70 * this.children.length]; }
    remove_all_transitions() {}
}
class Clone {
    constructor(properties) { this.source = properties.source; }
    set_scale(x, y) { this.scaleX = x; this.scaleY = y; }
    set_position(x, y) { this.x = x; this.y = y; }
    set_pivot_point() {}
}
const monitor = new Signals(); monitor.cancel = () => { monitor.cancelled = true; };
const file = {
    monitor_directory: () => monitor,
    read_async: (_priority, _cancel, callback) => callback(file, null),
    read_finish: () => ({
        read_bytes_async(_count, _priority, _cancel, callback) { callback(this, null); },
        read_bytes_finish: () => ({get_data: () => new TextEncoder().encode(configText)}),
        close_async() {},
    }),
};
// Session bus names and their watches. GDBus reports a watched name from the
// main loop, never inside bus_watch_name; flushBus() runs that dispatch.
const busNames = new Set([':1.42']), watchers = new Map(), busQueue = [];
let watchId = 0, bridgeXml = '';
function flushBus() { while (busQueue.length) busQueue.shift()(); }
function dropBusName(name) {
    busNames.delete(name);
    for (const [id, watcher] of watchers)
        if (watcher.name === name) busQueue.push(() => { if (watchers.has(id)) watcher.vanished(Gio.DBus.session, name); });
    flushBus();
}
const Gio = {
    Cancellable: class { cancel() { this.cancelled = true; } },
    File: {new_for_path: () => file}, FileMonitorFlags: {NONE: 0},
    DBus: {session: {}}, BusNameWatcherFlags: {NONE: 0},
    DBusExportedObject: {wrapJSObject: (xml, object) => {
        bridgeXml = xml;
        return {
            export() { exported.push(object); },
            unexport() { exported.splice(exported.indexOf(object), 1); },
            emit_signal: (name, values) => emitted.push({name, values: values.values}),
        };
    }},
    bus_watch_name_on_connection: (connection, name, _flags, appeared, vanished) => {
        assert.equal(connection, Gio.DBus.session);
        const id = ++watchId;
        watchers.set(id, {name, appeared, vanished});
        busQueue.push(() => {
            if (!watchers.has(id)) return;
            if (busNames.has(name)) appeared(connection, name, name); else vanished(connection, name);
        });
        return id;
    },
    bus_unwatch_name: id => assert.ok(watchers.delete(id), 'unwatch owns a bus name watch'),
};
// Calls the exported bridge the way GJS does: a plain method wins over its
// ...Async form, which receives the invocation and must answer through it.
function bridge(method, args = [], sender = ':1.42') {
    const object = exported.at(-1);
    if (typeof object[method] === 'function') return object[method](...args);
    assert.equal(typeof object[`${method}Async`], 'function', `the bridge handles ${method}`);
    let reply;
    object[`${method}Async`](args, {get_sender: () => sender, return_value: value => { reply = value; }});
    return reply;
}
function makeWindow(pid, wmClass, exe, cmdline) {
    const window = new Signals();
    Object.assign(window, {fullscreen: false, wmClass, is_fullscreen: () => window.fullscreen, get_pid: () => pid,
        get_wm_class: () => window.wmClass});
    processes.set(pid, {exe, cmdline});
    return window;
}
const demoPath = `${home}/.local/share/Steam/steamapps/common/Demo/demo`;
const focus = makeWindow(42, 'Demo', demoPath, `${demoPath}\0-fullscreen\0`);
focus.get_gtk_application_id = () => 'org.test.Demo';
const stage = new Signals();
const view = {get_refresh_rate: () => 144}; stage.get_view_at = () => view;
const display = new Signals(); display.focus_window = focus;
function focusWindow(window) { display.focus_window = window; display.emit('notify::focus-window'); }
const tracker = new Signals(); tracker.visible = true;
tracker.get_pointer_visible = () => tracker.visible;
const idle = {id: 0, watches: new Map(),
    add_idle_watch(_interval, callback) { this.watches.set(++this.id, {idle: true, callback}); return this.id; },
    add_user_active_watch(callback) { this.watches.set(++this.id, {idle: false, callback}); return this.id; },
    remove_watch(id) { this.watches.delete(id); },
};
const layout = new Signals(); layout.addChrome = (_actor, options) => assert.equal(options.affectsInputRegion, false);
layout.primaryMonitor = {x: 0, y: 0, width: 1920, height: 1080, index: 0};
layout.getWorkAreaForMonitor = () => ({x: 0, y: 0, width: 1920, height: 1040});
const session = new Signals();
const Main = {layoutManager: layout, sessionMode: session};
const Clutter = {EVENT_PROPAGATE: 0, ModifierType: {BUTTON1_MASK: 256, BUTTON2_MASK: 512, BUTTON3_MASK: 1024},
    EventType: {BUTTON_PRESS: 1, BUTTON_RELEASE: 2, MOTION: 3, KEY_PRESS: 4, KEY_RELEASE: 5}, EventFlags: {REPEATED: 2}, Clone,
    BoxLayout: class { constructor(properties) { Object.assign(this, properties); } },
    Orientation: {HORIZONTAL: 0, VERTICAL: 1}, ActorAlign: {FILL: 0, START: 1, CENTER: 2, END: 3}};
const global = {stage, display, window_group: {}, get_pointer: () => [pointer.x, pointer.y, pointer.modifiers],
    get_backend: () => ({get_cursor_tracker: () => tracker, get_core_idle_monitor: () => idle})};
const sandbox = {Cairo: {Operator: {CLEAR: 0, OVER: 1}, RadialGradient: class { addColorStopRGBA() {} }}, Clutter, Gio, GLib, Meta: {},
    Shell: {WindowTracker: {get_default: () => ({get_window_app: () => ({get_id: () => 'org.test.Demo.desktop'})})}},
    St: {DrawingArea: Area, Label, BoxLayout: Box, Widget, ThemeContext: {get_for_stage: () => theme}},
    Extension: class {}, Main, global, TextDecoder, console, ...core};
let source = await readFile(new URL('./extension.js', import.meta.url), 'utf8');
source = source.replace(/^import[\s\S]*?;\n/gm, '').replace('export default class ClickShadeExtension',
    'globalThis.ClickShadeExtension = class ClickShadeExtension');
vm.runInNewContext(source, sandbox, {filename: 'extension.js'});
const extension = new sandbox.ClickShadeExtension();
const clickSignals = () => emitted.filter(signal => signal.name === 'Click');
const allowedSignals = () => emitted.filter(signal => signal.name === 'AllowedChanged');
const mouseRule = () => emitted.filter(signal => signal.name === 'MouseAllowedChanged').at(-1).values[0];
extension.enable();
check(exported.length === 1 && extension.GetStatus()[1], 'enable exports the native GNOME bridge');
check(/<method name="SetActive"><arg name="active" type="b" direction="in"\/><\/method>/.test(bridgeXml) &&
    /<method name="GetStatus">(\s*<arg name="\w+" type="[bs]" direction="out"\/>){5}\s*<\/method>/.test(bridgeXml) &&
    ['Click', 'Key', 'AllowedChanged', 'MouseAllowedChanged'].every(name => bridgeXml.includes(`<signal name="${name}">`)),
    'the bridge keeps the methods and signals the native app uses');
check(extension.GetStatus().length === 5 &&
    extension.GetStatus().every((value, index) => typeof value === (index < 2 ? 'boolean' : 'string')),
    'GetStatus still answers with the (bbsss) reply the native app reads');
check(actors.size === 0 && timers.size === 0 && !extension.GetStatus()[0] && extension.GetStatus()[2] === 'Paused',
    'without the ClickShade app the extension draws and samples nothing');
check(allowedSignals().at(-1).values[0] === false && mouseRule() === false,
    'before the app activates the bridge no sound is allowed');
check(bridge('SetActive', [true]) === null && extension.SetActive === undefined,
    'SetActive is answered through its invocation, which names the calling app');
flushBus();
check(watchers.size === 1 && [...watchers.values()][0].name === ':1.42', 'the bus name of the activating app is watched');
check(allowedSignals().at(-1).values[0] === true && mouseRule() === true,
    'activation publishes the current audio rule state to the native worker');
check(actors.size === 1 && [...actors][0].visible, 'activation creates a small visible cached highlight');
check([...actors].every(actor => !actor.reactive && !actor.can_focus && !actor.track_hover),
    'all overlay actors remain nonreactive and cannot acquire keyboard focus');
check(extension._rate === 240, 'manual high-refresh setting reaches the native scheduler');
const halo = [...actors][0];
const rasterizations = halo.paintCount;
pointer.x = 310; pointer.y = 240;
advance(25);
check(halo.x + halo.width / 2 === pointer.x && halo.y + halo.height / 2 === pointer.y,
    'the cached highlight follows the current global pointer');
check(halo.paintCount === rasterizations, 'moving a highlight does not rebuild its Cairo texture');
function event(pressed, button = 1) {
    const bit = button === 1 ? 256 : button === 2 ? 512 : 1024;
    pointer.modifiers = pressed ? pointer.modifiers | bit : pointer.modifiers & ~bit;
    return stage.emit('captured-event', {type: () => pressed ? 1 : 2,
        get_button: () => button, get_coords: () => [pointer.x, pointer.y]});
}
check(event(true) === Clutter.EVENT_PROPAGATE, 'captured mouse input always propagates to its real target');
advance(25);
check(extension._particles.length === 1 && clickSignals().length === 1,
    'captured input plus button polling creates one click and one sound notification');
event(false);
pointer.modifiers = 1024;
advance(10);
check(extension._particles.length === 2 && clickSignals().at(-1).values[0] === 2,
    'sampled right-button transitions work when Shell does not dispatch a captured event');
pointer.modifiers = 0; advance(10);
const beforeMiddle = clickSignals().length;
const particlesBeforeMiddle = extension._particles.length;
event(true, 2); advance(10); event(false, 2);
check(clickSignals().length === beforeMiddle + 1 && clickSignals().at(-1).values[0] === 4 &&
    extension._particles.length === particlesBeforeMiddle, 'middle press emits its own sound exactly once without inventing a right-click visual');
pointer.modifiers = 512; advance(10); pointer.modifiers = 0; advance(10);
check(clickSignals().length === beforeMiddle + 2 && clickSignals().at(-1).values[0] === 4,
    'sampled middle-button sound works without a captured event');
for (let i = 0; i < 20; ++i) { event(true); event(false); }
check(extension._particles.length === 8 && actors.size === 9, 'rapid presses are bounded to eight particles plus one halo');
const current = extension._particles[0];
configText += 'SoundVolume=40\n'; extension.Reload();
check(extension._particles[0] === current, 'unrelated live settings preserve existing click lifetimes');
advance(500);
check(extension._particles.length === 0 && actors.size === 1, 'expired click actors are destroyed');

configText += 'LeftClickEnabled=0\n'; extension.Reload();
const beforeSound = clickSignals().length; event(true); event(false);
check(extension._particles.length === 0 && clickSignals().length === beforeSound + 1,
    'left visual selection does not disable separately configured left click audio');
configText += 'LeftClickEnabled=1\n'; extension.Reload();
focus.fullscreen = true; display.emit('in-fullscreen-changed');
check(!halo.visible && timers.size === 0 && extension.GetStatus()[2] === 'Paused for a fullscreen game',
    'a fullscreen game (a program in a Steam library) hides actors and stops sampling');
check(allowedSignals().at(-1).values[0] === false,
    'fullscreen entry notifies the worker to flush existing audio voices');
event(true); event(false);
check(extension._particles.length === 0, 'paused fullscreen input cannot create an effect');
configText += 'AllowedProcesses=DEMO\n'; extension.Reload();
check(halo.visible && allowedSignals().at(-1).values[0] === true,
    'an explicit application allowance overrides fullscreen pause');
configText += 'ExcludedProcesses=demo\n'; extension.Reload();
check(!halo.visible, 'an explicit exclusion takes priority over an application allowance');
configText += 'ExcludedProcesses=\n'; extension.Reload();
session.isLocked = true; session.emit('updated');
check(!halo.visible, 'an allowed application cannot override the lock screen');
session.isLocked = false; session.emit('updated');
tracker.visible = false; tracker.emit('visibility-changed');
check(!halo.visible, 'an allowed application cannot override an intentionally hidden pointer');
tracker.visible = true; tracker.emit('visibility-changed');
configText += 'AllowedProcesses=\n'; extension.Reload();
check(!halo.visible, 'removing an allowance restores the fullscreen rule immediately');
focus.fullscreen = false; display.emit('in-fullscreen-changed');
check(halo.visible, 'leaving fullscreen resumes the steady highlight');
check(allowedSignals().at(-1).values[0] === true,
    'leaving fullscreen resumes audio eligibility');

// Only games pause in fullscreen; browsers, video and presentations keep their effects.
const editor = makeWindow(80, 'org.gnome.TextEditor', '/usr/bin/gnome-text-editor', '/usr/bin/gnome-text-editor\0/home/test/notes.txt\0');
const readsBeforeEditor = procReads;
focusWindow(editor);
check(procReads === readsBeforeEditor + 1 && halo.visible, 'a windowed application has only its executable read, never its command line');
const browser = makeWindow(77, 'firefox', '/usr/lib/firefox/firefox', '/usr/lib/firefox/firefox\0https://example.com/setup.exe\0');
browser.fullscreen = true; focusWindow(browser);
check(halo.visible && allowedSignals().at(-1).values[0] === true && !extension.GetStatus()[2].includes('Paused'),
    'a fullscreen browser keeps its effects and sounds');
const readsAfterFirstFocus = procReads;
focusWindow(focus); focusWindow(browser); display.emit('in-fullscreen-changed');
check(procReads === readsAfterFirstFocus && halo.visible, 'process details are read once per window');
browser.wmClass = 'steam_app_620'; display.emit('in-fullscreen-changed');
check(!halo.visible && procReads === readsAfterFirstFocus, 'a window class set after mapping is rechecked without rereading the process');
const proton = makeWindow(78, 'demo.exe', undefined, 'Z:\\home\\test\\Games\\demo\\demo.exe\0');
proton.fullscreen = true; focusWindow(proton);
check(!halo.visible && allowedSignals().at(-1).values[0] === false,
    'a fullscreen Wine/Proton program pauses even when its executable cannot be read');
const presentation = makeWindow(79, 'libreoffice-impress', '/usr/lib/libreoffice/program/soffice.bin', '/usr/lib/libreoffice/program/soffice.bin\0--impress\0');
presentation.fullscreen = true; focusWindow(presentation);
check(halo.visible, 'a fullscreen presentation keeps its effects');
configText += 'PauseFullscreen=0\n'; extension.Reload(); focusWindow(proton);
check(halo.visible, 'switching the fullscreen pause off keeps effects over fullscreen games');
configText += 'PauseFullscreen=1\n'; extension.Reload();
focusWindow(focus);
check(halo.visible, 'returning to a windowed game resumes the effects');
configText += 'ExcludedProcesses=demo\n'; extension.Reload();
check(!halo.visible && extension.GetStatus()[2].includes('excluded'),
    'foreground executable/application exclusions suppress effects');
configText += 'ExcludedProcesses=\n'; extension.Reload();
tracker.visible = false; tracker.emit('visibility-changed');
check(!halo.visible && timers.size === 0, 'a hidden real cursor suppresses all visual work');
tracker.visible = true; tracker.emit('visibility-changed');
check(halo.visible, 'cursor visibility notification resumes the overlay');
theme.scale_factor = 2; theme.emit('notify::scale-factor');
check(halo.width === 24 * 2 * 2 + 8, 'highlight geometry follows the GNOME UI scale');

// Highlight/click styles, middle clicks, breathing and the keyboard bridge.
configText += 'HighlightStyle=3\nClickStyle=4\nMiddleClickEnabled=1\nKeyboardSoundsEnabled=1\n'; extension.Reload();
check(extension._config.HighlightStyle === 3 && extension._config.ClickStyle === 4, 'style numbers reach the extension configuration');
check(halo.width === Math.ceil(24 * 2 * 1.75 * 2) + 8, 'the crosshair highlight reserves room for its arms');
const particlesBeforeMiddleVisual = extension._particles.length;
event(true, 2); advance(10); event(false, 2);
check(extension._particles.length === particlesBeforeMiddleVisual + 1 &&
    extension._particles.at(-1).color === core.DEFAULTS.MiddleClickColor && extension._particles.at(-1).style === 4,
    'an enabled middle click draws its own neon particle in the selected style');
configText += 'ClickStyle=5\n'; extension.Reload();
event(true, 1); advance(10); event(false, 1);
const targetParticle = extension._particles.at(-1);
check(targetParticle.style === core.CLICK_STYLES.TARGET && targetParticle.circle.shape.radius > targetParticle.maximum * 0.9,
    'a target click starts near its full radius');
advance(Math.round(core.DEFAULTS.ClickDurationMs * 0.8));
check(!targetParticle.circle.actor.destroyed && targetParticle.circle.shape.radius < targetParticle.maximum * 0.8,
    'a target click contracts toward the click point');
configText += 'ClickStyle=4\n'; extension.Reload();
const keySignals = () => emitted.filter(signal => signal.name === 'Key');
const keysBefore = keySignals().length;
const keyEvent = (keysym, flags = 0) => stage.emit('captured-event', {type: () => 4, get_key_symbol: () => keysym, get_flags: () => flags});
check(keyEvent(0x20) === Clutter.EVENT_PROPAGATE && keyEvent(0x61) === Clutter.EVENT_PROPAGATE && keyEvent(0x61, 2) === Clutter.EVENT_PROPAGATE,
    'captured key input always propagates to its real target');
check(keySignals().length === keysBefore + 2 && keySignals().at(-2).values[0] === 1 && keySignals().at(-1).values[0] === 0,
    'key presses are reduced to a coarse category and auto-repeat is ignored');
focus.fullscreen = true; display.emit('in-fullscreen-changed'); keyEvent(0x20);
check(keySignals().length === keysBefore + 2, 'paused rules also silence keyboard notifications');
focus.fullscreen = false; display.emit('in-fullscreen-changed');
// Editors hide the pointer while you type: keyboard sounds keep playing.
tracker.visible = false; tracker.emit('visibility-changed');
keyEvent(0x61);
check(keySignals().length === keysBefore + 3 && allowedSignals().at(-1).values[0] === true && mouseRule() === false,
    'a hidden pointer keeps keyboard sounds and their audio rule; only the mouse rule turns off');
const clicksWhileHidden = clickSignals().length; event(true); event(false);
check(clickSignals().length === clicksWhileHidden && !halo.visible,
    'a hidden pointer silences click notifications and hides the highlight');
tracker.visible = true; tracker.emit('visibility-changed');
check(mouseRule() === true && halo.visible, 'the mouse rule and the highlight return with the pointer');
configText += 'KeyboardSoundsEnabled=0\n'; extension.Reload(); keyEvent(0x20);
check(keySignals().length === keysBefore + 3, 'keyboard notifications stop as soon as keyboard sounds are off');
configText += 'HighlightEnabled=0\nClickEffectsEnabled=0\nClickSoundsEnabled=0\nKeyboardSoundsEnabled=1\n'; extension.Reload();
keyEvent(0x61); advance(100);
check(extension._timer === 0 && !halo.visible && keySignals().length === keysBefore + 4,
    'keyboard sounds alone never run the pointer sampling timer');
configText += 'HighlightEnabled=1\nClickEffectsEnabled=1\nClickSoundsEnabled=1\nKeyboardSoundsEnabled=0\n'; extension.Reload();
check(halo.visible && extension._timer !== 0, 'switching a pointer effect back on resumes sampling');
configText += 'HighlightBreathing=1\n'; extension.Reload();
let breathingPaints = halo.paintCount;
advance(1000);
const breathsWhileSampling = halo.paintCount - breathingPaints;
check(breathsWhileSampling >= 28 && breathsWhileSampling <= 32,
    'a breathing highlight repaints about 30 times a second while sampling runs at 240 Hz');
const idleBreath = [...idle.watches.values()].find(watch => watch.idle);
idleBreath.callback();
const callbacksBeforeIdle = callbacks; breathingPaints = halo.paintCount;
advance(1000);
check(callbacks - callbacksBeforeIdle <= 32 && halo.paintCount - breathingPaints >= 28 && halo.visible,
    'an idle breathing highlight wakes only at the pace of the breath');
const breathResume = [...idle.watches].find(([, watch]) => !watch.idle);
idle.watches.delete(breathResume[0]); breathResume[1].callback();
configText += 'HighlightBreathing=0\nHighlightIdleHideMs=1500\n'; extension.Reload(); advance(500);
check(halo.visible, 'the highlight stays visible while the pointer rested for less than the idle limit');
advance(1200);
check(!halo.visible, 'the highlight hides once the pointer rests beyond the idle limit');
pointer.x += 5; advance(20);
check(halo.visible, 'movement brings the hidden highlight back');
pointer.x -= 5; advance(20);
configText += 'HighlightIdleHideMs=0\nHighlightStyle=0\nClickStyle=0\nMiddleClickEnabled=0\n'; extension.Reload(); advance(500);

// Key display and magnifier lens.
check(core.keyChordLabel(0x73, core.KEY_MODIFIERS.CONTROL | core.KEY_MODIFIERS.SHIFT) === 'Ctrl + Shift + S' &&
    core.keyChordLabel(0xffbe, 0) === 'F1' && core.keyChordLabel(0xff53, 0) === '→' &&
    core.keyChordLabel(0xffeb, 0) === 'Super' && core.keyChordLabel(0x71, core.KEY_MODIFIERS.ALTGR) === 'AltGr + Q' &&
    core.keyChordLabel(1, core.KEY_MODIFIERS.MOUSE) === 'Left click', 'key labels follow the shared naming');
check(core.keyDisplayWanted(0, 0x73, core.KEY_MODIFIERS.CONTROL) && !core.keyDisplayWanted(0, 0x73, 0) &&
    !core.keyDisplayWanted(0, 0x73, core.KEY_MODIFIERS.SHIFT) && core.keyDisplayWanted(0, 0xffbe, 0) &&
    !core.keyDisplayWanted(0, 0x71, core.KEY_MODIFIERS.ALTGR) && core.keyDisplayWanted(1, 0x73, 0),
    'shortcuts-only hides typing and shows chords, function keys and tapped modifiers');
check(core.modifiersFromState(4 | 1) === (core.KEY_MODIFIERS.CONTROL | core.KEY_MODIFIERS.SHIFT) &&
    core.modifiersFromState(64) === core.KEY_MODIFIERS.SUPER && core.modifiersFromState(128) === core.KEY_MODIFIERS.ALTGR,
    'Clutter modifier state maps onto the shared flags');
const beside = core.lensOrigin(100, 500, 200, {x: 0, y: 0, width: 1920, height: 1080});
const flipped = core.lensOrigin(1900, 50, 200, {x: 0, y: 0, width: 1920, height: 1080});
check(beside.x > 100 && beside.y + 200 < 500 && flipped.x + 200 < 1900 && flipped.y > 50, 'the lens sits beside the pointer and flips inside the monitor');
check(core.keyChordParts(0x73, core.KEY_MODIFIERS.SUPER | core.KEY_MODIFIERS.CONTROL | core.KEY_MODIFIERS.SHIFT).join('|') === 'Super|Ctrl|Shift|S' &&
    core.keyChordParts(0x71, core.KEY_MODIFIERS.ALTGR).join('|') === 'AltGr|Q' && core.keyChordParts(0xffeb, 0).join('|') === 'Super',
    'keycaps list the modifiers in order, then the key');
check(core.typedText(0x61, 0) === 'a' && core.typedText(0x41, core.KEY_MODIFIERS.SHIFT) === 'A' && core.typedText(0x40, core.KEY_MODIFIERS.ALTGR) === '@' &&
    core.typedText(0x20, 0) === ' ' && core.typedText(0xe4, 0) === 'ä',
    'typed text comes from the event character, including Shift and AltGr');
check(core.typedText(0x73, core.KEY_MODIFIERS.CONTROL) === '' && core.typedText(0x0d, 0) === '' && core.typedText(0x08, 0) === '' &&
    core.typedText(0, 0) === '' && core.typedText(0x61, core.KEY_MODIFIERS.MOUSE) === '',
    'shortcuts, control characters and keys without text type nothing');
{
    let rows = [];
    const hold = 1000000;
    rows = core.addKeyStroke(rows, {caps: ['A'], typed: 'h'}, 0, hold);
    rows = core.addKeyStroke(rows, {caps: ['I'], typed: 'i'}, 100000, hold);
    check(rows.length === 1 && rows[0].typed === 'hi', 'typed characters join one row');
    rows = core.addKeyStroke(rows, {caps: ['Backspace'], backspace: true}, 200000, hold);
    check(rows.length === 1 && rows[0].typed === 'h', 'Backspace edits the typed row');
    rows = core.addKeyStroke(rows, {caps: ['Ctrl', 'S']}, 300000, hold);
    rows = core.addKeyStroke(rows, {caps: ['Ctrl', 'S']}, 400000, hold);
    check(rows.length === 2 && rows[1].repeat === 2 && rows[1].caps.join('+') === 'Ctrl+S', 'a repeated shortcut counts up in its row');
    rows = core.addKeyStroke(rows, {caps: ['Space'], typed: ' '}, 500000, hold);
    check(rows.length === 3 && rows[2].caps.join() === 'Space', 'a space outside a typed row is a keycap');
    rows = core.addKeyStroke(rows, {caps: ['F5']}, 600000, hold);
    check(rows.length === core.KEY_ROW_LIMIT && rows[0].caps.join('+') === 'Ctrl+S' && rows.at(-1).caps.join() === 'F5',
        'the oldest row leaves once the display is full');
    rows = core.addKeyStroke([{caps: [], typed: 'old', repeat: 1, updated: 0}], {caps: ['N'], typed: 'n'}, 2000000, hold);
    check(rows.length === 2 && rows[1].typed === 'n', 'a typed row that has started to fade is not extended');
}
const keyPress = (keysym, state = 0, unicode = 0) => stage.emit('captured-event', {type: () => 4, get_key_symbol: () => keysym,
    get_state: () => state, get_flags: () => 0, get_key_unicode: () => unicode});
const keyRelease = (keysym, state = 0) => stage.emit('captured-event', {type: () => 5, get_key_symbol: () => keysym, get_state: () => state, get_flags: () => 0});
const rowTexts = () => extension._keyStack.children.map(row => row.children.map(label => label.text).join('|'));
const actorsBeforeKeys = actors.size;
keyPress(0x61, 0, 0x61);
check(actors.size === actorsBeforeKeys && !extension._keyStack, 'no key display actor exists while the switch is off');
configText += 'KeyDisplayEnabled=1\nKeyDisplayMode=1\nKeyDisplayDurationMs=1000\n'; extension.Reload();
keyPress(0x61, 0, 0x61);
check(extension._keyStack?.visible && rowTexts().join() === 'a', 'a typed key starts a typed row');
check(!extension._keyStack.reactive && extension._keyStack.y + extension._keyStack.get_preferred_size()[3] <= 1040 && extension._keyStack.x > 0,
    'the key display is nonreactive and placed inside the bottom of the work area');
keyPress(0x62, 0, 0x62);
keyPress(0xff08, 0, 0x08);
keyPress(0x63, 0, 0x63);
check(rowTexts().join() === 'ac', 'typing joins one row and Backspace edits it');
keyPress(0xffe3, 0); keyPress(0xffe1, 4); keyPress(0x73, 4 | 1, 0x13); keyRelease(0x73, 4 | 1); keyRelease(0xffe1, 4 | 1); keyRelease(0xffe3, 4);
check(rowTexts().length === 2 && rowTexts()[1] === 'Ctrl|Shift|S', 'a chord becomes a row of keycaps');
keyPress(0xffe3, 0); keyPress(0xffe1, 4); keyPress(0x73, 4 | 1, 0x13); keyRelease(0x73, 4 | 1); keyRelease(0xffe1, 4 | 1); keyRelease(0xffe3, 4);
check(rowTexts().length === 2 && rowTexts()[1] === 'Ctrl|Shift|S|×2', 'a repeated chord counts up instead of adding a row');
keyPress(0xffeb, 0); keyRelease(0xffeb, 64);
check(rowTexts().at(-1) === 'Super', 'a tapped Super key is labelled on release');
keyPress(0xffe3, 0); keyPress(0x63, 4, 0x03); keyRelease(0x63, 4); keyRelease(0xffe3, 4);
check(rowTexts().at(-1) === 'Ctrl|C' && rowTexts().length === 3, 'a modifier used in a chord is not labelled again, and at most three rows show');
advance(1400);
check(!extension._keyStack.visible && extension._keyRows.length === 0, 'every row fades after the hold time');
configText += 'KeyDisplayMode=0\n'; extension.Reload();
keyPress(0x61, 0, 0x61);
check(!extension._keyStack.visible, 'shortcuts-only ignores plain typing');
keyPress(0xffc2, 0);
check(extension._keyStack.visible && rowTexts()[0] === 'F5', 'shortcuts-only still shows function keys');
tracker.visible = false; tracker.emit('visibility-changed');
keyPress(0x73, 4, 0x13);
check(extension._keyStack.visible && rowTexts().at(-1) === 'Ctrl|S', 'a hidden pointer does not pause the key display');
tracker.visible = true; tracker.emit('visibility-changed');
focus.fullscreen = true; display.emit('in-fullscreen-changed');
check(!extension._keyStack.visible, 'fullscreen pause hides the key display');
keyPress(0x73, 4, 0x13);
check(!extension._keyStack.visible, 'paused rules ignore key presses');
focus.fullscreen = false; display.emit('in-fullscreen-changed');
configText += 'HighlightEnabled=0\nClickEffectsEnabled=0\nClickSoundsEnabled=0\n'; extension.Reload();
keyPress(0xffc2, 0); advance(100);
check(extension._timer === 0 && extension._keyStack.visible && rowTexts().at(-1) === 'F5',
    'the key display without mouse buttons never samples the pointer');
configText += 'KeyDisplayMouse=1\n'; extension.Reload();
check(extension._timer !== 0, 'mouse buttons in the key display need the pointer sampling');
configText += 'HighlightEnabled=1\nClickEffectsEnabled=1\nClickSoundsEnabled=1\n'; extension.Reload();
event(true); event(false);
check(extension._keyStack.visible && rowTexts().at(-1) === 'Left click', 'mouse buttons are labelled when that switch is on');
configText += 'KeyDisplayEnabled=0\nKeyDisplayMouse=0\n'; extension.Reload();
check(!extension._keyStack.visible, 'switching the key display off hides it');
configText += 'MagnifierEnabled=1\nMagnifierZoom=300\nMagnifierSize=200\n'; extension.Reload();
pointer.x = 400; pointer.y = 600; advance(25);
const lensSize = 200 * theme.scale_factor; // The lens follows the GNOME UI scale like every other actor.
check(extension._lens?.visible && extension._lens.width === lensSize && extension._lens.height === lensSize && !extension._lens.reactive,
    'an enabled magnifier shows a square nonreactive lens scaled with the UI');
check(extension._lens.x > pointer.x && extension._lens.y + lensSize < pointer.y, 'the lens sits above and to the right of the pointer');
check(extension._lensClone.source === global.window_group && extension._lensClone.scaleX === 3 &&
    extension._lensClone.x === Math.round(lensSize / 2 - 400 * 3) && extension._lensClone.y === Math.round(lensSize / 2 - 600 * 3),
    'the lens clones the window layer scaled so the pointer position sits at its center');
// Hidden pointer: every pointer effect hides, except the lens during a typing hold.
advance(1600);
tracker.visible = false; tracker.emit('visibility-changed');
check(!extension._lens.visible && !halo.visible && extension._typingTimer === 0,
    'a pointer hidden without recent typing hides the lens with every other pointer effect');
tracker.visible = true; tracker.emit('visibility-changed');
check(extension._lens.visible && halo.visible, 'the lens and the highlight return with the pointer');
keyPress(0x61, 0, 0x61); advance(500);
tracker.visible = false; tracker.emit('visibility-changed');
check(extension._lens.visible && !halo.visible && timers.size === 1,
    'a pointer that hides right after a key press is a typing hold: the lens stays, the highlight hides, sampling stops');
advance(2000); keyPress(0x62, 0, 0x62); advance(2500);
check(extension._lens.visible, 'each key press extends the typing hold');
advance(600);
check(!extension._lens.visible && extension._typingTimer === 0 && timers.size === 0,
    'about 3 s without a key ends the hold and hides the lens');
keyPress(0x63, 0, 0x63);
check(!extension._lens.visible, 'typing while the pointer stays hidden starts no new hold');
tracker.visible = true; tracker.emit('visibility-changed');
check(extension._lens.visible, 'the lens returns when the pointer shows again');
keyPress(0x61, 0, 0x61); advance(1600);
tracker.visible = false; tracker.emit('visibility-changed');
check(!extension._lens.visible, 'a pointer that hides more than 1.5 s after the last key is no typing hold');
tracker.visible = true; tracker.emit('visibility-changed');
keyPress(0x61, 0, 0x61);
tracker.visible = false; tracker.emit('visibility-changed');
tracker.visible = true; tracker.emit('visibility-changed');
check(extension._lens.visible && extension._typingTimer === 0, 'showing the pointer ends the hold and its timer');
focus.fullscreen = true; display.emit('in-fullscreen-changed');
check(!extension._lens.visible, 'other pauses hide the lens');
focus.fullscreen = false; display.emit('in-fullscreen-changed');
advance(25);
check(/border: \d+px solid/.test(extension._lens.style), 'the lens has a colored frame by default');
configText += 'MagnifierFrame=0\n'; extension.Reload(); advance(25);
check(extension._lens.visible && /border: none/.test(extension._lens.style), 'switching the frame off leaves the lens without a border');
configText += 'MagnifierEnabled=0\n'; extension.Reload(); advance(25);
check(!extension._lens.visible, 'switching the magnifier off hides the lens');
pointer.x = 310; pointer.y = 240; advance(25); // Back where the earlier checks left the mock pointer.
bridge('SetActive', [false]);
check(!halo.visible && timers.size === 0 && watchers.size === 0,
    'the GTK bridge can pause without disabling or modifying the real pointer, and stops watching the app');
bridge('SetActive', [true]); flushBus();
const ruleSignalsBeforeReconnect = allowedSignals().length;
bridge('SetActive', [true]); flushBus();
check(allowedSignals().length === ruleSignalsBeforeReconnect + 1 && allowedSignals().at(-1).values[0] && watchers.size === 1,
    'a reconnecting GTK process receives the unchanged current audio rule state and keeps a single watch');
const idleEntry = [...idle.watches.values()].find(watch => watch.idle);
idleEntry.callback();
check(timers.size === 0 && halo.visible, 'idle observation retains the highlight but stops repeating sampling');
const activeEntry = [...idle.watches].find(([, watch]) => !watch.idle);
idle.watches.delete(activeEntry[0]); activeEntry[1].callback();
check(timers.size === 1, 'the one-shot user-active watch resumes sampling after idle');
extension.Restore();
check(extension.GetStatus()[0] === false && timers.size === 0 && watchers.size === 0, 'Restore clears visuals and pauses the extension');
bridge('SetActive', [true]); flushBus();
check(halo.visible && extension.GetStatus()[0], 'the app can resume the effects after Restore');

// The app quits, crashes or is killed without SetActive(false).
dropBusName(':1.42');
check(!halo.visible && timers.size === 0 && !extension.GetStatus()[0] && watchers.size === 0 &&
    allowedSignals().at(-1).values[0] === false && mouseRule() === false,
    'effects and sounds stop when the app that activated them leaves the bus');
event(true); event(false); keyEvent(0x20);
check(extension._particles.length === 0 && !halo.visible && !extension._lens.visible, 'no effect starts after the app has gone');
busNames.add(':1.43');
bridge('SetActive', [true], ':1.43'); flushBus();
check(halo.visible && extension.GetStatus()[0] && [...watchers.values()][0].name === ':1.43',
    'a newly started app activates the effects again');

// The lock screen disables the extension and enables it again afterwards.
extension.disable();
check(watchers.size === 0 && actors.size === 0 && timers.size === 0, 'disable also removes the watch of the app');
extension.enable();
check(!extension.GetStatus()[0] && actors.size === 0, 'a re-enabled extension first checks that its app still runs');
flushBus();
check(extension.GetStatus()[0] && extension._highlight?.actor.visible && allowedSignals().at(-1).values[0] === true,
    'after the lock screen the effects resume while the app that activated them still runs');
extension.disable();
dropBusName(':1.43');
extension.enable(); flushBus();
check(!extension.GetStatus()[0] && actors.size === 0 && watchers.size === 0,
    'an app that quit while the extension was disabled does not resume the effects');
busNames.add(':1.44');
bridge('SetActive', [true], ':1.44'); flushBus();
check(extension._highlight?.actor.visible, 'the next app start activates them as usual');
extension.disable();
check(allowedSignals().at(-1).values[0] === false && mouseRule() === false,
    'disable stops active audio before unexporting the bridge');
check(actors.size === 0 && timers.size === 0 && exported.length === 0 && watchers.size === 0,
    'disable destroys every actor, timer, bus name watch, and exported D-Bus object');
check([stage, display, layout, tracker, theme, session, focus, editor, browser, proton, presentation, monitor]
    .every(object => object.signals.size === 0) &&
    idle.watches.size === 0 && monitor.cancelled, 'disable disconnects signals, idle watches and the file monitor');
check(pointer.x === 310 && pointer.y === 240 && pointer.modifiers === 0,
    'the extension never rewrites pointer position, visibility, theme, or button state');
console.log(`RESULT ${checks} passed; headless mocks only, native GNOME runtime remains unverified.`);
