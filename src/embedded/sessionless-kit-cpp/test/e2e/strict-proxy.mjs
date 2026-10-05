// A verifying front for fount, used by the end-to-end test.
//
// fount's resolve() currently ignores failed signature checks (see README), so a test against fount alone
// can't tell a correct signature from a wrong one. This proxy sits in front of the REAL fount, verifies
// everything a correct resolver must, and forwards only what passes:
//   registration: signature over timestamp + pubKey, fresh timestamp
//   resolve:      caster signature over timestamp+spell+casterUUID+totalCost+mp+ordinal, every gateway signature
//                 over timestamp+uuid+minimumCost+ordinal, ordinal strictly increasing per caster, fresh
//                 timestamps, each gateway extra <= 128 bytes, body <= 2048 bytes
// Rejections answer 403 {"success":false,"error":...}. Every decision is logged as "ACCEPT ..." / "REJECT ...".
import http from 'node:http';
import { verify } from '../../../../sdk/javascript/sessionless-kit/src/index.js';

const FOUNT = process.env.FOUNT_URL || 'http://127.0.0.1:3006';
const PORT = Number(process.env.PROXY_PORT || 3099);
const SKEW = 300000;
const users = new Map();       // uuid -> pubKey, learned from registrations this proxy forwarded
const lastOrdinal = new Map(); // casterUUID -> last accepted ordinal

const fresh = (ts) => Math.abs(Date.now() - Number(ts)) <= SKEW;
const log = (kind, what) => console.log(`${kind} ${what}`);

const readBody = (req) => new Promise((resolve) => { let d = ''; req.on('data', (c) => (d += c)); req.on('end', () => resolve(d)); });
const send = (res, status, obj) => { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
const forward = async (req, path, raw) => {
  const r = await fetch(FOUNT + path, { method: req.method, headers: { 'Content-Type': 'application/json' }, body: raw });
  return { status: r.status, text: await r.text() };
};

const checkResolve = (raw, spell) => {
  if (Buffer.byteLength(raw) > 2048) return 'body over 2048 bytes';
  if (!fresh(spell.timestamp)) return 'stale caster timestamp';
  const caster = users.get(spell.casterUUID);
  if (!caster) return 'unknown caster';
  const msg = spell.timestamp + spell.spell + spell.casterUUID + spell.totalCost + spell.mp + spell.ordinal;
  if (!verify(spell.casterSignature, msg, caster)) return 'bad caster signature';
  for (const g of spell.gateways || []) {
    const pub = users.get(g.uuid);
    if (!pub) return 'unknown gateway';
    if (!fresh(g.timestamp)) return 'stale gateway timestamp';
    if (!verify(g.signature, g.timestamp + g.uuid + g.minimumCost + g.ordinal, pub)) return 'bad gateway signature';
    if (g.extra !== undefined && Buffer.byteLength(JSON.stringify(g.extra)) > 128) return 'gateway extra over 128 bytes';
  }
  if (!(spell.ordinal > (lastOrdinal.get(spell.casterUUID) ?? 0))) return 'replayed ordinal';
  return null;
};

http.createServer(async (req, res) => {
  try {
    const raw = await readBody(req);
    if (req.method === 'PUT' && req.url === '/user/create') {
      const b = JSON.parse(raw);
      if (!fresh(b.timestamp) || !verify(b.signature, b.timestamp + b.pubKey, b.pubKey)) {
        log('REJECT', 'registration bad signature or stale');
        return send(res, 403, { error: 'auth error' });
      }
      const f = await forward(req, req.url, raw);
      const user = JSON.parse(f.text);
      if (user.uuid) users.set(user.uuid, b.pubKey);
      log('ACCEPT', `registration ${user.uuid}`);
      res.writeHead(f.status, { 'Content-Type': 'application/json' });
      return res.end(f.text);
    }
    const m = req.method === 'POST' && req.url.match(/^\/resolve\/([A-Za-z0-9._:-]+)$/);
    if (m) {
      const spell = JSON.parse(raw);
      const why = checkResolve(raw, spell);
      if (why) {
        log('REJECT', `resolve ${m[1]}: ${why}`);
        return send(res, 403, { success: false, error: why });
      }
      const f = await forward(req, req.url, raw);
      let ok = false;
      try { ok = JSON.parse(f.text).success === true; } catch {}
      if (ok) lastOrdinal.set(spell.casterUUID, spell.ordinal);
      log(ok ? 'ACCEPT' : 'REJECT', `resolve ${m[1]} ordinal=${spell.ordinal} fount=${f.status}${(spell.gateways?.[0]?.extra) ? ' extra=' + JSON.stringify(spell.gateways[0].extra) : ''}`);
      res.writeHead(f.status, { 'Content-Type': 'application/json' });
      return res.end(f.text);
    }
    send(res, 404, { error: 'not found' });
  } catch (err) {
    log('ERROR', err.message);
    send(res, 502, { error: err.message });
  }
}).listen(PORT, '127.0.0.1', () => console.log(`READY proxy on ${PORT} -> ${FOUNT}`));
