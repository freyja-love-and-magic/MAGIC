// Test plumbing: a real fount (run from a throwaway copy, so your fount data/ is untouched), real signed casts.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Identity, signTimestamped } from 'sessionless-kit';
import { createArena } from '../src/arena.js';
import { seededRng } from '../public/js/engine.js';
import { SPELLS } from '../public/js/spells.js';

const here = path.dirname(fileURLToPath(import.meta.url));
export const PUBLIC_DIR = path.join(here, '..', 'public');
export const FOUNT_PORT = 3006;  // hardcoded in fount.js
export const FOUNT_BASE = `http://127.0.0.1:${FOUNT_PORT}`;

export const startFount = async () => {
  const src = process.env.FOUNT_DIR || path.join(here, '..', '..', '..', '..', '..', 'fount', 'src', 'server', 'node');
  if (!fs.existsSync(path.join(src, 'fount.js'))) throw new Error(`fount not found at ${src} (set FOUNT_DIR)`);
  const busy = await fetch(`${FOUNT_BASE}/api/ping`).then(() => true, () => false);
  if (busy) throw new Error(`Something is already listening on ${FOUNT_PORT}. Stop it first; these tests start their own fount.`);

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'arena-fount-'));
  fs.cpSync(src, tmp, { recursive: true, filter: (p) => { const rel = path.relative(src, p); return !/^(node_modules|data)(\/|$)/.test(rel); } });
  fs.symlinkSync(path.join(src, 'node_modules'), path.join(tmp, 'node_modules'));
  const child = spawn('node', ['fount.js'], { cwd: tmp, env: { ...process.env, LOCALHOST: '1' }, stdio: 'ignore' });
  for (let i = 0; i < 60; i++) {
    if (await fetch(`${FOUNT_BASE}/api/ping`).then((r) => r.ok, () => false)) break;
    if (i === 59) { child.kill(); throw new Error('fount did not start'); }
    await new Promise((r) => setTimeout(r, 250));
  }
  return { stop: () => { child.kill(); fs.rmSync(tmp, { recursive: true, force: true }); } };
};

export const registerWithFount = async (id) => {
  const timestamp = Date.now().toString();
  const r = await fetch(`${FOUNT_BASE}/user/create`, {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ timestamp, pubKey: id.pubKey, signature: await id.sign(timestamp + id.pubKey) })
  });
  return r.json();
};

// An in-process arena on a random port, with a clock the test can push forward (cooldowns) without sleeping.
export const startArena = async ({ seed = 1 } = {}) => {
  const identity = await Identity.ephemeral();
  const reg = await registerWithFount(identity);
  const fountUser = { uuid: reg.uuid, ordinal: 0 };
  let offset = 0;
  const arena = createArena({
    config: { FOUNT_BASE, PORT: 0, STOP_NAME: 'arena', PUBLIC_DIR, PUBLIC_URL: 'http://example.test' },
    identity, fountUser, rng: seededRng(seed), now: () => Date.now() + offset
  });
  const server = await new Promise((resolve) => { const s = arena.app.listen(0, '127.0.0.1', () => resolve(s)); });
  const base = `http://127.0.0.1:${server.address().port}`;
  return {
    ...arena, base, fountUser, identity,
    advanceClock: (ms) => { offset += ms; },
    stop: () => { for (const c of arena.sseClients) c.end(); server.close(); }
  };
};

export const makeViewer = async (arena) => {
  const id = await Identity.ephemeral();
  const timestamp = Date.now().toString();
  const r = await fetch(`${arena.base}/register`, {
    method: 'PUT', headers: { 'Content-Type': 'application/json', 'X-Join-Code': arena.joinCode },
    body: JSON.stringify({ timestamp, pubKey: id.pubKey, signature: await id.sign(timestamp + id.pubKey) })
  });
  const body = await r.json();
  if (!body.uuid) throw new Error('viewer registration failed: ' + JSON.stringify(body));
  return { id, uuid: body.uuid, ordinal: Date.now() };
};

// Builds a spell exactly the way the cast page does. `override` lets a test cheat (wrong cost, forged signer...).
export const buildSpell = async (viewer, name, override = {}) => {
  const cost = override.totalCost ?? SPELLS[name].cost;
  const ordinal = override.ordinal ?? ++viewer.ordinal;
  const signer = override.signer ?? viewer.id;
  const signed = await signTimestamped(signer, name, viewer.uuid, cost, true, ordinal);
  const spell = { timestamp: signed.timestamp, spell: name, casterUUID: viewer.uuid, totalCost: cost, mp: true, ordinal, casterSignature: signed.signature, gateways: [] };
  return { ...spell, ...(override.fields ?? {}) };
};

export const cast = async (arena, spell, name = spell.spell) => {
  const r = await fetch(`${arena.base}/magic/spell/${name}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Join-Code': arena.joinCode }, body: JSON.stringify(spell) });
  return { status: r.status, body: await r.json() };
};

export const mpOf = async (arena, viewer) => {
  const timestamp = Date.now().toString();
  const sig = await viewer.id.sign(timestamp + viewer.uuid);
  const r = await fetch(`${arena.base}/mp?uuid=${viewer.uuid}&timestamp=${timestamp}&signature=${sig}`, { headers: { 'X-Join-Code': arena.joinCode } });
  return { status: r.status, body: await r.json() };
};

// Collects server-sent events until stopped.
export const watchEvents = async (arena) => {
  const ctrl = new AbortController();
  const res = await fetch(`${arena.base}/events`, { signal: ctrl.signal });
  const received = [];
  const decoder = new TextDecoder();
  let buf = '';
  (async () => {
    try {
      for await (const chunk of res.body) {
        buf += decoder.decode(chunk, { stream: true });
        let i;
        while ((i = buf.indexOf('\n\n')) >= 0) {
          const block = buf.slice(0, i); buf = buf.slice(i + 2);
          const ev = /^event: (.+)$/m.exec(block), data = /^data: (.+)$/m.exec(block);
          if (ev && data) received.push({ event: ev[1], data: JSON.parse(data[1]) });
        }
      }
    } catch { /* aborted */ }
  })();
  const wait = async (pred, ms = 2000) => {
    const t = Date.now();
    while (Date.now() - t < ms) { const hit = received.find(pred); if (hit) return hit; await new Promise((r) => setTimeout(r, 20)); }
    return null;
  };
  return { received, wait, stop: () => ctrl.abort(), events: () => received.filter((e) => e.event === 'events').flatMap((e) => e.data) };
};
