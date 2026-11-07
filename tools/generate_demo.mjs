#!/usr/bin/env node
// Generate the README's illustrative feature showcase without opening an app.
// Requires Node.js, sharp and ffmpeg. Run from the repository root:
// node tools/generate_demo.mjs --work-dir build/demo --output assets/demo.gif
import { createRequire } from 'node:module';
import fs from 'node:fs/promises';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const sharp = createRequire(import.meta.url)('sharp');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const work = path.resolve(args.get('--work-dir') || 'build/demo');
const output = path.resolve(args.get('--output') || 'assets/demo.gif');
const width = 960, height = 520, fps = 20;
await fs.mkdir(work, { recursive: true });
await fs.mkdir(path.dirname(output), { recursive: true });

const chapters = [
  { id: 'cursor', duration: 5, title: 'Your Windows cursor. Your style.', subtitle: 'Press feedback, crisp scaling, colors, opacity and shadow.', items: ['Dark while held, or a brief flash', 'Theme, Windows, classic or custom .cur pair', '100–600% scale, including busy cursors', 'Opacity, shadow, fill and outline colors', 'Reverse light/dark; choose trigger buttons'] },
  { id: 'shake', duration: 3.5, title: 'Lost the pointer? Give it a shake.', subtitle: 'A quick wiggle enlarges the pointer so you can find it again.', items: ['Plasma-style or macOS-inspired presets', 'Adjust sensitivity and maximum size', 'Temporary enlargement; then back to normal'] },
  { id: 'highlights', duration: 5.5, title: 'Keep attention exactly where you need it.', subtitle: 'Five highlight styles, from a subtle ring to a focused spotlight.', items: ['Filled, ring, soft glow, crosshair, spotlight', 'Choose color, radius, opacity and width', 'Optional breathing animation', 'Hide the highlight when the pointer rests'] },
  { id: 'trails', duration: 5.5, title: 'Give movement a little personality.', subtitle: 'Five optional trails make the pointer easy to follow.', items: ['Dots, rings, sparkles, comet, fine sparkle', 'Adjust color, size, opacity and fade', 'Trails are off by default'] },
  { id: 'clicks', duration: 5.5, title: 'Every mouse button gets its own feedback.', subtitle: 'Six animations, with independent left, right and middle button colors.', items: ['Ring, pulse, ripple, burst, neon, target', 'Separate switches for all three buttons', 'Independent colors for each button', 'Radius, duration, opacity, width and easing'] },
  { id: 'magnifier', duration: 5, title: 'Make the small details readable.', subtitle: 'A moving lens enlarges the content beneath the pointer.', items: ['Round or rounded-square lens', 'Centered on or beside the pointer', '1.25–8× zoom; adjustable lens size', 'Frame color, or no frame', 'Highlights and clicks stay out of the lens'] },
  { id: 'keys', duration: 4.5, title: 'Let your audience follow every keystroke.', subtitle: 'Show shortcut keycaps, typed words and optional mouse buttons.', items: ['Shortcuts only, or every key', 'Typed letters join into words', 'Optional mouse button labels', 'Four positions; size, hold time and colors', 'Rows disappear after fading'] },
  { id: 'sounds', duration: 4, title: 'Add feedback you can hear.', subtitle: 'Mouse and keyboard sounds have separate switches and volume.', items: ['11 original mouse sets; 5 keyboard sets', 'Per-button files or your own audio', 'WAV, MP3, FLAC, M4A, AAC on Windows', 'Space, Enter and Backspace variants', 'Separate controls and shortcuts'] },
  { id: 'rules', duration: 4.5, title: 'Keep effects where they belong.', subtitle: 'Automatic pause rules and app lists fit around your workflow.', items: ['Pause in fullscreen games, confinement or lock', 'Exclude apps, or always allow desktop tools', 'Hide pointer effects with a hidden pointer', 'Key display and typing sounds can continue', 'Optional admin mode for elevated windows'] },
  { id: 'capture', duration: 4.5, title: 'Ready to present. Easy to recover.', subtitle: 'Choose capture visibility, with cursor restoration if the app crashes.', items: ['Show or hide effects in recordings', 'Works with OBS, ShareX and Snipping Tool', 'Crash: restore cursors and write a report', 'Automatic recovery; stops after 3 restarts'] },
  { id: 'settings', duration: 4, title: 'Make the controls comfortable, too.', subtitle: 'A settings window with compact cards and accessible adjustments.', items: ['Dark and light palettes; custom colors', 'Extra interface zoom', 'Reset each card or individual slider', 'Optional animated spectrum line', 'Settings save as you change them'] },
  { id: 'everyday', duration: 4.5, title: 'Set it up once. Make it part of your day.', subtitle: 'Tray controls, profiles and shortcuts keep everything close at hand.', items: ['9 configurable global shortcuts', 'Tray menu; optional start with Windows', 'Save profiles, back up and import settings', 'Restore all defaults whenever you like', 'No account, telemetry or network service'] }
];
let duration = 0;
for (const chapter of chapters) { chapter.start = duration; duration += chapter.duration; }

const clamp = x => Math.max(0, Math.min(1, x));
const smooth = x => { x = clamp(x); return x * x * (3 - 2 * x); };
const esc = s => s.replaceAll('&', '&amp;').replaceAll('<', '&lt;');
const svg = body => Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">${body}</svg>`);
const text = (x, y, value, size = 16, color = '#edf5fc', weight = 400, extra = '') => `<text x="${x}" y="${y}" font-family="Segoe UI,DejaVu Sans,sans-serif" font-size="${size}" font-weight="${weight}" fill="${color}" ${extra}>${esc(value)}</text>`;
const rect = (x, y, w, h, fill, stroke = 'none', r = 0, extra = '') => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}" stroke="${stroke}" ${extra}/>`;
const circle = (x, y, r, fill = 'none', stroke = '#79dfc6', line = 2, extra = '') => `<circle cx="${x}" cy="${y}" r="${r}" fill="${fill}" stroke="${stroke}" stroke-width="${line}" ${extra}/>`;
const label = (x, y, w, value, active = false) => rect(x, y, w, 29, active ? '#23463f' : '#203244', active ? '#79dfc6' : '#3b5268', 6) + text(x + w / 2, y + 19, value, 13, active ? '#a2f2df' : '#c2d4e3', 500, 'text-anchor="middle"');
function wrap(value, limit = 33) {
  const result = []; let line = '';
  for (const word of value.split(' ')) {
    if (line && line.length + word.length + 1 > limit) { result.push(line); line = ''; }
    line += (line ? ' ' : '') + word;
  }
  if (line) result.push(line);
  return result;
}
// Reuse CreateClassicArrow's Windows silhouette from src/cursor_engine.cpp.
// No Windows cursor files are redistributed by this generator.
const cursorOutline = 'M.5 .5V17.4L4.9 13.2 8.1 20 10.9 18.8 7.8 12.1 13.4 12.1Z';
function cursor(x, y, { scale = 1.5, fill = '#ffffff', outline = '#000000', opacity = 1, shadow = false } = {}) {
  const under = shadow ? `<path d="${cursorOutline}" transform="translate(2.5 3)" fill="#000" stroke="#000" stroke-width="3" opacity=".26"/>` : '';
  return `<g transform="translate(${x} ${y}) scale(${scale})" opacity="${opacity}">${under}<path d="${cursorOutline}" fill="${fill}" stroke="${outline}" stroke-width="1" stroke-linejoin="miter" stroke-miterlimit="2"/></g>`;
}

function shell(chapter, index) {
  let body = rect(0, 0, width, height, '#0d1520');
  body += rect(32, 26, 5, 19, '#79dfc6', 'none', 2);
  body += text(48, 41, 'CLICKSHADE / FEATURE SHOWCASE', 13, '#79dfc6', 700, 'letter-spacing="1.1"');
  body += text(928, 41, `${String(index + 1).padStart(2, '0')} / ${chapters.length}`, 13, '#a8bdcf', 600, 'text-anchor="end"');
  body += text(32, 94, chapter.title, 29, '#f0f6fc', 700);
  body += text(32, 124, chapter.subtitle, 16, '#afc2d3');
  body += rect(32, 155, 566, 278, '#162231', '#31465a', 12);
  body += rect(618, 155, 310, 278, '#121e2b', '#2d4255', 12);
  body += text(640, 183, 'FEATURES & OPTIONS', 12, '#79dfc6', 700, 'letter-spacing=".7"');
  let y = 212;
  for (const item of chapter.items) {
    body += circle(643, y - 5, 2.5, '#79dfc6', 'none');
    for (const line of wrap(item)) { body += text(655, y, line, 14, '#c5d5e3'); y += 18; }
    y += 10;
  }
  body += text(32, 489, 'Scripted Windows feature showcase · GIF is silent', 13, '#9db3c6');
  body += text(928, 489, 'Linux capabilities vary', 13, '#8ea7ba', 400, 'text-anchor="end"');
  body += text(32, 510, 'Illustrative animation. Open the settings guide for every control and range.', 11, '#7f98ad');
  return body;
}

function sample() {
  return text(60, 198, 'Presentation notes', 20, '#edf5fc', 600)
    + text(60, 241, 'Small details. Bigger clarity.', 17, '#c5d5e3')
    + text(60, 276, 'Make the important details stand out.', 17, '#c5d5e3')
    + text(60, 309, 'Your audience can follow every step.', 15, '#99b0c3')
    + label(60, 359, 150, 'Left click') + label(225, 359, 150, 'Right click') + label(390, 359, 175, 'Middle click');
}

function clickEffect(x, y, style, age, color) {
  if (age < 0 || age > .8) return '';
  const q = clamp(age / .8), expansion = 1 - (1 - q) ** 3;
  const r = 40 * (.28 + .72 * expansion), opacity = (1 - q) ** 2;
  let body = '';
  if (style === 'Pulse') body = circle(x, y, r, color, 'none', 0, 'fill-opacity=".65"');
  else if (style === 'Ripple') body = circle(x, y, r, 'none', color, 2.2) + circle(x, y, r * .65, 'none', color, 1.5);
  else if (style === 'Burst') {
    for (let i = 0; i < 10; i++) { const a = i * Math.PI / 5; body += `<path d="M${x + Math.cos(a) * r * .65} ${y + Math.sin(a) * r * .65}L${x + Math.cos(a) * r} ${y + Math.sin(a) * r}" stroke="${color}" stroke-width="2.5"/>`; }
  } else if (style === 'Neon') body = circle(x, y, r, 'none', color, 11, 'opacity=".09"') + circle(x, y, r, 'none', color, 6, 'opacity=".25"') + circle(x, y, r, 'none', color, 2.5);
  else if (style === 'Target') { const target = 42 * (1 - .5 * expansion); body = circle(x, y, target, 'none', color, 2) + `<path d="M${x-target-7} ${y}h14M${x+target-7} ${y}h14M${x} ${y-target-7}v14M${x} ${y+target-7}v14" stroke="${color}" stroke-width="2"/>`; }
  else body = circle(x, y, r, 'none', color, 2.5);
  return `<g opacity="${opacity}">${body}</g>`;
}

function highlight(x, y, style, t) {
  if (style === 'Filled') return circle(x, y, 30, '#ffd640', 'none', 0, 'fill-opacity=".3"');
  if (style === 'Glow') return circle(x, y, 37, '#79dfc6', 'none', 0, 'fill-opacity=".05"') + circle(x, y, 30, '#79dfc6', 'none', 0, 'fill-opacity=".08"') + circle(x, y, 22, '#79dfc6', 'none', 0, 'fill-opacity=".13"');
  if (style === 'Crosshair') return `<g stroke="#79dfc6" stroke-width="2"><path d="M${x-42} ${y}h28m28 0h28M${x} ${y-42}v28m0 28v28"/></g>` + circle(x, y, 7, 'none', '#79dfc6', 1.5);
  if (style === 'Spotlight') return `<path d="M32 155H598V433H32Z M${x-72} ${y}a72 72 0 1 0 144 0a72 72 0 1 0-144 0Z" fill="#000" opacity=".62" fill-rule="evenodd"/>`;
  return circle(x, y, 28, '#79dfc6', '#79dfc6', 2.3, `fill-opacity=".07" stroke-opacity="${.6 + .2 * Math.sin(t * 5)}"`);
}

function stage(chapter, t, progress) {
  let body = '', overlay = '', lens = null;
  const center = [315 + 110 * Math.sin(t * 1.8), 288 + 38 * Math.cos(t * 2)];
  if (chapter.id === 'cursor') {
    body += text(60, 193, 'A familiar arrow, tailored to your desktop.', 17, '#c5d5e3');
    const pressed = t % 1.5 > 1.08;
    const scale = 1.5 + .7 * (.5 - .5 * Math.cos(t * 1.4));
    const styles = [ ['Windows / press', 114, { fill: pressed ? '#000' : '#fff', outline: pressed ? '#fff' : '#000', scale }], ['Custom colors', 296, { fill: '#79dfc6', outline: '#0a181e', scale: 2.5, shadow: true }], ['Opacity + shadow', 480, { scale: 2.5, opacity: .5, shadow: true }] ];
    for (const [name, x, look] of styles) { body += rect(x-57, 219, 133, 145, '#1e3042', '#354d63', 9); overlay += cursor(x, 251, look); body += text(x + 8, 387, name, 13, '#b5cada', 500, 'text-anchor="middle"'); }
    body += label(60, 398, 120, pressed ? 'Pressed / dark' : 'Released / light', pressed) + label(192, 398, 170, 'Size + theme options') + label(374, 398, 192, 'Color + opacity + shadow');
  } else if (chapter.id === 'shake') {
    body += text(60, 197, 'Wiggle to find the pointer', 19, '#d5e4ee', 600);
    const grow = smooth((t-.55)/.8) * (1-smooth((t-2.4)/.75));
    const x = 287 + 34 * Math.sin(t * 24) * Math.sin(Math.PI * progress);
    overlay += cursor(x, 249, { scale: 1.5 + 3 * grow, shadow: true });
    body += label(60, 385, 238, 'Plasma-style preset', t < 1.75) + label(313, 385, 253, 'macOS-inspired preset', t >= 1.75);
  } else if (chapter.id === 'highlights') {
    body += sample();
    const styles = ['Filled', 'Ring', 'Glow', 'Crosshair', 'Spotlight'];
    const style = styles[Math.min(4, Math.floor(t / 1.1))];
    overlay += highlight(...center, style, t) + cursor(...center);
    body += label(391, 166, 183, style, true);
  } else if (chapter.id === 'trails') {
    const styles = ['Dots', 'Rings', 'Sparkles', 'Comet', 'Fine sparkle'];
    const index = Math.min(4, Math.floor(t / 1.1));
    body += text(60, 195, styles[index], 21, '#79dfc6', 600);
    const point = age => [310 + 190 * Math.sin((t-age) * 2), 297 + 60 * Math.sin((t-age) * 4)];
    for (let i = 24; i >= 1; i--) {
      const [x, y] = point(i * .024), opacity = (1-i/25) * .7;
      if (index === 0) overlay += circle(x, y, 4, '#79dfc6', 'none', 0, `opacity="${opacity}"`);
      else if (index === 1) overlay += circle(x, y, 5, 'none', '#79dfc6', 1.2, `opacity="${opacity}"`);
      else if (index === 3) overlay += circle(x, y, 7 * (1-i/25)+1, '#79dfc6', 'none', 0, `opacity="${opacity}"`);
      else { const s = index === 4 ? 2.3 : 4.5; overlay += `<path d="M${x-s} ${y}h${s*2}M${x} ${y-s}v${s*2}" stroke="#79dfc6" stroke-width="1.2" opacity="${opacity}"/>`; }
    }
    overlay += cursor(...point(0));
    body += label(60, 385, 506, 'Optional trails · choose color, size and fade', true);
  } else if (chapter.id === 'clicks') {
    const styles = ['Ring', 'Pulse', 'Ripple', 'Burst', 'Neon', 'Target'];
    for (let i = 0; i < styles.length; i++) {
      const x = 128 + i % 3 * 186, y = 216 + Math.floor(i/3)*117;
      const color = ['#00c8ff', '#ff4949', '#39ff14'][i%3];
      body += text(x, y + 65, styles[i], 14, '#c5d5e3', 500, 'text-anchor="middle"');
      overlay += clickEffect(x, y, styles[i], (t + i*.13) % 1.1, color);
    }
    body += text(315, 421, 'LEFT · BLUE       RIGHT · RED       MIDDLE · GREEN', 11, '#a4bacd', 600, 'text-anchor="middle"');
  } else if (chapter.id === 'magnifier') {
    body += sample();
    const p = [160 + 110 * smooth((t%2.5)/2), t < 2.5 ? 241 : 276];
    const beside = t >= 2.5;
    lens = { source: p, center: beside ? [p[0]+135, p[1]-8] : p, round: !beside, zoom: beside ? 3 : 2 };
    overlay += cursor(...p);
    body += label(60, 398, 506, beside ? 'Beside the pointer · rounded square · 3×' : 'Centered on the pointer · round · 2×', true);
  } else if (chapter.id === 'keys') {
    body += text(60, 198, 'Shortcut keycaps', 18, '#d3e3ef', 600);
    const k = Math.min(3, Math.floor(t/.7));
    const keys = ['Ctrl', 'Shift', 'S'];
    for (let i=0; i<k; i++) body += rect(78+i*104, 224, 86, 54, '#30475c', '#7191aa', 8) + text(121+i*104, 258, keys[i], 20, '#f0f7fc', 600, 'text-anchor="middle"');
    body += text(60, 315, 'Typed words and mouse buttons', 17, '#d3e3ef', 600);
    const word = 'Hello audience'.slice(0, Math.max(0, Math.floor((t-1.4)*9)));
    body += rect(78, 337, 255, 53, '#2b4054', '#5b7c96', 8) + text(96, 371, word, 20, '#f2f7fc', 500);
    if (t > 2.6) body += label(353, 348, 194, 'Left mouse button', true);
  } else if (chapter.id === 'sounds') {
    body += text(60, 198, 'Mouse clicks', 19, '#dcebf5', 600) + text(60, 302, 'Keyboard typing', 19, '#dcebf5', 600);
    for (let row=0; row<2; row++) for(let i=0;i<35;i++) {
      const h = 5 + 40 * Math.abs(Math.sin(i*.71+t*(row?9:6))) * (.3+.7*Math.abs(Math.sin(t*3+i*.09)));
      body += rect(65+i*13.5, 241+row*104-h/2, 6, h, row ? '#79dfc6' : '#00c8ff', 'none', 2);
    }
    body += label(60, 385, 506, 'Sound settings shown visually · this GIF is silent', true);
  } else if (chapter.id === 'rules') {
    const paused = t > 1.4 && t < 3.05;
    body += rect(60, 181, 506, 156, '#1e3042', '#3b5268', 9);
    body += text(80, 211, paused ? 'Fullscreen game / locked session' : 'Desktop app / presentation', 18, '#dcebf5', 600);
    body += label(80, 231, 176, paused ? 'Effects paused' : 'Effects active', !paused);
    if (!paused) overlay += highlight(407, 272, 'Ring', t);
    overlay += cursor(407, 272);
    body += label(60, 355, 245, 'App exclusions + allowances') + label(320, 355, 246, 'Administrator mode · optional');
    body += text(60, 410, 'Browsers, videos and presentations can keep their effects.', 13, '#a9c0d2');
  } else if (chapter.id === 'capture') {
    const visible = t < 2.2;
    for (const [x, title] of [[60,'Your screen'],[323,'Recording']]) {
      body += rect(x, 184, 243, 136, '#203244', '#3e5970', 8) + text(x+16, 211, title, 17, '#d5e5f0', 600);
      if (x===60 || visible) overlay += highlight(x+118, 263, 'Ring',t);
      overlay += cursor(x+118,263);
    }
    body += label(60, 338, 506, visible ? 'Show effects in screenshots and recordings' : 'Hide effects from screenshots and recordings', visible);
    body += text(60, 398, 'Crash recovery', 17, '#79dfc6', 600) + text(218, 398, 'Restore cursor → report → restart', 14, '#c3d5e4');
    body += text(60, 420, 'Recovery stops after three consecutive restarts.', 12, '#9bb3c7');
  } else if (chapter.id === 'settings') {
    const light = t > 1.9;
    body += rect(60, 181, 506, 219, light ? '#eaf0f6' : '#1e2b3b', '#58748b', 9);
    const ink = light ? '#243b50' : '#dbeaf4';
    body += rect(60, 181, 104, 219, light ? '#d7e3ed' : '#172332', 'none', 8);
    for (let i=0;i<5;i++) body += rect(76,207+i*33,69,9,i===2?'#79bfae':(light?'#8da5ba':'#425a71'),'none',3);
    body += text(185, 211, light ? 'Light palette' : 'Dark palette', 20, ink, 600);
    body += rect(185, 232, 355, 61, light ? '#fff' : '#263a4e', '#66849b', 7) + text(201,257,'Color, scale and per-control resets',15,ink);
    body += rect(201, 274, 235, 3, '#79dfc6') + circle(365+20*Math.sin(t*2),275,5,'#79dfc6','none');
    body += rect(185, 309, 355, 61, light ? '#fff' : '#263a4e', '#66849b', 7) + text(201,344,'Interface zoom + custom palettes',15,ink);
    for(let i=0;i<32;i++) body += rect(61+i*15.7,390,16,3,`hsl(${(i*11+t*40)%360},85%,65%)`);
    body += text(60, 422, 'Illustrative settings layout', 12, '#9bb3c7');
  } else if (chapter.id === 'everyday') {
    body += text(60, 197, 'Profiles for different workflows', 19, '#d5e5f0', 600);
    for (let i=0;i<3;i++) body += label(60+i*171,216,158,['Everyday','Presenting','Recording'][i],Math.floor(t)%3===i);
    const labels = ['Tray menu', '9 shortcuts', 'Start with Windows', 'Backup + import'];
    for(let i=0;i<4;i++) body += rect(60+i%2*261,268+Math.floor(i/2)*66,245,51,'#203244','#3c586f',8)+text(78+i%2*261,300+Math.floor(i/2)*66,labels[i],16,'#dcebf5',500);
    body += text(60, 421, 'Local settings. No account. No telemetry.', 14, '#79dfc6', 600);
  }
  return { body, overlay, lens };
}

const masks = new Map();
for (const round of [true, false]) masks.set(round, await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="180" height="180">${round ? circle(90,90,87,'white','none') : rect(3,3,174,174,'white','none',17)}</svg>`)).png().toBuffer());
const coverage = chapters.map(({ id, start, duration, items }) => ({ id, start, duration, features: items }));
await fs.writeFile(path.join(work,'chapters.json'),JSON.stringify(coverage,null,2));
let frame = 0;
for (let index=0; index<chapters.length; index++) {
  const chapter=chapters[index], total=Math.round(chapter.duration*fps);
  for (let f=0; f<total; f++,frame++) {
    const t=f/fps, progress=f/total;
    const {body,overlay,lens}=stage(chapter,t,progress);
    let baseBody=shell(chapter,index)+`<defs><clipPath id="stage"><rect x="33" y="156" width="564" height="276" rx="11"/></clipPath></defs><g clip-path="url(#stage)">${body}</g>`;
    const segment=(896-(chapters.length-1)*6)/chapters.length;
    for(let i=0;i<chapters.length;i++) {
      const x=32+i*(segment+6);
      baseBody+=rect(x,453,segment,4,i<index?'#527d78':'#2b4054','none',2);
      if(i===index) baseBody+=rect(x,453,Math.max(1,segment*progress),4,'#79dfc6','none',2);
    }
    const base=await sharp(svg(baseBody)).png().toBuffer();
    const layers=[];
    if(lens) {
      const size=180, cropSize=Math.round(size/lens.zoom);
      const crop=await sharp(base).extract({left:Math.round(lens.source[0]-cropSize/2),top:Math.round(lens.source[1]-cropSize/2),width:cropSize,height:cropSize}).resize(size,size).ensureAlpha().composite([{input:masks.get(lens.round),blend:'dest-in'}]).png().toBuffer();
      layers.push({input:crop,left:Math.round(lens.center[0]-size/2),top:Math.round(lens.center[1]-size/2)});
      const border=lens.round?circle(...lens.center,89,'none','#79dfc6',3):rect(lens.center[0]-89,lens.center[1]-89,178,178,'none','#79dfc6',19,'stroke-width="3"');
      layers.push({input:svg(border),left:0,top:0});
    }
    layers.push({input:svg(`<defs><clipPath id="stage"><rect x="33" y="156" width="564" height="276" rx="11"/></clipPath></defs><g clip-path="url(#stage)">${overlay}</g>`),left:0,top:0});
    await sharp(base).composite(layers).png().toFile(path.join(work,`frame-${String(frame).padStart(4,'0')}.png`));
  }
  console.log(`Rendered ${index+1}/${chapters.length}: ${chapter.id}`);
}

function ffmpeg(params) {
  const result=spawnSync('ffmpeg',['-hide_banner','-loglevel','error','-y',...params],{stdio:'inherit'});
  if(result.error) throw result.error;
  if(result.status!==0) throw new Error(`ffmpeg exited with ${result.status}`);
}
const sequence=path.join(work,'frame-%04d.png'),palette=path.join(work,'palette.png');
// Trimming avoids frames left over from an older, longer scratch render.
ffmpeg(['-framerate',String(fps),'-i',sequence,'-vf',`trim=end_frame=${frame},palettegen=max_colors=192:stats_mode=diff:reserve_transparent=0`,'-frames:v','1',palette]);
ffmpeg(['-framerate',String(fps),'-i',sequence,'-i',palette,'-lavfi','paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle','-frames:v',String(frame),'-loop','0',output]);
console.log(`Created ${output}: ${duration}s, ${fps}fps, ${width}×${height}, ${(await fs.stat(output)).size.toLocaleString()} bytes, ${chapters.length} chapters.`);
