# sessionless-kit

Sessionless keys in a few lines: create, save, sign, verify. No sessions, no global state, no storage code to write.

Wire-compatible with `sessionless-node` (keccak256 of the UTF-8 message, secp256k1, 128-char compact hex signature). The test suite cross-verifies 200 signatures in each direction.

## Quick start

```js
import { Identity, verify } from 'sessionless-kit';

const me = await Identity.loadOrCreate({ name: 'caster' }); // ~/.sessionless/caster.json, created on first run
const signature = await me.sign('hello');

verify(signature, 'hello', me.pubKey); // true. Needs no keys, so it works on any server.
```

Run it again and you get the same identity back.

## Identity

| | |
|---|---|
| `Identity.loadOrCreate(opts)` | Load keys, or create and save them on first use |
| `Identity.load(opts)` | Load keys, fail if none exist |
| `Identity.create(opts)` | Create keys, fail if some already exist (never overwrites) |
| `Identity.import(privateKeyHex, opts)` | Save an existing private key (recovery, moving devices) |
| `Identity.ephemeral()` | In-memory keys, never saved (tests, throwaway callers) |
| `id.pubKey` | Public key hex |
| `id.sign(message)` | Signature hex |
| `id.verify(sig, message)` | Verify against this identity's key |
| `id.exportPrivateKey()` | Private key hex for backup. Treat it like a password |

`opts` picks where keys live. Use one of:

- `{ name }` stores at `~/.sessionless/<name>.json` (`SESSIONLESS_HOME` changes the directory)
- `{ path }` stores in an explicit file
- `{ store }` uses any `{ load(), save(keys) }` object. Built in: `fileStore`, `memoryStore`, `localStorageStore`, `customStore`
- `{ saveKeys, getKeys }` takes the same callbacks `sessionless-node` uses, so existing storage code keeps working (Keychain, a database, NVS, and so on)

Key files are written atomically, with the directory at `0700` and the file at `0600`.

## Verifying

```js
import { verify, verifyEnvelope, signTimestamped, verifyTimestamped } from 'sessionless-kit';

verify(sig, message, pubKey);            // boolean. Returns false for malformed input, never throws on it.

const env = await me.signEnvelope('pay 5');            // { message, signature, pubKey }
verifyEnvelope(env, { pubKey: knownKey });             // pin the signer whenever you know who to expect

const signed = await signTimestamped(me, uuid, cost);  // signs Date.now() + uuid + cost
verifyTimestamped(signed, [uuid, cost], { maxAgeMs: 60_000 }); // rejects stale (replayed) and altered requests
```

An envelope's own `pubKey` only proves that the holder of *that* key signed. It doesn't prove who they are, so pass `{ pubKey }` when you have an expected signer.

## With MAGIC

An `Identity` already has the `sign()` method `magic-gateway-js` wants as its `signer`, so a gateway or caster no longer needs the `generateKeys(() => {}, getKeys)` priming step:

```js
const me = await Identity.loadOrCreate({ name: 'my-gateway' });
gateway.expressApp(app, fountUser, spellbook, 'my-stop', me, extraForGateway, onSuccess);
```

A caster signs the spell the same way `caster.js` in `src/streaming/demo` does: `await me.sign(timestamp + spell + uuid + totalCost + mp + ordinal)`. Unlike the demo's throwaway key, `loadOrCreate` keeps the identity (and its fount registration) across runs.

## CLI

```
npx sessionless-kit keygen [--name n | --path file]
npx sessionless-kit pubkey
npx sessionless-kit sign "hello"          # '-' reads stdin
npx sessionless-kit verify <sig> "hello" <pubKey>   # prints valid/invalid, exit code 0/1
npx sessionless-kit export | import <privateKey>
```

## Not included (yet)

- **Encryption at rest.** Keys are plain hex in an owner-only file, the same as `ssh` keys without a passphrase. Use `saveKeys`/`getKeys` or a custom `store` to put them in a Keychain or KMS.
- **Other languages.** This is the JS/Node layer. The Rust, Swift and embedded implementations need the same treatment.

## In a browser

```js
import { Identity, localStorageStore, signTimestamped } from 'sessionless-kit/browser';
const me = await Identity.loadOrCreate({ store: localStorageStore({ key: 'my-app-keys' }) });
```

`sessionless-kit/browser` is the same API without Node file storage (asking it for `{ name }` or `{ path }` throws a clear error). It bundles with esbuild into about 40 KB with no Node built-ins, and a test checks that its import graph never touches one. It is used by the viewer page in `MAGIC/src/games/endless-battle`, and was exercised there in real Chromium (keys kept across reloads, signatures accepted by fount). Keys in `localStorage` are readable by any script on that origin, so treat them as low-value (a viewer's wand), not as a vault.

## Tests

`npm test`
