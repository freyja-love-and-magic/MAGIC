# Endless Battle

A Final Fantasy-style battle scene where **no one dies, they just get back up**, built to be streamed through OBS, with a
**MAGIC gateway so viewers can cast spells that change the fight**. Four heroes (Brom, Cass, Aria, Dex) fight an Ember
Drake and two minions in an Active Time Battle. Damage numbers fly, a knocked-out unit lies there a few seconds and then
rises again in **green** with a REVIVE burst, and the sounds are small synthesized effects (no audio files).

```
viewer's phone                this server                         fount
 /cast page    --signed-->   /magic/spell/arenaFire  --signed-->  resolves, spends the viewer's MP
 (keys live in               checks join code, cost, replay,                 |
  the browser;               cooldown, can-it-land, then adds                v
  types the join code)       its own signature
                                  |                                  {success:true}
                                  v
                           the battle changes ---SSE---> scene (OBS Browser Source)
```

## Try it in 10 seconds (no servers)

Open `public/scene.html?local=1` in a browser (serve `public/` with anything, e.g. `cd public && python3 -m http.server 8080`,
then `http://localhost:8080/scene.html?local=1`). The engine runs in the page. Press **1-5** to cast Fire, Heal, Revive,
Haste, Meteor. `&seed=7&warp=90` starts a specific fight 90 seconds in, `&mute=1` silences it.

## Run it for real

1. **fount** (the MAGIC resolver): `cd fount/src/server/node && LOCALHOST=true node fount.js` (port 3006). Use a fount that
   includes the signature fix in `src/routes/magic.js`: without it a viewer could cast *as another viewer*. The tests below
   check that a forged signature is refused.
2. **This server:**
   ```
   cp config.js.example config.js      # then edit FOUNT_BASE / PORT / PUBLIC_URL if needed
   npm install
   npm start                           # builds the viewer page, registers with fount, starts the fight
   ```
3. **OBS:** add a **Browser** source with URL `http://localhost:3300/`, width 1280, height 720 (1920x1080 also works: the
   scene scales to fit). Tick **Control audio via OBS** so the sound effects show up in the audio mixer. Then stream to
   Twitch the usual way (Settings > Stream).
4. **Let viewers in.** Twitch viewers can't reach `localhost`, so expose the server and tell the scene its public address:
   ```
   cloudflared tunnel --url http://localhost:3300      # or ngrok http 3300
   ```
   Put the `https://...` address it prints in `config.js` as `PUBLIC_URL` and restart. (On your home Wi-Fi, for testing on
   your own phone, use your Mac's address instead: `http://<its IP>:3300`.)

**How viewers join.** The stream shows an address and a big **join code** (like `5Z7J`; no 0/O/1/I/L so it can't be misread).
A viewer opens the address on their phone, types the code once, and gets the spell buttons. Casting a spell happens on stream
a moment later, credited to a name like "Brave Otter". The terminal also prints a **one-tap link**,
`<address>/cast?code=5Z7J`, handy to post in chat: it skips typing the code.

The code is random each time the server starts. To keep it the same between shows, set `JOIN_CODE` in `config.js` (4-8 letters
or digits). If you restart and it changes, viewers who had joined are asked for the new one automatically.

## The spells

| Spell | MP | Recharge (all viewers share it) | What it does |
|---|---|---|---|
| Fire | 50 | 2.5 s | 70-95 damage to the dragon |
| Heal | 40 | 4 s | +60 HP to every hero still standing (refused if the party is healthy) |
| Revive | 80 | 4 s | raises every fallen hero at 70% HP, in green (refused if no one has fallen) |
| Haste | 60 | 12 s | party fills its ATB gauges 1.8x faster for 12 s |
| Meteor | 200 | 15 s | 95-130 damage to every enemy standing |

MP is the viewer's own and is spent at fount. A spell that couldn't land (nothing to revive, on recharge, bad cost) is
refused **before** any MP is spent. Edit `public/js/spells.js` to change costs and recharges, and `public/js/engine.js` for
damage, speed and revive timing.

## What the gateway enforces, and why

fount verifies the caster's signature and spends the MP, but it does **not** check that the cost signed into a spell is the
price, nor that the spell could land, nor repeats. So this gateway (`src/arena.js`) does, before anything reaches fount:

- the **join code** from the stream is required for registering, reading MP and casting (so casting stays with people who are
  watching), and wrong guesses are rate limited per address (10 a minute). The scene is only told the code when it is opened on
  the server's own machine; anyone else asking gets nothing, and the terminal prints it too;
- the cost in the signed spell must equal the spell's price (a viewer can't sign `totalCost: 0`);
- the spell must arrive with **no `gateways`** (fount pays rewards to every gateway it lists, so a caster must not invent them);
- no unexpected fields, a fresh timestamp (60 s), a strictly increasing `ordinal` per viewer (replays are refused), and a
  recharge window that is released if the cast doesn't land;
- the spell must be able to land in the current fight.

Viewers never type a name: the name on stream is derived from their uuid, so nothing a viewer controls appears on a live
stream (no moderation problem, and nothing to look anyone up by). Registration goes through `/register` (rate limited per IP)
and MP through `/mp`, so fount itself never has to be reachable from the internet. A gateway also earns fount's gateway
reward on every spell that resolves through it.

## Tests

```
npm test               # engine (offline) + gateway (incl. the join code) against a REAL fount from a throwaway copy
npm run test:engine    # just the engine, no fount needed
npm run test:live      # real fount + real server + three real browsers, through the join-code flow (needs: npm i --no-save playwright && npx playwright install chromium)
npm run test:sound     # renders every sound offline and checks level and length
```

The gateway tests start your fount from a copy (so its `data/` is untouched) and need port 3006 free; set `FOUNT_DIR` if fount
isn't at `../../../../fount/src/server/node`.

## Verified (on the machine this was built on)

- **Engine:** 14 tests. Twenty simulated minutes across five seeds: nobody stays down longer than 10.5 s, every knock-out is
  followed by a revive, the fight never goes quiet for more than 12 s, and a shadow state rebuilt only from the emitted events
  always equals the engine's real state (so the scene can't drift from the truth). About 54 actions a minute, a hero going down
  every ~22 s, the dragon every ~75 s.
- **Gateway against the real fount:** 18 tests (13 on casting plus 5 on the join code: required on every endpoint, forgiving about case and spaces, rate limited, and not leaked to remote visitors). Real signed casts: the right MP is charged, underpaying and overpaying are
  refused, a forged signature is refused and the viewer isn't charged, replays are refused, shared recharge, two simultaneous
  casts land exactly one, spells that can't land cost nothing, an out-of-MP viewer is refused without touching the fight, and
  malformed or hostile requests are rejected.
- **Whole stack in real Chromium:** a scene page and two phone-sized viewer pages. A wrong code is refused, the right one (typed
  lowercase with a space) gets in, a chat link skips typing, and a stale code after a restart asks again. The viewer registers, sees 1000 MP, casts
  Fire, the dragon loses HP on the scene, the ticker credits the viewer, MP drops to 950 and survives a reload, a second device
  is a different viewer, and there are no console errors.
- **Look:** screenshots of the live scene (with the join code) and the phone's join and cast screens, including revive, meteor, fire and haste mid-animation.
- **Sound:** every effect renders audibly, below clipping, and ends within 2.5 s. (This checks they work, not that they sound nice.)

## Not verified

- **OBS and Twitch.** OBS isn't installed here, so the Browser Source setup and streaming to Twitch are untested. In particular
  I'm relying on OBS's browser source allowing audio without a click (the scene shows a "click to enable sound" banner only in
  a normal browser that blocks it).
- **A human listening.** I can't hear the effects.
- **Tunnels and real phones.** The viewer page was tested in a phone-sized Chromium (including over plain `http://` on the LAN
  address, an insecure context, which is how a phone on your Wi-Fi sees it), not on real devices, and not through cloudflared/ngrok.
- **A hosted fount.** Tests use a local fount. A deployed fount may not have the signature fix, and viewers' MP regeneration
  there is whatever that fount does.
- **Load.** One scene and a handful of viewers. The SSE hub and the in-memory replay table are simple by design.
- **Art.** Sprites are emoji, which look different on Windows and Mac. Swap in your own art by changing `LAYOUT` and the
  `.glyph` rendering in `public/js/scene.js`.
