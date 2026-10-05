// Run: cp config.js.example config.js && npm install && npm start     (then see README.md for OBS)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Identity } from 'sessionless-kit';
import { createArena } from './src/arena.js';

const here = path.dirname(fileURLToPath(import.meta.url));

// CONFIG=/path/to/other-config.js picks a different config file (handy for running several streams or for tests).
const configPath = process.env.CONFIG ? path.resolve(process.env.CONFIG) : path.join(here, 'config.js');
let config;
try {
  config = (await import(configPath)).default;
} catch (err) {
  console.error(`Can't load ${configPath}: ${err.code === 'ERR_MODULE_NOT_FOUND' ? 'copy config.js.example to config.js and adjust it' : err.message}`);
  process.exit(1);
}
config = { STOP_NAME: 'arena', DATA_DIR: path.join(here, 'data'), ...config, PUBLIC_DIR: path.join(here, 'public') };

if (!fs.existsSync(path.join(config.PUBLIC_DIR, 'js', 'cast.bundle.js'))) {
  console.error('The viewer page has not been built. Run: npm run build   (npm start does this for you)');
  process.exit(1);
}

// The gateway's own identity, kept across restarts (keys are stored owner-only by sessionless-kit).
fs.mkdirSync(config.DATA_DIR, { recursive: true, mode: 0o700 });
const identity = await Identity.loadOrCreate({ path: path.join(config.DATA_DIR, 'keys.json') });

// Registering with fount is idempotent for a given key, so it is done on every start instead of caching a uuid:
// that way pointing this at a different (or reset) fount just works.
console.log(`Registering this gateway with fount at ${config.FOUNT_BASE} ...`);
const timestamp = Date.now().toString();
let res, body;
try {
  res = await fetch(`${config.FOUNT_BASE}/user/create`, {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ timestamp, pubKey: identity.pubKey, signature: await identity.sign(timestamp + identity.pubKey) })
  });
  body = await res.json();
} catch (err) {
  console.error(`Can't reach fount at ${config.FOUNT_BASE}: ${err.message}\nStart fount first (see README.md), or fix FOUNT_BASE in config.js.`);
  process.exit(1);
}
if (!res.ok || !body.uuid) {
  console.error(`fount refused the registration (${res.status}):`, body);
  process.exit(1);
}
const fountUser = { uuid: body.uuid, ordinal: body.ordinal ?? 0 };
console.log(`Gateway registered with fount as ${fountUser.uuid}`);

const arena = createArena({ config, identity, fountUser });

let last = performance.now();
setInterval(() => {
  const t = performance.now();
  arena.tick(Math.min(250, t - last));   // cap so a stalled laptop doesn't fast-forward the fight
  last = t;
}, config.TICK_MS ?? 50);

arena.app.listen(config.PORT, () => {
  console.log(`\nendless-battle is running`);
  console.log(`  Scene for OBS (Browser Source):  http://localhost:${config.PORT}/`);
  console.log(`  Viewers cast spells at:          ${arena.castUrl}`);
  console.log(`  JOIN CODE (shown on the stream): ${arena.joinCode}`);
  console.log(`  One-tap link for chat:           ${arena.castUrl}?code=${arena.joinCode}`);
  if (!config.PUBLIC_URL) console.log(`  (PUBLIC_URL is not set, so the address on the stream only works on this machine. See README.md.)`);
});
