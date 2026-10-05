// The scene: plays back battle events with animation and sound. It never decides anything; the server (or, in
// local preview mode, the engine running right here) says what happened and what the numbers are afterwards.
//
//   /                       live: watches the server's /events (this is what OBS shows)
//   /?local=1               preview without any server: runs the engine here. Keys 1-5 cast a spell.
//   /?local=1&seed=7&warp=90   same, fast-forwarded 90 s first (handy for screenshots)
//   &mute=1                 no sound
import { createBattle, seededRng } from './engine.js';
import { SPELLS } from './spells.js';
import { viewerName } from './names.js';
import { createSfx } from './sfx.js';

const params = new URLSearchParams(location.search);
const LOCAL = params.has('local');
const sfx = createSfx({ muted: params.has('mute') });

const $ = (id) => document.getElementById(id);
const stage = $('stage'), unitsEl = $('units'), fxEl = $('fx');

// Where each unit stands on the 1280x720 stage (center of its sprite) and how big it is.
const LAYOUT = {
  drake: { x: 250, y: 255, size: 210 }, imp1: { x: 120, y: 440, size: 100 }, bat: { x: 400, y: 410, size: 88, fly: true },
  brom: { x: 970, y: 170, size: 96 }, cass: { x: 1050, y: 268, size: 96 }, aria: { x: 1000, y: 366, size: 96 }, dex: { x: 1070, y: 462, size: 96 }
};

const units = {};            // id -> renderer state (what is SHOWN, driven by events)
const lastAt = {};           // id -> when its last event was scheduled, so per-unit order never inverts
let haveState = false;

// ---------------- fit the 1280x720 stage into whatever size OBS or the window gives us ----------------
const fit = () => {
  const s = Math.min(innerWidth / 1280, innerHeight / 720);
  stage.style.setProperty('--s', s);
  stage.style.setProperty('--tx', `${(innerWidth - 1280 * s) / 2}px`);
  stage.style.setProperty('--ty', `${(innerHeight - 720 * s) / 2}px`);
};
addEventListener('resize', fit);
fit();

// ---------------- building the scene from a snapshot ----------------
const build = (snapshot) => {
  unitsEl.innerHTML = '';
  for (const s of snapshot.units) {
    const pos = LAYOUT[s.id];
    const el = document.createElement('div');
    el.className = `unit ${s.side}${pos.fly ? ' fly' : ''}${s.ko ? ' ko' : ''}`;
    el.style.cssText = `left:${pos.x}px;top:${pos.y}px;--size:${pos.size}px;--bob:${(-Math.random() * 2).toFixed(2)}s`;
    el.innerHTML = '<div class="shadow"></div><div class="body"><span class="glyph"></span></div>';
    el.querySelector('.glyph').textContent = s.emoji;
    if (s.boss) el.classList.add('boss');
    unitsEl.appendChild(el);
    units[s.id] = { ...s, el, pos };
  }
  haveState = true;
  renderHud();
};

// ---------------- HUD ----------------
const renderHud = () => {
  const all = Object.values(units);
  $('party').innerHTML = all.filter((u) => u.side === 'hero').map((u) => {
    const low = !u.ko && u.hp < u.maxHp * 0.25;
    return `<div class="row${u.hasted ? ' hasted' : ''}" data-id="${u.id}"><span class="nm">${u.name}</span>
      <span class="atb${u.atb >= 100 ? ' full' : ''}"><i style="width:${u.ko ? 0 : u.atb}%"></i></span>
      <span class="stats"><span class="hp${low ? ' low' : ''}${u.ko ? ' ko' : ''}">${u.hp}/${u.maxHp}</span>${u.maxMp ? `<span class="mp">${u.mp}</span>` : ''}</span></div>`;
  }).join('');
  $('enemies').innerHTML = all.filter((u) => u.side === 'enemy').map((u) => `<div class="${u.ko ? 'down' : ''}">${u.name}</div>`).join('');
  const boss = all.find((u) => u.boss);
  if (boss) {
    $('boss-name').textContent = boss.name;
    $('boss-fill').style.width = `${(boss.hp / boss.maxHp) * 100}%`;
    $('boss-hp').textContent = `${boss.hp}`;
    $('boss').classList.toggle('ko', boss.ko);
  }
};

// Gauges and MP come from the server's twice-a-second snapshots. HP and knock-outs do NOT: those come from events,
// so the numbers change exactly when the animation hits instead of a moment before it.
const applySnapshot = (snapshot) => {
  if (!haveState) return build(snapshot);
  for (const s of snapshot.units) {
    const u = units[s.id];
    if (!u) continue;
    u.atb = s.atb; u.mp = s.mp; u.maxMp = s.maxMp;
  }
  renderHud();
};

// ---------------- effects ----------------
const centerOf = (u, dy = 0) => ({ x: u.pos.x, y: u.pos.y + dy });
const spawn = (cls, text, x, y, extra = {}) => {
  const el = document.createElement('div');
  el.className = cls;
  if (text !== undefined) el.textContent = text;
  el.style.left = `${x}px`; el.style.top = `${y}px`;
  for (const [k, v] of Object.entries(extra)) el.style.setProperty(k, v);
  fxEl.appendChild(el);
  setTimeout(() => el.remove(), 1600);
  return el;
};
const sparkles = (u, glyph, n = 8) => {
  for (let i = 0; i < n; i++) {
    const a = (i / n) * Math.PI * 2;
    spawn('spark', glyph, u.pos.x, u.pos.y, { '--dx': `${Math.cos(a) * 70}px`, '--dy': `${Math.sin(a) * 70 - 20}px` });
  }
};
const flash = (color) => { const f = $('flash'); f.style.background = color; f.classList.remove('go'); void f.offsetWidth; f.classList.add('go'); };
const shake = () => { stage.classList.remove('shake'); void stage.offsetWidth; stage.classList.add('shake'); };
const pulse = (u, cls, ms) => { u.el.classList.add(cls); setTimeout(() => u.el.classList.remove(cls), ms); };

let messageTimer = 0, lastActAt = 0, messageLockUntil = 0;
const message = (text, cls = '') => {
  const now = performance.now();
  // A viewer's spell holds the banner for a moment so it can actually be read; the fight's own messages wait.
  if (cls !== 'viewer' && now < messageLockUntil) return;
  if (cls === 'viewer') messageLockUntil = now + 2200;
  const m = $('message');
  $('message-text').textContent = text;
  m.className = `win on ${cls}`;
  clearTimeout(messageTimer);
  messageTimer = setTimeout(() => m.classList.remove('on'), cls === 'viewer' ? 2300 : 1900);
};

const ticker = [];
const pushTicker = (who, spell) => {
  const def = SPELLS[spell];
  ticker.push(`${def ? def.icon : '✨'} ${who} cast ${def ? def.name : spell}`);
  if (ticker.length > 3) ticker.shift();
  const box = $('ticker');
  box.replaceChildren(...ticker.map((t) => { const d = document.createElement('div'); d.textContent = t; return d; }));  // textContent: nothing a viewer controls is ever parsed as HTML
};

// ---------------- playing one event ----------------
let hastedAt = 0;
const apply = (e) => {
  const u = units[e.target || e.actor];
  switch (e.t) {
    case 'act': {
      if (!u) return;
      lastActAt = performance.now();
      message(e.name);
      u.mp = e.mpAfter;
      if (e.kind === 'lunge') { sfx.swing(); pulse(u, 'lunge', 380); } else { sfx.cast(); pulse(u, 'casting', 650); }
      break;
    }
    case 'damage': {
      if (!u) return;
      u.hp = e.hpAfter;
      pulse(u, 'hit', 140);
      const kind = e.kind;
      const c = centerOf(u, -u.pos.size * 0.15);
      spawn(`num ${kind}`, String(e.amount), c.x, c.y);
      if (kind === 'fire') { spawn('burst', '🔥', c.x, c.y); sfx.fire(); }
      else if (kind === 'thunder') { spawn('bolt', undefined, c.x, c.y - 20); flash('#fff8c0'); sfx.thunder(); }
      else if (kind === 'meteor') {
        spawn('meteor', '🌑', c.x, c.y, { '--x': `${c.x}px`, '--y': `${c.y}px` });
        setTimeout(() => { spawn('burst', '💥', c.x, c.y); flash('#ff9a4a'); shake(); }, 540);
        sfx.meteor();
      } else if (kind === 'crit') { spawn('burst', '✨', c.x, c.y); sfx.crit(); shake(); }
      else { sfx.hit(); if (u.side === 'hero' && e.amount > 25) shake(); }
      renderHud();
      break;
    }
    case 'heal': {
      if (!u) return;
      u.hp = e.hpAfter;
      const c = centerOf(u, -u.pos.size * 0.15);
      if (e.amount > 0) spawn('num heal', `+${e.amount}`, c.x, c.y);
      sparkles(u, '✨', 6);
      sfx.heal();
      renderHud();
      break;
    }
    case 'ko': {
      if (!u) return;
      u.ko = true; u.atb = 0; u.hasted = false;
      u.el.classList.remove('rising', 'hasted');
      u.el.classList.add('ko');
      sfx.ko();
      renderHud();
      break;
    }
    case 'revive': {
      if (!u) return;
      u.ko = false; u.hp = e.hpAfter;
      u.el.classList.remove('ko');
      pulse(u, 'rising', 1000);
      const c = centerOf(u);
      spawn('ring', undefined, c.x, c.y);
      spawn('num revive', `+${e.amount}`, c.x, c.y - u.pos.size * 0.3);
      spawn('label', 'REVIVE', c.x, c.y - u.pos.size * 0.75);
      sparkles(u, '💚', 8);
      flash('#5dff8a55');
      sfx.revive();
      renderHud();
      break;
    }
    case 'status': {
      if (!u) return;
      u.hasted = e.on;
      u.el.classList.toggle('hasted', e.on);
      if (e.on && performance.now() - hastedAt > 500) { hastedAt = performance.now(); sfx.haste(); }
      renderHud();
      break;
    }
    case 'viewer': {
      const def = SPELLS[e.spell];
      message(`${def ? def.icon : '✨'} ${e.who} casts ${def ? def.name.toUpperCase() : e.spell}!`, 'viewer');
      pushTicker(e.who, e.spell);
      sfx.viewer();
      break;
    }
    case 'msg': {
      if (performance.now() - lastActAt < 700 && !/rises|gets back/.test(e.text)) return;  // the action's own name is already showing
      message(e.text, /rises|gets back/.test(e.text) ? 'green' : '');
      break;
    }
  }
};

// Schedule each event at now + delay, but never before an earlier event for the same unit.
const play = (events) => {
  const now = performance.now();
  for (const e of events) {
    const key = e.target || e.actor || '*';
    const at = Math.max(now + (e.delay || 0), (lastAt[key] || 0) + 1);
    lastAt[key] = at;
    setTimeout(() => apply(e), at - now);
  }
};

// ---------------- sources ----------------
const connectLive = () => {
  const es = new EventSource('/events');
  es.addEventListener('snapshot', (m) => applySnapshot(JSON.parse(m.data)));
  es.addEventListener('events', (m) => { if (haveState) play(JSON.parse(m.data)); });
  // Only a scene opened on the server's own machine is told the code (see /info); the terminal prints it too.
  fetch('/info').then((r) => r.json()).then((i) => {
    $('cast-url').textContent = i.castUrl.replace(/^https?:\/\//, '');
    $('join-code').textContent = i.code ?? '----';
    if (!i.code) $('cast-url').textContent = 'code: see the terminal';
  }).catch(() => {});
};

const startLocal = () => {
  const battle = createBattle({ rng: seededRng(Number(params.get('seed') || Date.now())) });
  const warp = Number(params.get('warp') || 0);
  for (let ms = 0; ms < warp * 1000; ms += 50) battle.step(50);
  build(battle.snapshot());
  setInterval(() => play(battle.step(50)), 50);
  setInterval(() => applySnapshot(battle.snapshot()), 500);
  const keys = Object.keys(SPELLS);
  addEventListener('keydown', (ev) => {
    const name = keys[Number(ev.key) - 1];
    if (!name) return;
    const r = battle.cast(SPELLS[name].effect, viewerName('local-preview'), name);
    if (r.error) message(r.error); else play(r.events);
  });
  // &autocast=1,3 fires those spells (by key number) shortly after load, handy for screenshots and demos
  for (const n of (params.get('autocast') || '').split(',').filter(Boolean)) {
    setTimeout(() => keys[Number(n) - 1] && play((battle.cast(SPELLS[keys[Number(n) - 1]].effect, viewerName('local-preview'), keys[Number(n) - 1]).events) || []), Number(params.get('at') || 300));
  }
  $('join-code').textContent = 'DEMO';
  $('cast-url').textContent = 'preview: press 1-5 to cast';
};

$('cast-spells').innerHTML = Object.values(SPELLS).map((s) => `${s.icon} ${s.name} <span>${s.cost} MP</span>`).join('<br>');

// Sound: browsers wait for a click before allowing audio. OBS's browser source doesn't, so the banner never shows there.
const soundBanner = $('sound');
const unlock = () => { sfx.unlock(); soundBanner.hidden = true; };
addEventListener('pointerdown', unlock);
addEventListener('keydown', unlock);
if (sfx.available) {
  sfx.unlock();
  setTimeout(() => { if (!sfx.running) soundBanner.hidden = false; }, 1200);
}

if (LOCAL) startLocal(); else connectLive();
