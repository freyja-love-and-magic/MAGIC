import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import sessionlessNode from 'sessionless-node';
import {
  Identity, verify, verifyEnvelope, signTimestamped, verifyTimestamped, memoryStore
} from '../src/index.js';

const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'sl-kit-'));

test('create, save, reload gives the same identity', async () => {
  const file = path.join(tmp(), 'sub', 'keys.json');
  const a = await Identity.create({ path: file });
  const b = await Identity.load({ path: file });
  assert.equal(a.pubKey, b.pubKey);
  assert.equal(a.exportPrivateKey(), b.exportPrivateKey());
});

test('key file is owner-only', async () => {
  const dir = tmp();
  const file = path.join(dir, 'keys.json');
  await Identity.create({ path: file });
  assert.equal(fs.statSync(file).mode & 0o777, 0o600);
});

test('loadOrCreate is idempotent; create refuses to overwrite', async () => {
  const file = path.join(tmp(), 'keys.json');
  const a = await Identity.loadOrCreate({ path: file });
  const b = await Identity.loadOrCreate({ path: file });
  assert.equal(a.pubKey, b.pubKey);
  await assert.rejects(Identity.create({ path: file }), /refusing to overwrite/);
});

test('load fails clearly when nothing exists', async () => {
  await assert.rejects(Identity.load({ path: path.join(tmp(), 'nope.json') }), /No keys found/);
});

test('named identities use SESSIONLESS_HOME', async () => {
  const dir = tmp();
  process.env.SESSIONLESS_HOME = dir;
  try {
    await Identity.create({ name: 'caster' });
    assert.ok(fs.existsSync(path.join(dir, 'caster.json')));
    await assert.rejects(Identity.create({ name: '../evil' }), /Invalid identity name/);
  } finally {
    delete process.env.SESSIONLESS_HOME;
  }
});

test('corrupt key files fail loudly and are not overwritten', async () => {
  const file = path.join(tmp(), 'keys.json');
  fs.writeFileSync(file, '{not json');
  await assert.rejects(Identity.loadOrCreate({ path: file }), /not valid JSON/);
  assert.equal(fs.readFileSync(file, 'utf-8'), '{not json');

  const a = await Identity.ephemeral();
  fs.writeFileSync(file, JSON.stringify({ privateKey: a.exportPrivateKey(), pubKey: '02'.padEnd(66, '1') }));
  await assert.rejects(Identity.load({ path: file }), /does not match/);
});

test('sign and verify round trip; tampering fails', async () => {
  const id = await Identity.ephemeral();
  const sig = await id.sign('hello');
  assert.equal(sig.length, 128);
  assert.equal(verify(sig, 'hello', id.pubKey), true);
  assert.equal(id.verify(sig, 'hello'), true);
  assert.equal(verify(sig, 'hellO', id.pubKey), false);
  const other = await Identity.ephemeral();
  assert.equal(verify(sig, 'hello', other.pubKey), false);
});

test('verify returns false for garbage, throws for non-strings', () => {
  assert.equal(verify('zz', 'm', 'zz'), false);
  assert.equal(verify('', '', ''), false);
  assert.throws(() => verify(null, 'm', 'k'), TypeError);
});

test('identities are independent (no global state)', async () => {
  const a = await Identity.ephemeral();
  const b = await Identity.ephemeral();
  assert.notEqual(a.pubKey, b.pubKey);
  assert.equal(a.verify(await a.sign('x'), 'x'), true);
  assert.equal(a.verify(await b.sign('x'), 'x'), false);
});

test('import restores identity and refuses different keys', async () => {
  const src = await Identity.ephemeral();
  const file = path.join(tmp(), 'keys.json');
  const imported = await Identity.import(src.exportPrivateKey(), { path: file });
  assert.equal(imported.pubKey, src.pubKey);
  await Identity.import(src.exportPrivateKey(), { path: file }); // same key: fine
  const other = await Identity.ephemeral();
  await assert.rejects(Identity.import(other.exportPrivateKey(), { path: file }), /refusing to overwrite/);
  await assert.rejects(Identity.import('abc', { path: file }), /64 hex/);
});

test('sessionless-node style saveKeys/getKeys', async () => {
  let held = null;
  const a = await Identity.loadOrCreate({ saveKeys: (k) => { held = k; }, getKeys: () => held });
  assert.equal(held.pubKey, a.pubKey);
  const b = await Identity.loadOrCreate({ saveKeys: () => assert.fail('must not resave'), getKeys: () => held });
  assert.equal(b.pubKey, a.pubKey);
});

test('private key is not leaked by JSON or inspect', async () => {
  const id = await Identity.ephemeral();
  const priv = id.exportPrivateKey();
  assert.ok(!JSON.stringify(id).includes(priv));
  assert.ok(!(await import('node:util')).inspect(id).includes(priv));
});

test('interop with sessionless-node, both directions', async () => {
  for (let i = 0; i < 200; i++) {
    const msg = `message ${i} ✨ ${Date.now()}`;

    // kit signs -> sessionless-node verifies
    const kit = await Identity.ephemeral();
    assert.equal(sessionlessNode.verifySignature(await kit.sign(msg), msg, kit.pubKey), true);

    // sessionless-node signs -> kit verifies
    let keys;
    await sessionlessNode.generateKeys((k) => { keys = k; }, () => keys);
    assert.equal(verify(await sessionlessNode.sign(msg), msg, keys.pubKey), true);
  }
});

test('envelopes', async () => {
  const id = await Identity.ephemeral();
  const env = await id.signEnvelope('pay 5');
  assert.equal(verifyEnvelope(env), true);
  assert.equal(verifyEnvelope(env, { pubKey: id.pubKey, message: 'pay 5' }), true);
  assert.equal(verifyEnvelope(env, { pubKey: (await Identity.ephemeral()).pubKey }), false);
  assert.equal(verifyEnvelope({ ...env, message: 'pay 500' }), false);
  assert.equal(verifyEnvelope(null), false);
  assert.equal(verifyEnvelope({ message: 'x' }), false);
});

test('timestamped signatures enforce freshness and field binding', async () => {
  const id = await Identity.ephemeral();
  const signed = await signTimestamped(id, 'uuid-1', 50);
  assert.equal(verifyTimestamped(signed, ['uuid-1', 50]), true);
  assert.equal(verifyTimestamped(signed, ['uuid-1', 51]), false);
  assert.equal(verifyTimestamped(signed, ['uuid-1', 50], { now: Date.now() + 120_000 }), false);
  assert.equal(verifyTimestamped({ ...signed, timestamp: 'abc' }, ['uuid-1', 50]), false);
});

test('works as a magic-gateway-js signer', async () => {
  const id = await Identity.ephemeral();
  const sig = await id.sign('1234uuid20' + 1); // gateway signer only needs .sign(message)
  assert.equal(id.verify(sig, '1234uuid201'), true);
});

test('CLI keygen / sign / verify / export / import', () => {
  const cli = path.resolve('bin/cli.js');
  const file = path.join(tmp(), 'cli.json');
  const run = (...a) => execFileSync('node', [cli, ...a, '--path', file], { encoding: 'utf-8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();

  const pubKey = run('keygen');
  assert.match(pubKey, /^0[23][0-9a-f]{64}$/);
  assert.equal(run('pubkey'), pubKey);
  const sig = run('sign', 'hi');
  assert.equal(execFileSync('node', [cli, 'verify', sig, 'hi', pubKey], { encoding: 'utf-8' }).trim(), 'valid');
  const bad = spawnSync('node', [cli, 'verify', sig, 'bye', pubKey], { encoding: 'utf-8' });
  assert.equal(bad.status, 1);
  assert.equal(bad.stdout.trim(), 'invalid');

  const priv = run('export');
  const file2 = path.join(tmp(), 'other.json');
  const out = execFileSync('node', [cli, 'import', priv, '--path', file2], { encoding: 'utf-8' }).trim();
  assert.equal(out, pubKey);
  assert.equal(spawnSync('node', [cli, 'keygen', '--path', file]).status, 2); // refuses overwrite
});

// ---- browser entry -------------------------------------------------------------------------------------------

test('the browser entry never reaches a Node built-in', () => {
  const root = path.resolve('src');
  const seen = new Set();
  const walk = (file) => {
    if (seen.has(file)) return;
    seen.add(file);
    const src = fs.readFileSync(file, 'utf8');
    assert.ok(!/from ['"]node:/.test(src) && !/import\(['"]node:/.test(src), `${path.relative(root, file)} imports a Node built-in`);
    for (const m of src.matchAll(/(?:from|import\()\s*['"](\.[^'"]+)['"]/g)) walk(path.resolve(path.dirname(file), m[1]));
  };
  walk(path.join(root, 'browser.js'));
  assert.ok(seen.size >= 4, 'it should have followed its imports');
  assert.ok(![...seen].some((f) => f.endsWith('stores-node.js')), 'file storage must not be part of the browser build');
});

test('in a browser (no file storage registered) keys live in the store you give it, and file storage fails clearly', () => {
  // A fresh process, so nothing has registered Node file storage (the Node entry does that when imported).
  const script = `
    import { Identity, localStorageStore } from ${JSON.stringify(path.resolve('src/browser.js'))};
    const data = new Map();
    const storage = { getItem: (k) => data.get(k) ?? null, setItem: (k, v) => data.set(k, v) };
    const a = await Identity.loadOrCreate({ store: localStorageStore({ key: 'k', storage }) });
    const b = await Identity.loadOrCreate({ store: localStorageStore({ key: 'k', storage }) });
    if (a.pubKey !== b.pubKey) throw new Error('keys were not kept');
    const sig = await a.sign('hi');
    if (!b.verify(sig, 'hi')) throw new Error('signature did not verify');
    let message = '';
    try { await Identity.loadOrCreate({ name: 'x' }); } catch (e) { message = e.message; }
    if (!/only available in Node/.test(message)) throw new Error('unclear error: ' + message);
    console.log('ok');
  `;
  const out = execFileSync('node', ['--input-type=module', '-e', script], { encoding: 'utf8' }).trim();
  assert.equal(out, 'ok');
});

test('the Node entry still gives file storage by name', async () => {
  const dir = tmp();
  process.env.SESSIONLESS_HOME = dir;
  try {
    const a = await Identity.loadOrCreate({ name: 'viaIndex' });
    assert.ok(fs.existsSync(path.join(dir, 'viaIndex.json')));
    assert.equal((await Identity.loadOrCreate({ name: 'viaIndex' })).pubKey, a.pubKey);
  } finally { delete process.env.SESSIONLESS_HOME; }
});
