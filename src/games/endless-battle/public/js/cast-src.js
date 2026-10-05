// The viewer's cast page. Keys are made and kept in this browser (sessionless-kit), the viewer is registered through
// the gateway, and every spell is signed here before it is sent. Source for public/js/cast.bundle.js.
import { Identity, localStorageStore, signTimestamped } from 'sessionless-kit/browser';
import { SPELLS } from './spells.js';
import { viewerName } from './names.js';

const $ = (id) => document.getElementById(id);
const STORE = 'endless-battle';
const storage = (() => { try { return globalThis.localStorage; } catch { return null; } })();
const get = (k) => { try { return storage.getItem(`${STORE}:${k}`); } catch { return null; } };
const set = (k, v) => { try { storage.setItem(`${STORE}:${k}`, v); } catch { /* private mode: the viewer just re-registers next visit */ } };

let me = null, uuid = null, code = null;
const cooldowns = {};    // spell -> ms timestamp when it can be cast again (shown, not trusted: the server decides)
const buttons = {};
let busy = false;

const say = (text, kind = '') => { const s = $('status'); s.textContent = text; s.className = kind; };

// Every request carries the join code from the stream. If the server says it's wrong (the host restarted with a new
// one), the code is forgotten and the join screen comes back, instead of leaving the viewer with broken buttons.
const api = async (path, init = {}) => {
  const r = await fetch(path, { ...init, headers: { ...(init.headers || {}), 'X-Join-Code': code || '' } });
  let body = null;
  try { body = await r.json(); } catch { /* non-JSON error page from a proxy */ }
  const result = { status: r.status, body: body || {} };
  if (r.status === 403 && /join code/i.test(result.body.error || '')) { showJoin('That code no longer works. Check the stream for the current one.'); throw new Error('join code rejected'); }
  return result;
};

const showJoin = (message = '') => {
  code = null;
  set('code', '');
  $('game').hidden = true;
  $('join').hidden = false;
  $('join-error').textContent = message;
  $('join-go').disabled = false;
  $('code').focus();
};

const submitCode = async (e) => {
  e.preventDefault();
  const typed = $('code').value.trim();
  if (!typed) return;
  $('join-go').disabled = true;
  $('join-error').textContent = '';
  try {
    const r = await fetch('/join', { method: 'POST', headers: { 'X-Join-Code': typed } });
    const body = await r.json().catch(() => ({}));
    if (r.status === 200) { code = typed; set('code', typed); $('join').hidden = true; begin(); return; }
    $('join-error').textContent = r.status === 429 ? body.error : "That isn't the code on the stream. Check it and try again.";
  } catch {
    $('join-error').textContent = "Couldn't reach the stream. Check your connection.";
  }
  $('join-go').disabled = false;
};

const register = async () => {
  const timestamp = Date.now().toString();
  const signature = await me.sign(timestamp + me.pubKey);
  const { status, body } = await api('/register', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ timestamp, pubKey: me.pubKey, signature })
  });
  if (status !== 200 || !body.uuid) throw new Error(body.error || `registration failed (${status})`);
  return body.uuid;
};

const refreshMp = async () => {
  if (!uuid) return;
  const timestamp = Date.now().toString();
  const signature = await me.sign(timestamp + uuid);
  const { status, body } = await api(`/mp?uuid=${encodeURIComponent(uuid)}&timestamp=${timestamp}&signature=${signature}`);
  if (status !== 200 || typeof body.mp !== 'number') return;
  $('mp-text').textContent = `${Math.floor(body.mp)} / ${body.maxMP} MP`;
  $('mp-bar').style.width = `${Math.max(0, Math.min(100, (body.mp / body.maxMP) * 100))}%`;
};

const nextOrdinal = () => {
  const n = Math.max(Number(get('ordinal') || 0) + 1, Date.now());   // always increasing, even across reloads
  set('ordinal', String(n));
  return n;
};

const paintButtons = () => {
  const now = Date.now();
  for (const [name, b] of Object.entries(buttons)) {
    const wait = Math.max(0, (cooldowns[name] || 0) - now);
    b.el.disabled = busy || wait > 0;
    b.bar.style.width = wait > 0 ? `${(wait / SPELLS[name].cooldownMs) * 100}%` : '0';
  }
};

const castSpell = async (name) => {
  const def = SPELLS[name];
  busy = true; paintButtons();
  say(`Casting ${def.name}…`);
  try {
    const ordinal = nextOrdinal();
    const signed = await signTimestamped(me, name, uuid, def.cost, true, ordinal);
    const spell = { timestamp: signed.timestamp, spell: name, casterUUID: uuid, totalCost: def.cost, mp: true, ordinal, casterSignature: signed.signature, gateways: [] };
    const { status, body } = await api(`/magic/spell/${name}`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(spell) });
    if (status === 200 && body.success) {
      cooldowns[name] = Date.now() + def.cooldownMs;
      say(`${def.icon} ${def.name}! Watch the stream.`, 'ok');
    } else {
      if (status === 429 && body.retryMs) cooldowns[name] = Date.now() + body.retryMs;
      say(body.error || `That didn't work (${status}).`, 'err');
    }
  } catch (err) {
    if (err.message !== 'join code rejected') say("Couldn't reach the stream. Check your connection.", 'err');   // else the join screen is showing
  } finally {
    busy = false; paintButtons();
    refreshMp().catch(() => {});
  }
};

const buildButtons = () => {
  const list = $('spells');
  for (const [name, def] of Object.entries(SPELLS)) {
    const el = document.createElement('button');
    el.className = 'spell'; el.type = 'button';
    el.setAttribute('aria-label', `${def.name}, ${def.cost} MP. ${def.blurb}`);
    el.innerHTML = '<span class="icon"></span><span><div class="name"></div><div class="blurb"></div></span><span class="cost"></span><span class="cool"></span>';
    el.querySelector('.icon').textContent = def.icon;
    el.querySelector('.name').textContent = def.name;
    el.querySelector('.blurb').textContent = def.blurb;
    el.querySelector('.cost').textContent = `${def.cost} MP`;
    el.disabled = true;
    el.addEventListener('click', () => castSpell(name));
    list.appendChild(el);
    buttons[name] = { el, bar: el.querySelector('.cool') };
  }
};

const begin = async () => {
  $('game').hidden = false;
  try {
    me = await Identity.loadOrCreate({ store: localStorageStore({ key: `${STORE}:keys` }) });
    uuid = get(`uuid:${me.pubKey}`);
    if (!uuid) { say('Registering your wand…'); uuid = await register(); set(`uuid:${me.pubKey}`, uuid); }
    $('who').innerHTML = 'You are <b></b>';
    $('who').querySelector('b').textContent = viewerName(uuid);
    say('');
    await refreshMp();
    paintButtons();
    if (!timers) { timers = true; setInterval(paintButtons, 250); setInterval(() => refreshMp().catch(() => {}), 8000); }
  } catch (err) {
    if (err.message === 'join code rejected') return;   // the join screen is already showing
    $('who').textContent = 'Something went wrong';
    say(`Couldn't set up your wand: ${err.message}. Reload to try again.`, 'err');
  }
};
let timers = false;

const start = () => {
  buildButtons();
  $('join-form').addEventListener('submit', submitCode);
  // A link like /cast?code=K7QM (handy in chat) skips typing. The code is removed from the address bar after.
  const fromLink = new URLSearchParams(location.search).get('code');
  if (fromLink) { history.replaceState(null, '', location.pathname); set('code', fromLink); }
  code = fromLink || get('code') || null;
  if (code) begin(); else showJoin();
};

start();
