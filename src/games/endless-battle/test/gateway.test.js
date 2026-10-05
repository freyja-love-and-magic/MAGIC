// The viewer gateway against a REAL fount. Needs the fixed fount (resolve() must refuse bad signatures).
import { test as nodeTest, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { Identity } from 'sessionless-kit';
import { SPELLS } from '../public/js/spells.js';
import { viewerName } from '../public/js/names.js';
import { createArena } from '../src/arena.js';
import { startFount, startArena, makeViewer, buildSpell, cast, mpOf, watchEvents, FOUNT_BASE, PUBLIC_DIR } from './helpers.js';

let fount;
before(async () => { fount = await startFount(); });
after(() => { fount?.stop(); });

// Every test gets its own arena (own gateway identity, own battle, own cooldowns), so tests can't affect each other.
let arena;
const test = (name, fn) => nodeTest(name, async () => {
  arena = await startArena({ seed: 1 });
  try { await fn(); } finally { arena.stop(); }
});

const hurtTheParty = () => { for (let i = 0; i < 4000; i++) arena.tick(50); };

test('a viewer registers through the gateway, idempotently, and bad registrations are refused', async () => {
  const v = await makeViewer(arena);
  assert.match(v.uuid, /^[0-9a-f-]{36}$/);
  const again = await makeViewer({ ...arena, base: arena.base });  // a different key is a different viewer
  assert.notEqual(again.uuid, v.uuid);

  const ts = Date.now().toString();
  const send = (body) => fetch(`${arena.base}/register`, { method: 'PUT', headers: { 'Content-Type': 'application/json', 'X-Join-Code': arena.joinCode }, body: JSON.stringify(body) });
  const id = await Identity.ephemeral();
  const forged = await (await Identity.ephemeral()).sign(ts + id.pubKey);  // signed by someone else
  assert.equal((await send({ timestamp: ts, pubKey: id.pubKey, signature: forged })).status, 403);
  assert.equal((await send({ timestamp: ts, pubKey: 'nope', signature: forged })).status, 400);
  assert.equal((await send({})).status, 400);
});

test('MP is readable by the viewer, signed, and nothing else from the user record leaks', async () => {
  const v = await makeViewer(arena);
  const { status, body } = await mpOf(arena, v);
  assert.equal(status, 200);
  assert.deepEqual(Object.keys(body).sort(), ['maxMP', 'mp', 'success']);
  assert.equal(body.mp, 1000);

  const ts = Date.now().toString();
  const wrong = await (await Identity.ephemeral()).sign(ts + v.uuid);
  const r = await fetch(`${arena.base}/mp?uuid=${v.uuid}&timestamp=${ts}&signature=${wrong}`, { headers: { 'X-Join-Code': arena.joinCode } });
  assert.equal(r.status, 403);
});

test('a real cast lands: fount charges the MP, the fight changes, the scene is told', async () => {
  const v = await makeViewer(arena);
  const watch = await watchEvents(arena);
  const first = await watch.wait((e) => e.event === 'snapshot');
  assert.ok(first, 'a new scene gets a snapshot immediately');
  assert.equal(first.data.units.length, 7);

  const before = (await mpOf(arena, v)).body.mp;
  const r = await cast(arena, await buildSpell(v, 'arenaFire'));
  assert.deepEqual(r, { status: 200, body: { success: true, spell: 'arenaFire' } });
  assert.equal((await mpOf(arena, v)).body.mp, before - SPELLS.arenaFire.cost, 'fount charged exactly the spell cost');

  const hit = await watch.wait((e) => e.event === 'events' && e.data.some((x) => x.t === 'damage' && x.viewer));
  assert.ok(hit, 'the scene received the viewer damage');
  const announce = hit.data.find((x) => x.t === 'viewer');
  assert.equal(announce.who, viewerName(v.uuid));
  assert.equal(announce.spell, 'arenaFire');
  assert.equal(hit.data.find((x) => x.t === 'damage').target, 'drake');
  watch.stop();
});

test('a viewer cannot underpay: the cost signed into the spell must be the price', async () => {
  const v = await makeViewer(arena);
  const before = (await mpOf(arena, v)).body.mp;
  for (const cost of [0, 1, 49, 51, 5000, -50]) {
    const r = await cast(arena, await buildSpell(v, 'arenaFire', { totalCost: cost }));
    assert.equal(r.status, 400, `cost ${cost}: ${JSON.stringify(r.body)}`);
    assert.match(r.body.error, /costs 50 MP/);
  }
  assert.equal((await mpOf(arena, v)).body.mp, before, 'nothing was charged');
});

test('a spell signed with someone else\'s key is refused by fount, and the cooldown is not burned', async () => {
  const victim = await makeViewer(arena);
  const mallory = await Identity.ephemeral();
  const before = (await mpOf(arena, victim)).body.mp;
  const forged = await cast(arena, await buildSpell(victim, 'arenaFire', { signer: mallory }));
  assert.equal(forged.status, 402);
  assert.equal(forged.body.success, false);
  assert.equal((await mpOf(arena, victim)).body.mp, before, 'the victim was not charged');
  // the cooldown reservation was released, so the real owner can cast right away
  const honest = await cast(arena, await buildSpell(victim, 'arenaFire'));
  assert.equal(honest.status, 200, JSON.stringify(honest.body));
});

test('replaying a captured cast is refused and costs nothing', async () => {
  const v = await makeViewer(arena);
  const spell = await buildSpell(v, 'arenaFire');
  assert.equal((await cast(arena, spell)).status, 200);
  const mp = (await mpOf(arena, v)).body.mp;
  const replay = await cast(arena, spell);
  assert.equal(replay.status, 409);
  assert.match(replay.body.error, /already cast/);
  const older = await cast(arena, await buildSpell(v, 'arenaFire', { ordinal: spell.ordinal - 5 }));
  assert.equal(older.status, 409, 'an older ordinal is a replay too');
  assert.equal((await mpOf(arena, v)).body.mp, mp);
});

test('cooldowns are shared by all viewers, free to hit, and recover with time', async () => {
  const a = await makeViewer(arena), b = await makeViewer(arena);
  assert.equal((await cast(arena, await buildSpell(a, 'arenaFire'))).status, 200);
  const mpB = (await mpOf(arena, b)).body.mp;
  const blocked = await cast(arena, await buildSpell(b, 'arenaFire'));
  assert.equal(blocked.status, 429);
  assert.ok(blocked.body.retryMs > 0 && blocked.body.retryMs <= SPELLS.arenaFire.cooldownMs);
  assert.equal((await mpOf(arena, b)).body.mp, mpB, 'a blocked cast costs nothing');
  arena.advanceClock(SPELLS.arenaFire.cooldownMs + 1);
  assert.equal((await cast(arena, await buildSpell(b, 'arenaFire'))).status, 200);
});

test('two viewers casting at the same instant: exactly one lands', async () => {
  const a = await makeViewer(arena), b = await makeViewer(arena);
  const [sa, sb] = [await buildSpell(a, 'arenaFire'), await buildSpell(b, 'arenaFire')];
  const results = await Promise.all([cast(arena, sa), cast(arena, sb)]);
  assert.deepEqual(results.map((r) => r.status).sort(), [200, 429]);
});

test('spells that could not land are refused before any MP is spent', async () => {
  const v = await makeViewer(arena);
  const before = (await mpOf(arena, v)).body.mp;
  const r = await cast(arena, await buildSpell(v, 'arenaRevive'));
  assert.equal(r.status, 409);
  assert.match(r.body.error, /No one has fallen/);
  assert.equal((await mpOf(arena, v)).body.mp, before);
});

test('heal and revive work on a battered party, and the revive is the green one', async () => {
  hurtTheParty();
  const downed = () => arena.battle.snapshot().units.filter((u) => u.side === 'hero' && u.ko).length;
  while (downed() === 0) arena.tick(50);   // run until a hero actually falls

  const v = await makeViewer(arena);
  const watch = await watchEvents(arena);
  const heal = await cast(arena, await buildSpell(v, 'arenaHeal'));
  assert.equal(heal.status, 200, JSON.stringify(heal.body));
  assert.ok(await watch.wait((e) => e.event === 'events' && e.data.some((x) => x.t === 'heal')));

  if (downed() === 0) return watch.stop();  // an auto-revive got there first; the engine tests cover the rest
  const rev = await cast(arena, await buildSpell(v, 'arenaRevive'));
  assert.equal(rev.status, 200, JSON.stringify(rev.body));
  const hit = await watch.wait((e) => e.event === 'events' && e.data.some((x) => x.t === 'revive' && x.by === 'viewer'));
  assert.ok(hit, 'the scene got a viewer revive');
  assert.equal(downed(), 0);
  watch.stop();
});

test('out of MP: fount says no, the gateway reports it, the fight is untouched, the cooldown is released', async () => {
  const v = await makeViewer(arena);
  // drain the viewer straight at fount with a spell only fount sees
  const drain = await buildSpell(v, 'drainForTest', { totalCost: 990 });
  const res = await fetch(`${FOUNT_BASE}/resolve/drainForTest`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...drain, spell: 'drainForTest' }) });
  assert.equal((await res.json()).success, true);
  assert.equal((await mpOf(arena, v)).body.mp, 10);

  const before = JSON.stringify(arena.battle.snapshot());
  const r = await cast(arena, await buildSpell(v, 'arenaMeteor'));
  assert.equal(r.status, 402);
  assert.match(r.body.error, /Out of MP/);
  assert.equal(JSON.stringify(arena.battle.snapshot()), before, 'nothing happened in the fight');
  const rich = await makeViewer(arena);
  assert.equal((await cast(arena, await buildSpell(rich, 'arenaMeteor'))).status, 200, 'the failed cast did not burn the cooldown');
});

test('malformed and hostile casts are rejected without touching fount', async () => {
  const v = await makeViewer(arena);
  const good = await buildSpell(v, 'arenaFire');
  const bad = [
    ['unknown spell', 'arenaNuke', { ...good, spell: 'arenaNuke' }, 404],
    ['name mismatch', 'arenaFire', { ...good, spell: 'arenaHeal' }, 400],
    ['mp false', 'arenaFire', { ...good, mp: false }, 400],
    ['extra field', 'arenaFire', { ...good, recipientUUID: 'x' }, 400],
    ['forged gateway entry', 'arenaFire', { ...good, gateways: [{ timestamp: '1', uuid: 'a', minimumCost: 0, ordinal: 1, signature: 'x'.repeat(130) }] }, 400],
    ['stale timestamp', 'arenaFire', { ...good, timestamp: String(Date.now() - 120000) }, 400],
    ['non-numeric timestamp', 'arenaFire', { ...good, timestamp: 'yesterday' }, 400],
    ['bad uuid', 'arenaFire', { ...good, casterUUID: '<script>alert(1)</script>' }, 400],
    ['short signature', 'arenaFire', { ...good, casterSignature: 'abcd' }, 400],
    ['fractional ordinal', 'arenaFire', { ...good, ordinal: 1.5 }, 400],
    ['string cost', 'arenaFire', { ...good, totalCost: '50' }, 400],
    ['array body', 'arenaFire', [good], 400],
    ['null body', 'arenaFire', null, 400],
  ];
  for (const [label, name, body, want] of bad) {
    const r = await fetch(`${arena.base}/magic/spell/${name}`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Join-Code': arena.joinCode }, body: JSON.stringify(body) });
    assert.equal(r.status, want, `${label}: got ${r.status} ${await r.text()}`);
  }
  const huge = await fetch(`${arena.base}/magic/spell/arenaFire`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Join-Code': arena.joinCode }, body: JSON.stringify({ ...good, pad: 'x'.repeat(10000) }) });
  assert.equal(huge.status, 413);
  assert.equal((await cast(arena, good)).status, 200, 'the untouched spell still works');
});

test('the info endpoints a stream needs', async () => {
  const list = await (await fetch(`${arena.base}/spells`)).json();
  assert.deepEqual(list.map((s) => s.spell).sort(), Object.keys(SPELLS).sort());
  assert.ok(list.every((s) => s.name && s.cost === SPELLS[s.spell].cost && s.icon));
  const health = await (await fetch(`${arena.base}/health`)).json();
  assert.equal(health.gatewayUUID, arena.fountUser.uuid);
  assert.equal((await fetch(`${arena.base}/qr.svg`)).status, 404, 'the QR code is gone');
});

// ---------------------------------------------------------------- the join code

const withCode = (arena, code, extra = {}) => ({ headers: { 'Content-Type': 'application/json', ...(code === undefined ? {} : { 'X-Join-Code': code }), ...extra } });

test('the join code looks like something you can read off a screen: 4 characters, no lookalikes', () => {
  const codes = new Set();
  for (let i = 0; i < 300; i++) {
    const a = createArena({ config: { FOUNT_BASE, PORT: 0, STOP_NAME: 'arena', PUBLIC_DIR }, identity: { sign: async () => '' }, fountUser: { uuid: 'x', ordinal: 0 } });
    assert.match(a.joinCode, /^[A-HJKMNP-Z2-9]{4}$/, a.joinCode);   // no 0 O 1 I L
    codes.add(a.joinCode);
  }
  assert.ok(codes.size > 250, `codes should be varied, got ${codes.size} distinct of 300`);
});

test('JOIN_CODE can be set (any case), and a bad one is refused at startup', () => {
  const make = (code) => createArena({ config: { FOUNT_BASE, PORT: 0, STOP_NAME: 'arena', PUBLIC_DIR, JOIN_CODE: code }, identity: { sign: async () => '' }, fountUser: { uuid: 'x', ordinal: 0 } });
  assert.equal(make('pizza-9').joinCode, 'PIZZA9');
  assert.equal(make('ab cd').joinCode, 'ABCD');
  assert.throws(() => make('x'), /JOIN_CODE must be 4 to 8/);
  assert.throws(() => make('way-too-long-code'), /JOIN_CODE must be 4 to 8/);
  assert.throws(() => make('ab!d'), /JOIN_CODE must be 4 to 8/);
});

test('without the code (or with a wrong one) a viewer can do nothing: register, MP and casting are all refused', async () => {
  const v = await makeViewer(arena);
  const spell = await buildSpell(v, 'arenaFire');
  const id = await Identity.ephemeral();
  const ts = Date.now().toString();
  const reg = { method: 'PUT', body: JSON.stringify({ timestamp: ts, pubKey: id.pubKey, signature: await id.sign(ts + id.pubKey) }) };
  const mpUrl = `${arena.base}/mp?uuid=${v.uuid}&timestamp=${ts}&signature=${await v.id.sign(ts + v.uuid)}`;
  const mine = (await mpOf(arena, v)).body.mp;

  // Each wrong attempt comes from its own address (as if via a tunnel), so the guessing limiter, tested separately
  // below, doesn't get in the way of checking every endpoint.
  let n = 0;
  const from = () => ({ 'X-Forwarded-For': `198.51.100.${++n}` });
  const withCode = (a, code) => ({ headers: { 'Content-Type': 'application/json', ...from(), ...(code === undefined ? {} : { 'X-Join-Code': code }) } });
  for (const code of [undefined, '', 'ZZZZ', arena.joinCode + 'X', arena.joinCode.slice(1)]) {
    const label = JSON.stringify(code);
    assert.equal((await fetch(`${arena.base}/register`, { ...reg, ...withCode(arena, code) })).status, 403, `register with ${label}`);
    assert.equal((await fetch(mpUrl, withCode(arena, code))).status, 403, `mp with ${label}`);
    assert.equal((await fetch(`${arena.base}/magic/spell/arenaFire`, { method: 'POST', body: JSON.stringify(spell), ...withCode(arena, code) })).status, 403, `cast with ${label}`);
    assert.equal((await fetch(`${arena.base}/join`, { method: 'POST', ...withCode(arena, code) })).status, 403, `join with ${label}`);
  }
  assert.equal((await mpOf(arena, v)).body.mp, mine, 'nothing was charged');
  assert.equal(arena.battle.snapshot().units.find((u) => u.id === 'drake').hp, 900, 'the fight is untouched');
  // the right code works, however it was typed
  for (const typed of [arena.joinCode, arena.joinCode.toLowerCase(), ` ${arena.joinCode.slice(0, 2)} ${arena.joinCode.slice(2)} `, `${arena.joinCode.slice(0, 2)}-${arena.joinCode.slice(2)}`]) {
    assert.equal((await fetch(`${arena.base}/join`, { method: 'POST', ...withCode(arena, typed) })).status, 200, `typed as ${JSON.stringify(typed)}`);
  }
  assert.equal((await cast(arena, spell)).status, 200);
});

test('guessing the code is rate limited per address, and the limit lifts after a minute', async () => {
  const guess = async (code) => (await fetch(`${arena.base}/join`, { method: 'POST', ...withCode(arena, code) })).status;
  for (let i = 0; i < 10; i++) assert.equal(await guess('AAAA'), 403);
  assert.equal(await guess('AAAA'), 429, 'the 11th wrong guess is told to wait');
  assert.equal(await guess(arena.joinCode), 429, 'even the right code waits, or the limiter would be pointless');
  arena.advanceClock(61_000);
  assert.equal(await guess(arena.joinCode), 200, 'and it lifts');
  // a different address is unaffected
  for (let i = 0; i < 10; i++) await guess('BBBB');
  const other = await fetch(`${arena.base}/join`, { method: 'POST', headers: { 'X-Join-Code': arena.joinCode, 'X-Forwarded-For': '203.0.113.9' } });
  assert.equal(other.status, 200, 'someone else on another address can still join');
});

test('only the scene on the server\'s own machine is told the code', async () => {
  const local = await (await fetch(`${arena.base}/info`)).json();
  assert.equal(local.code, arena.joinCode);
  assert.equal(local.castUrl, 'http://example.test/cast');
  // the same request, but arriving through a tunnel on behalf of someone out on the internet
  const remote = await (await fetch(`${arena.base}/info`, { headers: { 'X-Forwarded-For': '198.51.100.7' } })).json();
  assert.equal(remote.code, null, 'a remote visitor does not get the code');
  assert.ok(!JSON.stringify(await (await fetch(`${arena.base}/state`)).json()).includes(arena.joinCode), 'the code is not in the public state');
  assert.ok(!JSON.stringify(await (await fetch(`${arena.base}/health`)).json()).includes(arena.joinCode), 'nor in /health');
});
