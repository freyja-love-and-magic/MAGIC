import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBattle, seededRng, HASTE_MS, ACTION_GAP_MS } from '../public/js/engine.js';
import { SPELLS } from '../public/js/spells.js';
import { viewerName } from '../public/js/names.js';

const DT = 50;
const fresh = (seed = 1) => createBattle({ rng: seededRng(seed) });
const hpOf = (b) => Object.fromEntries(b.snapshot().units.map((u) => [u.id, u.hp]));

// What a renderer does: it trusts hpAfter on each event. If this ever disagrees with the engine, the stream lies.
const shadow = (b) => {
  const hp = hpOf(b);
  const ko = Object.fromEntries(b.snapshot().units.map((u) => [u.id, u.ko]));
  return {
    hp, ko,
    apply(events) {
      // Events within one step are ordered by emission, not by `delay`, so hpAfter chains correctly.
      for (const e of events) {
        if (e.hpAfter !== undefined) hp[e.target] = e.hpAfter;
        if (e.t === 'ko') ko[e.target] = true;
        if (e.t === 'revive') ko[e.target] = false;
      }
    }
  };
};

test('starts as four heroes against three enemies, everyone at full health and standing', () => {
  const s = fresh().snapshot();
  assert.equal(s.units.filter((u) => u.side === 'hero').length, 4);
  assert.equal(s.units.filter((u) => u.side === 'enemy').length, 3);
  assert.ok(s.units.every((u) => u.hp === u.maxHp && !u.ko));
  assert.equal(s.units.filter((u) => u.boss).length, 1);
});

test('same seed, same fight; different seed, different fight', () => {
  const run = (seed) => { const b = fresh(seed); const out = []; for (let i = 0; i < 4000; i++) out.push(...b.step(DT)); return JSON.stringify(out); };
  assert.equal(run(7), run(7));
  assert.notEqual(run(7), run(8));
});

test('twenty minutes of fighting: nobody dies for good, everyone gets back up, nothing breaks', () => {
  for (const seed of [1, 2, 3, 4, 5]) {
    const b = fresh(seed);
    const sh = shadow(b);
    const downSince = {};
    const koCount = { hero: 0, enemy: 0 }, reviveCount = { hero: 0, enemy: 0 };
    let actions = 0;
    const MAX_DOWN_MS = 10500;

    for (let ms = 0; ms < 20 * 60 * 1000; ms += DT) {
      const events = b.step(DT);
      sh.apply(events);
      for (const e of events) {
        if (e.t === 'act') actions++;
        if (e.t === 'ko') { const u = b.unit(e.target); koCount[u.side]++; downSince[e.target] = ms; }
        if (e.t === 'revive') { const u = b.unit(e.target); reviveCount[u.side]++; delete downSince[e.target]; }
        if (e.t === 'damage' || e.t === 'heal') assert.ok(Number.isInteger(e.amount) && e.amount >= 0, `bad amount ${JSON.stringify(e)}`);
      }
      for (const u of b.snapshot().units) {
        assert.ok(u.hp >= 0 && u.hp <= u.maxHp, `${u.id} hp ${u.hp}`);
        assert.equal(u.ko, u.hp === 0, `${u.id} ko flag disagrees with hp`);
        assert.ok(u.mp >= 0 && u.mp <= u.maxMp, `${u.id} mp ${u.mp}`);
        assert.ok(u.atb >= 0 && u.atb <= 100);
        // the shadow built only from events must match the real state
        assert.equal(sh.hp[u.id], u.hp, `${u.id}: events say ${sh.hp[u.id]}, engine says ${u.hp} at ${ms}ms`);
        assert.equal(sh.ko[u.id], u.ko, `${u.id}: ko flag from events disagrees at ${ms}ms`);
      }
      for (const [id, since] of Object.entries(downSince)) {
        assert.ok(ms - since <= MAX_DOWN_MS, `${id} stayed down for ${ms - since}ms`);
      }
    }
    assert.ok(actions > 400, `seed ${seed}: only ${actions} actions in 20 minutes, the fight stalled`);
    assert.ok(koCount.hero >= 3 && koCount.enemy >= 3, `seed ${seed}: boring fight ${JSON.stringify(koCount)}`);
    // every knock-out was followed by a revive (the only ones still pending are those down right now)
    for (const side of ['hero', 'enemy']) assert.ok(koCount[side] - reviveCount[side] <= 4, `${side}: ${koCount[side]} KOs vs ${reviveCount[side]} revives`);
  }
});

test('only standing units act, and only standing units are targeted', () => {
  let actions = 0;
  for (const seed of [11, 12, 13]) {
    const b = fresh(seed);
    for (let ms = 0; ms < 600000; ms += DT) {
      const standing = new Set(b.snapshot().units.filter((u) => !u.ko).map((u) => u.id));
      for (const e of b.step(DT)) {
        if (e.t !== 'act') continue;
        actions++;
        assert.ok(standing.has(e.actor), `${e.actor} acted while down (${e.name})`);
        if (e.target && e.name !== 'Raise') assert.ok(standing.has(e.target), `${e.actor} targeted fallen ${e.target} with ${e.name}`);
        if (e.name === 'Raise') assert.ok(!standing.has(e.target), 'Raise must target a fallen hero');
      }
    }
  }
  assert.ok(actions > 1000);
});

test('the fight never goes quiet for long: actions keep coming', () => {
  const b = fresh(21);
  let sinceAction = 0, worst = 0;
  for (let ms = 0; ms < 600000; ms += DT) {
    const acted = b.step(DT).some((e) => e.t === 'act');
    sinceAction = acted ? 0 : sinceAction + DT;
    worst = Math.max(worst, sinceAction);
  }
  assert.ok(worst <= 12000, `longest silence ${worst}ms`);
});

test('actions are spaced out so the scene can play each one', () => {
  const b = fresh(5);
  let last = -Infinity;
  for (let ms = 0; ms < 120000; ms += DT) {
    for (const e of b.step(DT)) if (e.t === 'act') {
      assert.ok(ms - last >= ACTION_GAP_MS - DT, `two actions ${ms - last}ms apart`);
      last = ms;
    }
  }
});

test('green revive: a viewer revive raises every fallen hero at 70%, and only fallen heroes', () => {
  const b = fresh(3);
  assert.equal(b.canCast('revive').ok, false, 'nothing to revive at the start');
  assert.match(b.cast('revive', 'Brave Otter', 'arenaRevive').error, /No one has fallen/);

  // knock two heroes out with the engine's own rules by running until they fall
  let downed = [];
  for (let i = 0; i < 100000 && downed.length < 2; i++) { b.step(DT); downed = b.snapshot().units.filter((u) => u.side === 'hero' && u.ko); }
  assert.ok(downed.length >= 1, 'a hero should fall eventually');
  const ids = downed.map((u) => u.id);
  const { events } = b.cast('revive', 'Brave Otter', 'arenaRevive');
  const revives = events.filter((e) => e.t === 'revive');
  assert.deepEqual(revives.map((e) => e.target).sort(), ids.sort());
  for (const e of revives) { assert.equal(e.by, 'viewer'); assert.equal(e.hpAfter, Math.ceil(b.unit(e.target).maxHp * 0.7)); }
  assert.ok(b.snapshot().units.filter((u) => u.side === 'hero').every((u) => !u.ko));
  assert.equal(events[0].t, 'viewer');
});

test('fire hits the boss for 70-95; meteor hits every standing enemy for 95-130', () => {
  const b = fresh(9);
  const fire = b.cast('fire', 'x', 'arenaFire').events.find((e) => e.t === 'damage');
  assert.equal(fire.target, 'drake');
  assert.ok(fire.amount >= 70 && fire.amount <= 95 && fire.viewer === true, JSON.stringify(fire));
  const meteor = b.cast('meteor', 'x', 'arenaMeteor').events.filter((e) => e.t === 'damage');
  assert.equal(meteor.length, 3);
  for (const e of meteor) assert.ok(e.amount >= 95 || e.hpAfter === 0, `meteor did ${e.amount}`);
});

test('a viewer can knock the boss out and it still comes back (green)', () => {
  const b = fresh(4);
  const sh = shadow(b);
  let sawKo = false, sawRise = false;
  for (let i = 0; i < 60; i++) {
    const r = b.cast('fire', 'x', 'arenaFire');
    if (r.error) break;
    sh.apply(r.events);
    if (r.events.some((e) => e.t === 'ko' && e.target === 'drake')) { sawKo = true; break; }
  }
  assert.ok(sawKo, 'the boss should go down');
  for (let ms = 0; ms < 12000 && !sawRise; ms += DT) {
    const events = b.step(DT);
    sh.apply(events);
    sawRise = events.some((e) => e.t === 'revive' && e.target === 'drake' && e.by === 'auto');
  }
  assert.ok(sawRise, 'the boss rises again by itself');
  assert.equal(sh.ko.drake, false);
});

test('heal only reaches standing heroes and is refused when no one needs it', () => {
  const b = fresh(2);
  assert.equal(b.canCast('heal').ok, false);
  assert.match(b.cast('heal', 'x', 'arenaHeal').error, /good health/);
  // hurt the party by letting the enemies fight
  for (let i = 0; i < 4000; i++) b.step(DT);
  const before = hpOf(b);
  const standing = b.snapshot().units.filter((u) => u.side === 'hero' && !u.ko).map((u) => u.id);
  const r = b.cast('heal', 'x', 'arenaHeal');
  if (r.error) return;  // happened to be healthy: covered above
  const healed = r.events.filter((e) => e.t === 'heal');
  assert.deepEqual(healed.map((e) => e.target).sort(), standing.sort());
  for (const e of healed) assert.ok(e.hpAfter >= before[e.target] && e.hpAfter <= b.unit(e.target).maxHp);
});

test('haste speeds the party up for 12 seconds, then wears off, and cannot be stacked', () => {
  // Time until Brom's first action, with everyone else held back so they can't interfere.
  const timeToFirstAction = (hasted) => {
    const b = createBattle({ rng: () => 0.5 });
    b.unit('brom').atb = 0;
    if (hasted) b.cast('haste', 'x', 'arenaHaste');
    for (let ms = 0; ms < 20000; ms += DT) {
      b.units.filter((u) => u.id !== 'brom').forEach((u) => { u.atb = 0; });
      if (b.step(DT).some((e) => e.t === 'act' && e.actor === 'brom')) return ms + DT;
    }
    return Infinity;
  };
  const normal = timeToFirstAction(false), fast = timeToFirstAction(true);
  assert.ok(Number.isFinite(normal) && normal >= 3500 && normal <= 3700, `normal ${normal}ms (Brom fills in 3600)`);
  assert.ok(fast >= 1900 && fast <= 2100, `hasted ${fast}ms (3600 / 1.8 = 2000)`);

  const b = fresh(6);
  assert.equal(b.cast('haste', 'x', 'arenaHaste').events.filter((e) => e.t === 'status' && e.on).length, 4);
  assert.match(b.cast('haste', 'x', 'arenaHaste').error, /already hasted/);
  let off = 0, firstOff = null;
  for (let ms = 0; ms < HASTE_MS + 200; ms += DT) {
    const e = b.step(DT).filter((x) => x.t === 'status' && !x.on);
    if (e.length && firstOff === null) firstOff = ms;
    off += e.length;
  }
  assert.ok(off >= 3, 'haste wears off for the heroes still standing');
  assert.ok(firstOff >= HASTE_MS - 200, `wore off after ${firstOff}ms`);
  assert.equal(b.canCast('haste').ok, true);
});

test('unknown spells and spells with no valid target are refused without changing anything', () => {
  const b = fresh(1);
  const before = JSON.stringify(b.snapshot());
  assert.ok(b.cast('bogus', 'x', 'bogus').error);
  assert.equal(JSON.stringify(b.snapshot()), before);
});

test('every catalog spell maps to an effect the engine understands', () => {
  const b = fresh(1);
  for (const [name, spell] of Object.entries(SPELLS)) {
    assert.match(name, /^[A-Za-z0-9._:-]+$/, 'spell names must be safe MAGIC ids');
    assert.ok(Number.isInteger(spell.cost) && spell.cost > 0 && spell.cooldownMs > 0);
    assert.notEqual(b.canCast(spell.effect).reason, 'Unknown spell', name);
  }
});

test('viewer names are stable, friendly, and come only from the uuid', () => {
  assert.equal(viewerName('abc'), viewerName('abc'));
  assert.notEqual(viewerName('abc'), viewerName('abd'));
  assert.match(viewerName('9be79b1b-6709-4567-8391-43a95252586c'), /^[A-Z][a-z]+ [A-Z][a-z]+$/);
  const names = new Set(Array.from({ length: 500 }, (_, i) => viewerName(`uuid-${i}`)));
  assert.ok(names.size > 100, 'names should be varied');
});
