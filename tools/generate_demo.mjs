#!/usr/bin/env node
// Regenerate the illustrative README GIF without launching or controlling an app.
// Requires Node.js, sharp and ffmpeg. Run from the repository root:
//   node tools/generate_demo.mjs [--work-dir build/demo] [--output assets/demo.gif]
import { createRequire } from 'node:module';
import fs from 'node:fs/promises';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const sharp = createRequire(import.meta.url)('sharp');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const work = path.resolve(args.get('--work-dir') || 'build/demo');
const output = path.resolve(args.get('--output') || 'assets/demo.gif');
const width = 960, height = 520, fps = 20, seconds = 12;
await fs.mkdir(work, { recursive: true });
await fs.mkdir(path.dirname(output), { recursive: true });

const svg = body => Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">${body}</svg>`);
const escape = s => s.replaceAll('&', '&amp;').replaceAll('<', '&lt;');
const text = (x, y, value, size, color = '#edf5fc', weight = 400, extra = '') => `<text x="${x}" y="${y}" font-family="Segoe UI,DejaVu Sans,sans-serif" font-size="${size}" font-weight="${weight}" fill="${color}" ${extra}>${escape(value)}</text>`;
const rect = (x, y, w, h, fill, stroke = 'none', r = 0) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}" stroke="${stroke}"/>`;

function scene(magnifier) {
  let body = rect(0, 0, width, height, '#0d1520');
  body += rect(32, 26, 5, 19, '#79dfc6', 'none', 2);
  body += text(48, 41, 'CLICKSHADE / ANIMATED PREVIEW', 13, '#79dfc6', 700, 'letter-spacing="1.2"');
  body += text(928, 41, magnifier ? '02 / LIVE MAGNIFIER' : '01 / CURSOR FEEDBACK', 12, '#9aafc1', 600, 'text-anchor="end"');
  body += text(32, 96, magnifier ? 'Bring small details into focus.' : 'Make every click easy to follow.', 32, '#f0f6fc', 700);
  body += text(32, 127, magnifier ? 'A 2× lens follows the pointer and enlarges the content beneath it.' : 'A visible pointer, a subtle highlight and distinct feedback for each click.', 17, '#afc2d3');
  body += rect(32, 160, 896, 276, '#162231', '#31465a', 14);
  body += text(66, 199, 'Presentation notes', 20, '#edf5fc', 600);
  body += text(66, 242, 'Small details. Bigger clarity.', 17, '#c6d5e3');
  body += text(66, 277, 'Bring the important details into focus.', 17, '#c6d5e3');
  body += text(66, 310, 'Your audience can follow every step.', 15, '#93aabd');
  body += '<path d="M638 190v132" stroke="#304254"/>';
  body += text(671, 215, magnifier ? '2×' : '01', 38, '#79dfc6', 700);
  body += text(671, 246, magnifier ? 'Read the fine print' : 'Follow the pointer', 16, '#edf5fc', 600);
  body += text(671, 275, magnifier ? 'Highlights stay out' : 'Highlight, click, repeat.', 13, '#9fb5c7');
  body += text(671, 296, magnifier ? 'of the zoomed content.' : 'Keep attention in view.', 13, '#9fb5c7');
  for (const [x, name, subtitle, color] of [[92, 'Left click', 'Blue feedback', '#00c8ff'], [368, 'Right click', 'Red feedback', '#ff4949'], [644, 'Move & follow', 'Cursor highlight', '#79dfc6']]) {
    body += rect(x, 350, 220, 60, '#203244', '#3b5268', 9);
    body += `<circle cx="${x + 22}" cy="380" r="5" fill="${color}"/>`;
    body += text(x + 40, 376, name, 15, '#edf5fc', 600);
    body += text(x + 40, 396, subtitle, 12, '#a9bed0');
  }
  body += text(32, 474, magnifier ? 'LIVE MAGNIFIER · 2× ZOOM' : 'HIGHLIGHTS · LEFT & RIGHT CLICK EFFECTS', 13, '#79dfc6', 600);
  body += text(928, 474, 'Scripted feature preview', 13, '#8ca4b8', 400, 'text-anchor="end"');
  body += text(32, 500, 'Illustrative animation. Effects and colors are configurable in ClickShade.', 12, '#8199ad');
  return sharp(svg(body)).png().toBuffer();
}

const bases = [await scene(false), await scene(true)];
const keyframes = [
  [0, 755, 380], [.65, 755, 380], [1.55, 202, 380], [2.3, 202, 380],
  [2.95, 478, 380], [3.7, 478, 380], [4.45, 755, 380], [5.15, 755, 380],
  [5.7, 150, 240], [6.15, 150, 240], [7.2, 255, 240], [7.65, 255, 240],
  [8.3, 285, 276], [9.25, 175, 276], [9.8, 175, 276], [11.1, 755, 380], [12, 755, 380]
];
const clicks = [[1.75, 202, 380, '#00c8ff'], [3.15, 478, 380, '#ff4949'], [4.65, 755, 380, '#00c8ff']];
const smooth = x => x * x * (3 - 2 * x);
const clamp = x => Math.max(0, Math.min(1, x));
function position(t) {
  for (let i = 1; i < keyframes.length; i++) {
    const [b, bx, by] = keyframes[i];
    if (t <= b) {
      const [a, ax, ay] = keyframes[i - 1];
      const q = smooth(clamp((t - a) / (b - a)));
      return [ax + (bx - ax) * q, ay + (by - ay) * q];
    }
  }
  return keyframes.at(-1).slice(1);
}

// This crop comes from the clean scene, before highlights, rings or cursor are
// composited. It illustrates the application's exclusion of its own overlays.
const lensSize = 190;
const lensMask = await sharp(Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${lensSize}" height="${lensSize}"><circle cx="95" cy="95" r="92" fill="white"/></svg>`)).png().toBuffer();

for (let frame = 0; frame < seconds * fps; frame++) {
  const t = frame / fps;
  const lensAmount = smooth(clamp((t - 5.6) / .3)) * (1 - smooth(clamp((t - 10.2) / .5)));
  const showLens = lensAmount > 0;
  const [x, y] = position(t);
  const base = bases[t >= 5.5 && t < 10.8 ? 1 : 0];
  let effects = `<circle cx="${x}" cy="${y}" r="27" fill="#79dfc6" fill-opacity=".1" stroke="#79dfc6" stroke-opacity=".8" stroke-width="2.5"/>`;
  let pressed = false;
  for (const [start, cx, cy, color] of clicks) {
    const age = t - start;
    if (age < 0 || age > .9) continue;
    pressed ||= age < .2;
    const q = age / .9, remaining = 1 - q;
    // Same cubic expansion / squared fade curve used by DrawParticle.
    const radius = 54 * (.28 + .72 * (1 - remaining ** 3));
    const opacity = remaining ** 2;
    effects += `<g fill="none" stroke="${color}" opacity="${opacity}"><circle cx="${cx}" cy="${cy}" r="${radius}" stroke-width="12" opacity=".07"/><circle cx="${cx}" cy="${cy}" r="${radius}" stroke-width="7" opacity=".22"/><circle cx="${cx}" cy="${cy}" r="${radius}" stroke-width="2.8"/></g>`;
  }
  const layers = [{ input: svg(effects), left: 0, top: 0 }];
  if (showLens) {
    const left = Math.round(x - lensSize / 2), top = Math.round(y - lensSize / 2);
    const crop = await sharp(base).extract({ left: Math.round(x - lensSize / 4), top: Math.round(y - lensSize / 4), width: lensSize / 2, height: lensSize / 2 }).resize(lensSize, lensSize, { kernel: 'lanczos3' }).ensureAlpha().composite([{ input: lensMask, blend: 'dest-in' }]).raw().toBuffer();
    for (let i = 3; i < crop.length; i += 4) crop[i] = Math.round(crop[i] * lensAmount);
    layers.push({ input: crop, raw: { width: lensSize, height: lensSize, channels: 4 }, left, top });
    layers.push({ input: svg(`<g opacity="${lensAmount}"><circle cx="${x}" cy="${y}" r="94" fill="none" stroke="#08111b" stroke-width="9"/><circle cx="${x}" cy="${y}" r="94" fill="none" stroke="#79dfc6" stroke-width="3"/></g>`), left: 0, top: 0 });
  }
  const cursor = `<g transform="translate(${x} ${y})"><path d="M0 0 3 30 10 23 17 35 24 31 17 19 29 17Z" fill="${pressed ? '#182633' : '#f2f8fd'}" stroke="${pressed ? '#e0f4ff' : '#08131f'}" stroke-width="2.2" stroke-linejoin="round"/></g>`;
  layers.push({ input: svg(cursor), left: 0, top: 0 });
  await sharp(base).composite(layers).png().toFile(path.join(work, `frame-${String(frame).padStart(4, '0')}.png`));
}

function ffmpeg(params) {
  const result = spawnSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...params], { stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`ffmpeg exited with ${result.status}`);
}
const sequence = path.join(work, 'frame-%04d.png');
const palette = path.join(work, 'palette.png');
ffmpeg(['-framerate', String(fps), '-i', sequence, '-vf', 'palettegen=max_colors=192:stats_mode=diff:reserve_transparent=0', '-frames:v', '1', palette]);
ffmpeg(['-framerate', String(fps), '-i', sequence, '-i', palette, '-lavfi', 'paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle', '-loop', '0', output]);
console.log(`Created ${output}: ${seconds}s, ${fps}fps, ${width}×${height}, ${(await fs.stat(output)).size.toLocaleString()} bytes.`);
