// The arena: the battle, the viewer-facing MAGIC gateway, and the SSE feed the scene watches.
// Built as a factory so tests can run it in-process against a real fount without timers or fixed ports.
import crypto from 'node:crypto';
import express from 'express';
import gateway from 'magic-gateway-js';
import { createBattle } from '../public/js/engine.js';
import { SPELLS } from '../public/js/spells.js';
import { viewerName } from '../public/js/names.js';

const SAFE_ID = /^[A-Za-z0-9._:-]{1,64}$/;
const HEX = (n) => new RegExp(`^[0-9a-f]{${n}}$`);
const ALLOWED_KEYS = new Set(['timestamp', 'spell', 'casterUUID', 'totalCost', 'mp', 'ordinal', 'casterSignature', 'gateways']);
const MAX_SKEW_MS = 60_000;   // fount allows 5 minutes; a live stream has no reason to be that generous
const MAX_CASTERS_REMEMBERED = 10_000;

// The join code shown on stream. No 0/O or 1/I/L, so it can be read off a screen and typed without guessing.
const CODE_ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
const CODE_LENGTH = 4;
const makeCode = () => Array.from({ length: CODE_LENGTH }, () => CODE_ALPHABET[crypto.randomInt(CODE_ALPHABET.length)]).join('');
const normalizeCode = (s) => String(s ?? '').toUpperCase().replace(/[\s-]/g, '');   // "k7 qm" and "K7-QM" both work
const sameCode = (a, b) => a.length === b.length && crypto.timingSafeEqual(Buffer.from(a), Buffer.from(b));
const MAX_CODE_FAILURES = 10;       // wrong guesses per IP per minute before it is told to wait

export const createArena = ({ config, identity, fountUser, rng = Math.random, now = Date.now }) => {
  const battle = createBattle({ rng });
  const sseClients = new Set();
  const cooldownUntil = {};          // spell name -> ms timestamp
  const lastOrdinal = new Map();     // casterUUID -> highest ordinal accepted (fount does not enforce this, so we do)
  const registerHits = new Map();    // ip -> [timestamps], a small brake on anonymous registrations

  const publicUrl = config.PUBLIC_URL || `http://localhost:${config.PORT}`;
  const castUrl = `${publicUrl.replace(/\/$/, '')}/cast`;

  const joinCode = config.JOIN_CODE ? normalizeCode(config.JOIN_CODE) : makeCode();
  if (!/^[A-Z0-9]{4,8}$/.test(joinCode)) throw new Error('JOIN_CODE must be 4 to 8 letters or digits');
  const codeFailures = new Map();    // ip -> timestamps of recent wrong codes

  // ---------------- SSE ----------------
  const send = (client, event, data) => client.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`);
  const broadcast = (event, data) => { for (const c of sseClients) send(c, event, data); };

  // ---------------- time ----------------
  let snapshotClock = 0;
  const tick = (dtMs) => {
    const events = battle.step(dtMs);
    if (events.length) broadcast('events', events);
    snapshotClock += dtMs;
    if (snapshotClock >= 500) { snapshotClock = 0; broadcast('snapshot', battle.snapshot()); }  // gauges and MP, not animated
    return events;
  };

  // ---------------- gateway ----------------
  const spellbook = Object.fromEntries(Object.keys(SPELLS).map((name) => [name, {
    cost: SPELLS[name].cost,
    mp: true,
    resolver: 'fount',
    destinations: [
      { stopName: config.STOP_NAME, stopURL: `http://localhost:${config.PORT}/magic/spell/` },
      { stopName: 'fount', stopURL: `${config.FOUNT_BASE}/resolve/` }
    ]
  }]));

  const reject = (res, status, error, extra = {}) => res.status(status).send({ success: false, error, ...extra });

  // Everything a viewer does needs the code from the stream. It keeps casting to people who are watching, and the
  // per-IP brake stops anyone guessing it (4 characters from 31 is about 920,000 possibilities).
  const recentFailures = (ip) => (codeFailures.get(ip) || []).filter((t) => now() - t < 60_000);
  const requireCode = (req, res, next) => {
    if (recentFailures(req.ip).length >= MAX_CODE_FAILURES) return reject(res, 429, 'Too many wrong codes. Wait a minute and try again.');
    if (!sameCode(normalizeCode(req.get('x-join-code')), joinCode)) {
      codeFailures.set(req.ip, [...recentFailures(req.ip), now()]);
      return reject(res, 403, 'Wrong or missing join code');
    }
    next();
  };
  const isLoopback = (ip) => ip === '127.0.0.1' || ip === '::1' || ip === '::ffff:127.0.0.1';

  // Runs BEFORE the adapter forwards anything to fount. fount verifies the caster's signature and spends MP, but
  // it does not check that the cost signed into the spell is the price we charge, nor that the spell can land, nor
  // replays, so all of that is decided here, while it is still free for the viewer.
  const precheck = (req, res, next) => {
    const name = req.params.spellName;
    const def = SPELLS[name];
    const spell = req.body;
    if (!def) return reject(res, 404, 'No such spell');
    if (!spell || typeof spell !== 'object' || Array.isArray(spell)) return reject(res, 400, 'Malformed spell');
    for (const key of Object.keys(spell)) if (!ALLOWED_KEYS.has(key)) return reject(res, 400, `Unexpected field: ${key}`);

    if (spell.spell !== name) return reject(res, 400, 'Spell name does not match the request');
    if (typeof spell.casterUUID !== 'string' || !SAFE_ID.test(spell.casterUUID)) return reject(res, 400, 'Bad casterUUID');
    if (spell.totalCost !== def.cost) return reject(res, 400, `${def.name} costs ${def.cost} MP`);
    if (spell.mp !== true) return reject(res, 400, 'Spells here are paid for with MP');
    if (!Number.isSafeInteger(spell.ordinal) || spell.ordinal <= 0) return reject(res, 400, 'Bad ordinal');
    if (typeof spell.casterSignature !== 'string' || !HEX(128).test(spell.casterSignature)) return reject(res, 400, 'Bad signature');
    // This gateway is the first hop. Anything in `gateways` would be an entry the caster invented, and fount pays
    // rewards to every gateway it lists.
    if (spell.gateways !== undefined && !(Array.isArray(spell.gateways) && spell.gateways.length === 0)) {
      return reject(res, 400, 'Spells must arrive without gateways');
    }
    if (typeof spell.timestamp !== 'string' || !/^\d{13}$/.test(spell.timestamp) || Math.abs(now() - Number(spell.timestamp)) > MAX_SKEW_MS) {
      return reject(res, 400, 'Stale or invalid timestamp. Is your clock right?');
    }

    if (spell.ordinal <= (lastOrdinal.get(spell.casterUUID) ?? 0)) return reject(res, 409, 'That spell was already cast');

    const wait = (cooldownUntil[name] ?? 0) - now();
    if (wait > 0) return reject(res, 429, `${def.name} is recharging`, { retryMs: wait });

    const can = battle.canCast(def.effect);
    if (!can.ok) return reject(res, 409, can.reason);

    // Reserve the ordinal and the cooldown now so two simultaneous casts can't both pass; release if it doesn't land.
    if (lastOrdinal.size >= MAX_CASTERS_REMEMBERED) lastOrdinal.delete(lastOrdinal.keys().next().value);
    lastOrdinal.set(spell.casterUUID, spell.ordinal);
    const reservedUntil = now() + def.cooldownMs;
    cooldownUntil[name] = reservedUntil;
    res.locals.release = () => { if (cooldownUntil[name] === reservedUntil) cooldownUntil[name] = 0; };
    res.on('finish', () => { if (!res.locals.landed) res.locals.release(); });
    next();
  };

  // fount answered. If it said yes, the spell lands in the fight.
  const onResolved = (req, res, result) => {
    const name = req.params.spellName;
    const def = SPELLS[name];
    if (result && result.success === true) {
      fountUser.ordinal += 1;
      const landed = battle.cast(def.effect, viewerName(req.body.casterUUID), name);
      if (landed.error) {  // the fight changed while fount was thinking: say so rather than pretend
        return res.status(409).send({ success: false, error: landed.error, charged: true });
      }
      res.locals.landed = true;
      broadcast('events', landed.events);
      return res.send({ success: true, spell: name });
    }
    return res.status(402).send({ success: false, error: 'The resolver refused the spell. Out of MP?' });
  };

  // ---------------- HTTP ----------------
  const app = express();
  app.set('trust proxy', 'loopback');   // a local tunnel (cloudflared, ngrok) forwards the viewer's real IP
  app.use(express.json({ limit: '4kb' }));
  // Bad JSON and oversized bodies get a JSON answer, not an HTML error page and a stack trace in the log.
  app.use((err, req, res, next) => {
    if (err.type === 'entity.too.large') return reject(res, 413, 'Request too large');
    if (err.type === 'entity.parse.failed') return reject(res, 400, 'Malformed JSON');
    next(err);
  });
  app.use(express.static(config.PUBLIC_DIR));
  app.get('/', (req, res) => res.sendFile('scene.html', { root: config.PUBLIC_DIR }));
  app.get('/cast', (req, res) => res.sendFile('cast.html', { root: config.PUBLIC_DIR }));

  app.get('/spells', (req, res) => res.send(Object.entries(SPELLS).map(([spell, s]) => ({ spell, ...s }))));  // `spell` is the MAGIC id, `name` the label
  app.get('/health', (req, res) => res.send({ ok: true, gatewayUUID: fountUser.uuid, viewers: sseClients.size }));
  app.get('/state', (req, res) => res.send(battle.snapshot()));
  // The scene (OBS, on this machine) reads the code from here to show it on stream. Anyone else gets null: the code is
  // for people watching, not for anyone who can reach the port. The terminal prints it too.
  app.get('/info', (req, res) => res.send({ castUrl, code: isLoopback(req.ip) ? joinCode : null }));
  // Lets the viewer page check a typed code straight away (it sends it as X-Join-Code like every other request).
  app.post('/join', requireCode, (req, res) => res.send({ success: true }));

  app.get('/events', (req, res) => {
    res.set({ 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'keep-alive', 'X-Accel-Buffering': 'no' });
    res.flushHeaders();
    res.write('retry: 2000\n\n');
    send(res, 'snapshot', battle.snapshot());
    sseClients.add(res);
    const beat = setInterval(() => res.write(': keep-alive\n\n'), 15_000);
    req.on('close', () => { clearInterval(beat); sseClients.delete(res); });
  });

  // Viewers talk only to this server: it registers them with fount and relays their MP, so fount itself never has to
  // be reachable from the internet.
  const tooManyRegistrations = (ip) => {
    const t = now();
    const recent = (registerHits.get(ip) || []).filter((x) => t - x < 60_000);
    recent.push(t);
    registerHits.set(ip, recent);
    return recent.length > 20;
  };

  app.put('/register', requireCode, async (req, res) => {
    if (tooManyRegistrations(req.ip)) return reject(res, 429, 'Too many registrations, slow down');
    const { timestamp, pubKey, signature } = req.body || {};
    if (typeof timestamp !== 'string' || !/^\d{13}$/.test(timestamp) || !HEX(66).test(pubKey) || !HEX(128).test(signature)) {
      return reject(res, 400, 'Malformed registration');
    }
    try {
      const r = await fetch(`${config.FOUNT_BASE}/user/create`, {
        method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ timestamp, pubKey, signature })
      });
      const body = await r.json();
      if (!r.ok || !body.uuid) return reject(res, 403, 'Registration refused');
      res.send({ success: true, uuid: body.uuid, mp: body.mp, maxMP: body.maxMP });
    } catch (err) {
      reject(res, 502, 'The resolver is unreachable');
    }
  });

  app.get('/mp', requireCode, async (req, res) => {
    const { uuid, timestamp, signature } = req.query;
    if (!SAFE_ID.test(String(uuid)) || !/^\d{13}$/.test(String(timestamp)) || !HEX(128).test(String(signature))) return reject(res, 400, 'Malformed request');
    try {
      const r = await fetch(`${config.FOUNT_BASE}/user/${uuid}?timestamp=${timestamp}&signature=${signature}`);
      const body = await r.json();
      if (!r.ok || body.error) return reject(res, 403, 'Unknown viewer');
      res.send({ success: true, mp: body.mp, maxMP: body.maxMP });  // nothing else from the user record leaves this server
    } catch (err) {
      reject(res, 502, 'The resolver is unreachable');
    }
  });

  // The cast endpoint: our checks first, then the standard MAGIC gateway (adds our signature, forwards to fount).
  app.post('/magic/spell/:spellName', requireCode, precheck);
  gateway.expressApp(app, fountUser, spellbook, config.STOP_NAME, identity, undefined, onResolved);

  return { app, battle, tick, broadcast, sseClients, spellbook, castUrl, joinCode };
};
