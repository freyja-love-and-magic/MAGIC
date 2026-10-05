// The whole thing, for real: a live fount, the real server.js, a scene page in one browser and a phone-sized viewer
// page in another. Viewers cast through the actual gateway and the scene reacts.
//
// Needs Playwright (not a dependency, it downloads a browser):  npm i --no-save playwright && npx playwright install chromium
//   node test/e2e/live.mjs            add SHOTS=/some/dir to save screenshots
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { startFount } from '../helpers.js';

let chromium;
try { ({ chromium } = await import('playwright')); } catch { console.error('Playwright is not installed: npm i --no-save playwright && npx playwright install chromium'); process.exit(2); }

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..', '..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arena-live-'));
const PORT = 3377, BASE = `http://localhost:${PORT}`;
const shots = process.env.SHOTS;
if (shots) fs.mkdirSync(shots, { recursive: true });
const shot = async (page, name) => { if (shots) await page.screenshot({ path: path.join(shots, `${name}.png`) }); };

let failures = 0;
const ok = (cond, msg) => { if (!cond) { failures++; console.log('FAIL:', msg); } else console.log('ok  :', msg); };

const fount = await startFount();
fs.writeFileSync(path.join(tmp, 'config.js'), `export default { FOUNT_BASE: 'http://localhost:3006', PORT: ${PORT}, PUBLIC_URL: '${BASE}', DATA_DIR: ${JSON.stringify(path.join(tmp, 'data'))} };`);
await import('node:child_process').then(({ execFileSync }) => execFileSync('npm', ['run', 'build'], { cwd: root, stdio: 'ignore' }));
const server = spawn('node', ['server.js'], { cwd: root, env: { ...process.env, CONFIG: path.join(tmp, 'config.js') }, stdio: ['ignore', 'pipe', 'pipe'] });
let log = '';
server.stdout.on('data', (d) => { log += d; }); server.stderr.on('data', (d) => { log += d; });
const cleanup = async (browser) => { await browser?.close(); server.kill(); fount.stop(); fs.rmSync(tmp, { recursive: true, force: true }); };

let browser;
try {
  for (let i = 0; i < 60 && !log.includes('is running'); i++) await new Promise((r) => setTimeout(r, 200));
  assert.ok(log.includes('is running'), `server did not start:\n${log}`);

  browser = await chromium.launch();
  const errors = [];
  const track = (page, label) => { page.on('pageerror', (e) => errors.push(`${label}: ${e.message}`)); page.on('console', (m) => { if (m.type() === 'error') errors.push(`${label}: ${m.text()}`); }); };

  // the stream
  const scene = await (await browser.newContext({ viewport: { width: 1280, height: 720 } })).newPage();
  track(scene, 'scene');
  await scene.goto(`${BASE}/?mute=1`);
  await scene.waitForSelector('#party .row', { timeout: 5000 });
  ok((await scene.locator('#party .row').count()) === 4, 'scene shows the four heroes from the server snapshot');
  ok((await scene.locator('.unit').count()) === 7, 'scene shows all seven units');
  ok((await scene.locator('#qr').count()) === 0, 'there is no QR code any more');
  await scene.waitForFunction(() => /^[A-Z2-9]{4}$/.test(document.getElementById('join-code').textContent), null, { timeout: 5000 });
  const code = (await scene.locator('#join-code').textContent()).trim();
  ok(/^[A-HJKMNP-Z2-9]{4}$/.test(code), `the scene shows a short join code: ${code}`);
  ok((await scene.locator('#cast-url').textContent()).includes(`localhost:${PORT}/cast`), 'the scene shows the address to go to');
  ok(log.includes(`JOIN CODE (shown on the stream): ${code}`), 'the terminal prints the same code');
  await shot(scene, '1-scene-live');

  // a viewer on a phone
  const phone = await (await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true })).newPage();
  track(phone, 'phone');
  await phone.goto(`${BASE}/cast`);
  await phone.waitForSelector('#code', { state: 'visible', timeout: 5000 });
  ok(await phone.locator('#game').isHidden(), 'a new viewer sees only the join screen, not the spells');
  await shot(phone, '2a-phone-join');
  await phone.fill('#code', 'ZZZZ');
  await phone.click('#join-go');
  await phone.waitForFunction(() => /isn't the code/.test(document.getElementById('join-error').textContent), null, { timeout: 5000 });
  ok(await phone.locator('#game').isHidden(), 'a wrong code is refused and stays on the join screen');
  await shot(phone, '2b-phone-wrong-code');
  await phone.fill('#code', `${code.slice(0, 2).toLowerCase()} ${code.slice(2).toLowerCase()}`);   // typed sloppily
  await phone.click('#join-go');
  await phone.waitForFunction(() => /You are/.test(document.getElementById('who').textContent), null, { timeout: 8000 });
  ok(await phone.locator('#join').isHidden(), 'the right code (lowercase, with a space) gets in');
  const name = await phone.locator('#who b').textContent();
  ok(/^[A-Z][a-z]+ [A-Z][a-z]+$/.test(name), `the viewer got a friendly name: ${name}`);
  await phone.waitForFunction(() => /1000/.test(document.getElementById('mp-text').textContent), null, { timeout: 5000 });
  ok(true, 'the viewer starts with 1000 MP');
  await shot(phone, '2-phone-ready');

  const bossBefore = Number(await scene.locator('#boss-hp').textContent());
  await phone.getByRole('button', { name: /^Fire/ }).click();
  await phone.waitForFunction(() => /Fire!/.test(document.getElementById('status').textContent), null, { timeout: 5000 });
  ok(true, 'the phone reports the spell landed');
  await scene.waitForFunction((b) => Number(document.getElementById('boss-hp').textContent) < b, bossBefore, { timeout: 4000 });
  const bossAfter = Number(await scene.locator('#boss-hp').textContent());
  ok(bossAfter < bossBefore, `the dragon lost HP on stream (${bossBefore} -> ${bossAfter})`);
  await scene.waitForFunction((n) => document.getElementById('ticker').textContent.includes(n + ' cast Fire'), name, { timeout: 3000 });
  ok(true, `the stream credits ${name}: "cast Fire"`);
  await shot(scene, '3-scene-after-fire');
  await shot(phone, '4-phone-after-fire');
  ok(await phone.getByRole('button', { name: /^Fire/ }).isDisabled(), 'Fire shows as recharging on the phone');
  await phone.waitForFunction(() => /9[0-9]{2}/.test(document.getElementById('mp-text').textContent), null, { timeout: 5000 });
  ok(/950/.test(await phone.locator('#mp-text').textContent()), 'the phone shows 950 MP after a 50 MP spell');

  // the same viewer coming back: same keys, same name, MP remembered by fount
  await phone.reload();
  await phone.waitForFunction(() => /You are/.test(document.getElementById('who').textContent), null, { timeout: 8000 });
  ok((await phone.locator('#who b').textContent()) === name, 'reloading keeps the same viewer (keys live in the browser)');
  await phone.waitForFunction(() => /950/.test(document.getElementById('mp-text').textContent), null, { timeout: 5000 });
  ok(true, 'and their MP carried over');

  // a different viewer on another device, arriving by the one-tap link someone posted in chat
  const other = await (await browser.newContext({ viewport: { width: 390, height: 844 } })).newPage();
  track(other, 'other');
  await other.goto(`${BASE}/cast?code=${code}`);
  await other.waitForFunction(() => /You are/.test(document.getElementById('who').textContent), null, { timeout: 8000 });
  ok(await other.locator('#join').isHidden(), 'the chat link joins without typing');
  ok(!other.url().includes('code='), 'and the code is removed from the address bar');
  ok((await other.locator('#who b').textContent()) !== name, 'a second device is a different viewer');

  // the host restarts with a new code: the viewer's stored one stops working and they are asked again, politely
  await other.evaluate(() => localStorage.setItem('endless-battle:code', 'QQQQ'));
  await other.reload();
  await other.waitForSelector('#code', { state: 'visible', timeout: 5000 });
  ok(/no longer works/.test(await other.locator('#join-error').textContent()), 'a stale code sends the viewer back to the join screen with an explanation');
  await other.fill('#code', code);
  await other.click('#join-go');
  await other.waitForFunction(() => /You are/.test(document.getElementById('who').textContent), null, { timeout: 8000 });
  ok(true, 'and entering the current code gets them back in');

  // a spell that cannot land says so, and costs nothing
  await other.getByRole('button', { name: /^Revive/ }).click();
  await other.waitForFunction(() => /fallen|good health|already/.test(document.getElementById('status').textContent) || /fallen/.test(document.getElementById('status').textContent), null, { timeout: 5000 }).catch(() => {});
  const msg = await other.locator('#status').textContent();
  ok(/fallen|Revive!/.test(msg), `revive is refused when nobody is down, or lands when someone is: "${msg}"`);

  // meteor, the big one
  await other.getByRole('button', { name: /^Meteor/ }).click();
  await other.waitForFunction(() => /Meteor!/.test(document.getElementById('status').textContent), null, { timeout: 5000 });
  await scene.waitForTimeout(1000);
  await shot(scene, '5-scene-meteor');
  ok(true, 'meteor cast from the second phone');

  // The 409 is the refused Revive above (nobody had fallen) and the 403s are the wrong codes: the browser logs every
  // non-2xx fetch, expected or not.
  const real = errors.filter((e) => !/favicon|status of (409|403)/.test(e));
  ok(real.length === 0, `no console or page errors in any browser ${real.length ? JSON.stringify(real) : ''}`);
} catch (err) {
  failures++;
  console.log('FAIL:', err.message);
  console.log('--- server log ---\n' + log);
} finally {
  await cleanup(browser);
}
console.log(failures ? `\n${failures} FAILED` : '\nLIVE E2E OK');
process.exit(failures ? 1 : 0);
