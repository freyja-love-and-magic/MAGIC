// Renders every sound effect offline (no speakers) in a real browser and checks each is audible, not clipping, and
// short. It cannot tell you they SOUND good, only that none is silent, broken, or too quiet for a stream.
// Needs Playwright: npm i --no-save playwright && npx playwright install chromium
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

let chromium;
try { ({ chromium } = await import('playwright')); } catch { console.error('Playwright is not installed: npm i --no-save playwright && npx playwright install chromium'); process.exit(2); }

const pub = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'public');
const types = { '.js': 'text/javascript', '.html': 'text/html', '.css': 'text/css' };
const server = http.createServer((req, res) => {
  const file = path.join(pub, req.url.split('?')[0] === '/' ? 'scene.html' : req.url.split('?')[0]);
  if (!file.startsWith(pub) || !fs.existsSync(file)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
  res.end(fs.readFileSync(file));
}).listen(0);
const port = server.address().port;

const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto(`http://localhost:${port}/?local=1&mute=1`);
const result = await page.evaluate(async () => {
  const { createSfx } = await import('/js/sfx.js');
  const out = {};
  for (const name of ['swing', 'hit', 'crit', 'fire', 'thunder', 'meteor', 'cast', 'heal', 'revive', 'ko', 'haste', 'viewer']) {
    let ctx;
    class Offline extends OfflineAudioContext { constructor() { super(1, 44100 * 3, 44100); ctx = this; } resume() { return Promise.resolve(); } }
    Object.defineProperty(Offline.prototype, 'state', { get: () => 'running' });
    globalThis.AudioContext = Offline;
    createSfx()[name]();
    const data = (await ctx.startRendering()).getChannelData(0);
    let peak = 0, last = 0;
    for (let i = 0; i < data.length; i++) { const a = Math.abs(data[i]); if (a > peak) peak = a; if (a > 0.02) last = i; }
    out[name] = { peak: +peak.toFixed(3), audibleSeconds: +(last / 44100).toFixed(2) };
  }
  return out;
});
await browser.close();
server.close();

let bad = 0;
for (const [name, r] of Object.entries(result)) {
  const fine = r.peak >= 0.12 && r.peak <= 0.95 && r.audibleSeconds >= 0.06 && r.audibleSeconds < 2.5;
  if (!fine) bad++;
  console.log(fine ? 'ok  ' : 'FAIL', name.padEnd(8), JSON.stringify(r));
}
console.log(bad ? `\n${bad} sounds look wrong` : '\nall sounds render audibly, below clipping, and end within 2.5s');
process.exit(bad ? 1 : 0);
