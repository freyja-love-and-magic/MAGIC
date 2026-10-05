// The battle: four heroes against a dragon and two minions, an Active Time Battle where nobody stays down.
// Pure and deterministic given an rng: no timers, no DOM, no I/O. The server steps it; the scene just plays back
// the events it emits. Every event carries the value AFTER it applies (hpAfter, mpAfter) so a renderer never has
// to recompute anything, and replaying the events always reproduces the engine's state (the tests check this).

export const seededRng = (seed) => {  // mulberry32
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
};

export const ACTION_GAP_MS = 1100;   // breathing room between actions so the scene can play each one out
export const HASTE_MS = 12000;
export const HASTE_FACTOR = 1.8;

const HERO_REVIVE_PCT = 0.5, ENEMY_REVIVE_PCT = 0.5;
const REVIVE_AFTER_MS = [7000, 10000];  // how long a fallen unit lies there before getting back up

const roster = () => [
  { id: 'brom', side: 'hero', name: 'Brom', job: 'Warrior', emoji: '🤺', maxHp: 190, maxMp: 0, fillMs: 3600 },
  { id: 'cass', side: 'hero', name: 'Cass', job: 'Black Mage', emoji: '🧙', maxHp: 120, maxMp: 60, fillMs: 4200 },
  { id: 'aria', side: 'hero', name: 'Aria', job: 'White Mage', emoji: '🧝', maxHp: 130, maxMp: 70, fillMs: 4500 },
  { id: 'dex', side: 'hero', name: 'Dex', job: 'Thief', emoji: '🥷', maxHp: 140, maxMp: 0, fillMs: 2700 },
  { id: 'drake', side: 'enemy', name: 'Ember Drake', job: 'Boss', emoji: '🐉', maxHp: 900, maxMp: 0, fillMs: 3900, boss: true },
  { id: 'imp1', side: 'enemy', name: 'Imp', job: 'Minion', emoji: '👺', maxHp: 150, maxMp: 0, fillMs: 3100 },
  { id: 'bat', side: 'enemy', name: 'Cave Bat', job: 'Minion', emoji: '🦇', maxHp: 130, maxMp: 0, fillMs: 2600 }
];

export const createBattle = ({ rng = Math.random } = {}) => {
  const units = roster().map((u) => ({
    ...u, hp: u.maxHp, mp: u.maxMp, atb: Math.floor(rng() * 70), ko: false, reviveIn: 0, hasteMs: 0, readySeq: 0
  }));
  const byId = (id) => units.find((u) => u.id === id);
  const alive = (side) => units.filter((u) => u.side === side && !u.ko);
  const fallen = (side) => units.filter((u) => u.side === side && u.ko);
  const pick = (list) => list[Math.floor(rng() * list.length)];
  const between = (lo, hi) => lo + Math.floor(rng() * (hi - lo + 1));

  let lock = 0;        // ms until the next action may begin
  let seq = 0;         // orders units that became ready, first come first served
  let mpClock = 0;

  // ---- event builders: each applies its effect to the state AND returns the event describing it ----

  const damage = (target, amount, kind, delay, extra = {}) => {
    const dealt = Math.max(1, Math.min(target.hp, Math.round(amount)));
    target.hp -= dealt;
    const events = [{ t: 'damage', target: target.id, amount: dealt, hpAfter: target.hp, kind, delay, ...extra }];
    if (target.hp === 0) events.push(...knockOut(target, delay + 250));
    return events;
  };

  const knockOut = (target, delay) => {
    target.ko = true;
    target.atb = 0;
    target.hasteMs = 0;
    target.reviveIn = between(REVIVE_AFTER_MS[0], REVIVE_AFTER_MS[1]);
    return [
      { t: 'ko', target: target.id, delay },
      { t: 'msg', text: target.side === 'hero' ? `${target.name} falls!` : `${target.name} is down!`, delay }
    ];
  };

  const heal = (target, amount, delay) => {
    const healed = Math.max(0, Math.min(target.maxHp - target.hp, Math.round(amount)));
    target.hp += healed;
    return [{ t: 'heal', target: target.id, amount: healed, hpAfter: target.hp, delay }];
  };

  const revive = (target, pct, by, delay) => {
    target.ko = false;
    target.reviveIn = 0;
    target.hp = Math.max(1, Math.ceil(target.maxHp * pct));
    target.atb = 30;
    return [
      { t: 'revive', target: target.id, amount: target.hp, hpAfter: target.hp, by, delay },
      { t: 'msg', text: target.side === 'hero' ? `${target.name} gets back up!` : `${target.name} rises again!`, delay }
    ];
  };

  const spendMp = (u, cost) => { u.mp -= cost; };
  const act = (actor, name, kind, targetId, delay = 0) => ({
    t: 'act', actor: actor.id, name, kind, target: targetId ?? null, mpAfter: actor.mp, delay
  });

  // ---- what each unit does when its gauge fills ----

  const strike = (actor, target, lo, hi, kind, delay, critChance = 0.1) => {
    const crit = rng() < critChance;
    return damage(target, (between(lo, hi)) * (crit ? 2 : 1), crit ? 'crit' : kind, delay, crit ? { crit: true } : {});
  };

  const heroTurn = (u) => {
    const foes = alive('enemy');
    if (foes.length === 0) return [];
    const boss = foes.find((f) => f.boss);
    const mainFoe = () => (boss && rng() < 0.6 ? boss : pick(foes));
    switch (u.id) {
      case 'brom': {
        const t = mainFoe();
        return [act(u, 'Attack', 'lunge', t.id), ...strike(u, t, 18, 30, 'physical', 380)];
      }
      case 'dex': {
        const t = mainFoe();
        const events = [act(u, 'Double Cut', 'lunge', t.id), ...strike(u, t, 9, 16, 'physical', 300, 0.15)];
        if (!t.ko) events.push(...strike(u, t, 9, 16, 'physical', 560, 0.15));
        return events;
      }
      case 'cass': {
        if (u.mp >= 16 && foes.length >= 2 && rng() < 0.3) {
          spendMp(u, 16);
          const events = [act(u, 'Thunder', 'cast', null)];
          foes.forEach((f, i) => events.push(...damage(f, between(16, 24), 'thunder', 500 + i * 90)));
          return events;
        }
        if (u.mp >= 10) {
          spendMp(u, 10);
          const t = mainFoe();
          return [act(u, 'Fire', 'cast', t.id), ...damage(t, between(30, 44), 'fire', 520)];
        }
        const t = pick(foes);
        return [act(u, 'Staff', 'lunge', t.id), ...damage(t, between(5, 9), 'physical', 380)];
      }
      case 'aria': {
        const down = fallen('hero');
        if (down.length && u.mp >= 12) {
          spendMp(u, 12);
          const t = pick(down);
          return [act(u, 'Raise', 'cast', t.id), ...revive(t, 0.35, 'prayer', 600)];
        }
        const hurt = alive('hero').filter((h) => h.hp < h.maxHp * 0.55).sort((a, b) => a.hp / a.maxHp - b.hp / b.maxHp);
        if (hurt.length && u.mp >= 6) {
          spendMp(u, 6);
          return [act(u, 'Cure', 'cast', hurt[0].id), ...heal(hurt[0], between(45, 62), 520)];
        }
        const t = pick(foes);
        return [act(u, 'Rod', 'lunge', t.id), ...damage(t, between(5, 9), 'physical', 380)];
      }
      default:
        return [];
    }
  };

  const enemyTurn = (u) => {
    const heroes = alive('hero');
    if (heroes.length === 0) return [];
    if (u.boss) {
      if (rng() < 0.28) {
        const events = [act(u, 'Flame Breath', 'cast', null)];
        heroes.forEach((h, i) => events.push(...damage(h, between(15, 24), 'fire', 560 + i * 90)));
        return events;
      }
      const t = pick(heroes);
      return [act(u, 'Claw', 'lunge', t.id), ...strike(u, t, 24, 38, 'physical', 420, 0.08)];
    }
    const t = pick(heroes);
    return [act(u, u.id === 'bat' ? 'Bite' : 'Scratch', 'lunge', t.id), ...damage(t, between(9, 15), 'physical', 380)];
  };

  // ---- time ----

  const step = (dtMs) => {
    const events = [];
    lock = Math.max(0, lock - dtMs);

    mpClock += dtMs;
    while (mpClock >= 1500) {
      mpClock -= 1500;
      for (const u of units) if (u.side === 'hero' && u.maxMp > 0 && !u.ko) u.mp = Math.min(u.maxMp, u.mp + 1);
    }

    for (const u of units) {
      if (u.ko) {
        u.reviveIn -= dtMs;
        if (u.reviveIn <= 0) events.push(...revive(u, u.side === 'hero' ? HERO_REVIVE_PCT : ENEMY_REVIVE_PCT, 'auto', 0));
        continue;
      }
      if (u.hasteMs > 0) {
        u.hasteMs = Math.max(0, u.hasteMs - dtMs);
        if (u.hasteMs === 0) events.push({ t: 'status', target: u.id, status: 'haste', on: false, delay: 0 });
      }
      if (u.atb < 100) {
        u.atb = Math.min(100, u.atb + (100 * dtMs * (u.hasteMs > 0 ? HASTE_FACTOR : 1)) / u.fillMs);
        if (u.atb >= 100) u.readySeq = ++seq;
      }
    }

    // One action at a time, the unit that filled first goes first.
    while (lock === 0) {
      const ready = units.filter((u) => !u.ko && u.atb >= 100).sort((a, b) => a.readySeq - b.readySeq)[0];
      if (!ready) break;
      const result = ready.side === 'hero' ? heroTurn(ready) : enemyTurn(ready);
      if (result.length === 0) { ready.atb = 50; continue; }  // nothing to do (no targets): try again shortly
      ready.atb = 0;
      lock = ACTION_GAP_MS;
      events.push(...result);
    }
    return events;
  };

  // ---- viewer spells ----

  const canCast = (effect) => {
    switch (effect) {
      case 'fire': return alive('enemy').length ? { ok: true } : { ok: false, reason: 'Nothing is standing to burn' };
      case 'meteor': return alive('enemy').length ? { ok: true } : { ok: false, reason: 'Nothing is standing to crush' };
      case 'heal': return alive('hero').some((h) => h.hp < h.maxHp * 0.9) ? { ok: true } : { ok: false, reason: 'The party is in good health' };
      case 'revive': return fallen('hero').length ? { ok: true } : { ok: false, reason: 'No one has fallen' };
      case 'haste': return alive('hero').some((h) => h.hasteMs === 0) ? { ok: true } : { ok: false, reason: 'The party is already hasted' };
      default: return { ok: false, reason: 'Unknown spell' };
    }
  };

  const cast = (effect, who, spellName) => {
    const check = canCast(effect);
    if (!check.ok) return { error: check.reason };
    const events = [{ t: 'viewer', who, spell: spellName, effect, delay: 0 }];
    switch (effect) {
      case 'fire': {
        const foes = alive('enemy');
        const t = foes.find((f) => f.boss) || pick(foes);
        events.push(...damage(t, between(70, 95), 'fire', 450, { viewer: true }));
        break;
      }
      case 'meteor': {
        alive('enemy').forEach((f, i) => events.push(...damage(f, between(95, 130), 'meteor', 700 + i * 140, { viewer: true })));
        break;
      }
      case 'heal':
        alive('hero').forEach((h, i) => events.push(...heal(h, 60, 350 + i * 110)));
        break;
      case 'revive':
        fallen('hero').forEach((h, i) => events.push(...revive(h, 0.7, 'viewer', 450 + i * 150)));
        break;
      case 'haste':
        alive('hero').forEach((h) => {
          h.hasteMs = HASTE_MS;
          events.push({ t: 'status', target: h.id, status: 'haste', on: true, delay: 300 });
        });
        break;
    }
    return { events };
  };

  const snapshot = () => ({
    units: units.map((u) => ({
      id: u.id, side: u.side, name: u.name, job: u.job, emoji: u.emoji, boss: !!u.boss,
      hp: u.hp, maxHp: u.maxHp, mp: u.mp, maxMp: u.maxMp, atb: Math.floor(u.atb), ko: u.ko, hasted: u.hasteMs > 0
    }))
  });

  return { step, cast, canCast, snapshot, unit: byId, units };
};
