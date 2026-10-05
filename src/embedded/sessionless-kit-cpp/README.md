# SessionlessKit + MagicCore (C++)

Libraries in `libs/`:

| Library | What it is |
|---|---|
| **SessionlessKit** | keys: create, save, sign, verify |
| **MagicCore** | the transport-independent protocol: signed messages, spell JSON + limits, wire frames, clock, ordinal, and the two interfaces `Transport` and `Resolver` |
| **MagicWand** | the caster: learns time from a gateway, registers, signs and sends spells |
| **MagicGateway** | the relay: serves wands, adds its signed entry and `extra`, resolves through any `Resolver` |
| **MagicEsp32** | ESP32 implementations: ESP-NOW `Transport`, HTTPS `FountHttpResolver`, NVS stores |
| **MagicHost** | macOS/Linux/Pi implementation: libcurl `CurlResolver` |

Sections below: SessionlessKit, then MagicCore, then **Wand and gateway** at the end.

Sessionless keys for the hardware demos: create, save to flash, sign, verify, in a few lines. One library replaces the `lib/sessionless` copy that every demo project vendors today.

```cpp
#include <SessionlessKit.h>
#include <SessionlessNvs.h>

SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);   // see "Stack" below

sessionless::NvsKeyStore store("wand");              // flash namespace
sessionless::Identity me;

void setup() {
  WiFi.mode(WIFI_STA);                               // see "Randomness" below
  if (me.loadOrCreate(store) != sessionless::Status::Ok) { /* it tells you why */ }

  char sig[SESSIONLESS_SIGNATURE_HEX_LEN];
  me.signHex(message, sig);                          // 128 hex chars, what fount expects
}
```

Verifying needs no keys: `sessionless::verifyHex(sigHex, message, pubKeyHex)`.

Wire-compatible with `sessionless-node`: keccak256 of the message bytes, secp256k1 ECDSA with an RFC6979 deterministic nonce, low-S, 64-byte compact signature, 33-byte compressed public key.

## Use it from a demo project

```ini
; platformio.ini
lib_extra_dirs = ../sessionless-kit-cpp/libs
```

Delete the project's own `lib/sessionless` and `lib/secp256k1-embedded`. Both come from here now. Existing devices keep their identity: `NvsKeyStore` uses the same `privKey` and `pubKey` names the demos already write, so pass the demo's existing namespace (`"wand"`, ...).

## API

| | |
|---|---|
| `Identity::loadOrCreate(store, rng)` | Load, or create and save on first use. Never overwrites. |
| `Identity::load(store)` / `create(store, rng)` | Load-only / create-only (`KeysExist` if keys are there) |
| `Identity::importPrivateKeyHex(store, hex)` | Restore a known key |
| `Identity::ephemeral(rng)` | In RAM only |
| `publicKey()` / `publicKeyHex(out)` | 33 bytes / 66 hex chars |
| `sign(msg, sig)` / `signHex(msg, out)` | `msg` is a C string (or pass pointer and length) |
| `exportPrivateKeyHex(out)` | Backup. Treat as a password |
| `verify(...)` / `verifyHex(...)` | Stateless. Returns `false` on any malformed input |
| `toHex` / `fromHex` | Lowercase out; strict length and charset on input |

Every fallible call returns a `Status` (`statusName(s)` for the text). Nothing throws, and nothing falls back silently.

Storage: `NvsKeyStore` (ESP32 flash, `SessionlessNvs.h`), `FileKeyStore` (macOS/Linux, `SessionlessFileStore.h`, mode 0600), `MemoryKeyStore`, or subclass `KeyStore` for anything else.

## Behavior that matters

- **Randomness.** `defaultRandom()` is `esp_fill_random()` on ESP32 and `getentropy()` on macOS/Linux. There is deliberately no `rand()`/`time()` fallback: on an unknown platform you get `RngError` unless you pass your own `RandomFn`. This replaces the old `generateKeys`, which seeded `rand()` with `time()`. On ESP32 `esp_fill_random` is only a true random source while the radio is on (or `bootloader_random` is enabled), so create keys after `WiFi.mode(...)`, as the wand and target already do.
- **Stack.** libsecp256k1 needs far more than the Arduino loop task's 8 KB. Too little doesn't fail cleanly: it corrupts memory and aborts with a bogus "all-zero pubkey" error. Use `SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES)` (32 KB, what the wand uses), or give your own task that much.
- **Self-test on adopt.** Before a key is used or saved, the library derives the public key, checks it isn't degenerate, and signs and verifies a probe. A broken secp256k1 build (see below) fails with `CryptoError` instead of producing bad identities.
- **Corruption is loud.** A stored key that is the wrong length, out of range, or disagrees with its stored public key is `CorruptKeys`. It is never overwritten.
- **Persist or fail.** If saving a new key fails, `ready()` stays false. You never get an identity that disappears on reboot.
- **No copies to leak.** The private key is wiped on destruction and from temporary buffers.

## Verified

- Host tests (`cd test/host && make test`): 172 checks, 0 failed. They load `../../test-vectors.json` (made by the JS `sessionless-kit`, which is cross-verified against `sessionless-node`). All 20 vectors verify, and signing with each vector's private key reproduces the signature byte-for-byte. Also covered: tampering, malformed hex and lengths, zero and off-curve keys, RNG failure and a stuck RNG, wrong key, no-overwrite, a store that fails to save, a corrupt store, a mismatched public key, and file store mode 0600.
- `pio run` builds `examples/keys-demo` for the ESP32-S3 (um_tinys3): 793 KB flash, 43 KB RAM.

## Not verified

- **Not run on hardware.** I did not flash a board. Flash `examples/keys-demo`, then reset and confirm that the public key survives the reset and that "verifies: yes / tampered: rejected" prints.
- Signing and key-generation time and stack headroom on the chip are unmeasured. The 32 KB figure is inherited from the wand's measured fix, not tuned.
- The demos have not been migrated to this library yet.

## Broken vendored copies in the repo

`lora-sessionless/lib/secp256k1-embedded` and `lora-gateway-umpros3/lib/secp256k1-embedded` contain hand-inserted `return 1;` lines in `secp256k1_ec_pubkey_create` (debugging leftovers from the stack-overflow hunt). Firmware built from them gets an all-zero public key. The copies in `espnow-wand`, `espnow-target` and `ble-sessionless` are clean, and `libs/secp256k1-embedded` here is a copy of the clean one. This library's self-test would catch the broken build. Migrating those two projects here also fixes them.

---

# MagicCore

What a wand and a gateway both need that has nothing to do with a radio or WiFi: the signed message strings, the spell JSON, the wire frames, a clock, and a replay-proof counter. No Arduino, no network, no heap in the hot paths, so all of it is tested on a laptop. Include `<MagicCore.h>`.

## The payload limits (enforced, not advisory)

| Limit | Value | Where it bites |
|---|---|---|
| A gateway's `extra` | **128 bytes** | `checkExtra`, `writeSpellJson` -> `ExtraTooLarge` |
| Whole spell body | **2048 bytes** | `writeSpellJson` -> `BodyTooLarge`, even if your buffer is bigger |

`extra` must also be a valid JSON object (strict RFC 8259 grammar, ASCII only, nested at most 4 deep; use `\uXXXX` for non-ASCII). Anything else is `BadExtra`, so a gateway can't emit malformed JSON that breaks the resolver, and the nesting bound keeps stack use on an MCU predictable.

Why those numbers (measured): the caster's spell is ~309 bytes and each gateway's signature entry ~256 bytes before any extra. Four gateways with maximum extras come to 1835 bytes. 256-byte extras would not fit in 2048. 128 also fits in the 136 bytes left in a 250-byte ESP-NOW frame after a cast. Every receiver must be able to buffer `MAGIC_MAX_SPELL_BODY_BYTES`.

These limits are **not tracking protection**. A 16-byte stable ID fits easily in 128 bytes. The real levers are declaring each gateway's allowed `extra` keys and types in the spellbook (resolver-side, in fount, not this repo) and deciding what to do about `casterUUID` and `ordinal`, which every gateway already sees.

## API

```cpp
// signed messages: what fount's resolve() reconstructs
magic::signCaster(id, timestamp, "wandCast", casterUUID, 50, true, ordinal, sigHex);
magic::signGateway(id, timestamp, gatewayUUID, 0, ordinal, sigHex);
magic::signRegistration(id, timestamp, sigHex);

// the body a gateway POSTs, with limits enforced
magic::writeSpellJson(buf, sizeof(buf), &len, spellFields, gatewayFields, count);

// wire frames (explicit little-endian, byte-identical to the demos' packed structs)
magic::encode(CastSpellFrame{...}, out, cap);    magic::decode(in, len, castSpellFrame);

magic::EpochClock clock;  clock.sync(epochMsFromGateway, monotonicMs);  clock.now(monotonicMs, &epoch);
magic::OrdinalCounter counter(store);  counter.next(&ordinal);   // saved BEFORE it's returned
```

- **Strict inputs.** Spell names and UUIDs must be `[A-Za-z0-9._:-]`, so an id arriving in an untrusted radio frame can never inject JSON (tested). Frame decoding checks exact length, type byte, 0/1 flags, and UUID termination.
- **No guessing.** An unsynced clock returns `NotSynced`. A failed ordinal save returns `StoreError` and no ordinal. Everything returns a `Status`.
- **Clock input is 64-bit.** Pass `esp_timer_get_time()/1000`, not Arduino `millis()` (wraps at ~49.7 days).
- `MagicNvs.h` stores the ordinal under the same `"ordinal"` key the wand demo uses.

## Verified

- `cd test/host && make test`: SessionlessKit 172 checks + MagicCore 214 checks, 0 failed. MagicCore is checked against `../magic-vectors.json`, generated by the JS `sessionless-kit`: message strings and deterministic signatures (byte-for-byte), exact spell JSON bodies, and exact frame bytes (decode, and re-encode to identical bytes). Plus limits at the exact boundary (128 ok, 129 refused), malformed-JSON and depth cases, JSON-injection attempts, undersized buffers with a canary proving nothing is written past `cap`, truncated/oversized/wrong-type frames, clock and ordinal edge cases, uint64 max.
- Clean under UBSan (214 checks, no undefined behavior).
- `pio run` builds both examples for the ESP32-S3 (`examples/magic-demo`: 798 KB flash, 46 KB RAM).

## Not verified

- **AddressSanitizer did not run**: even an empty ASan program hangs at startup on this machine, so buffer safety rests on the canary tests, not a sanitizer. Run `make` with `-fsanitize=address` on a machine where it works.
- **Not run on hardware** (nothing flashed). `examples/magic-demo` prints the body it builds; check it against `magic-vectors.json` shapes by eye or by pasting it into a JSON parser.
- **Frames vs the live demo.** Layouts match the packed structs in `espnow-wand/src/espnow_protocol.h` by construction and by the golden vectors, but I haven't exchanged a frame between this library and the demo firmware.
- The resolver (fount) does not yet enforce the 128/2048 limits. That's a change outside this repo.

---

# Wand and gateway

```
 wand ──Transport──▶ gateway ──Resolver──▶ fount
 (MagicWand)         (MagicGateway)        (or anything with the same contract)
```

A **wand** runs `Wand::poll()` and calls `wand.cast(spell)`. A **gateway** runs `Gateway::poll()` and supplies a `GatewayHandler` for what happens when a spell arrives. Neither knows what radio or network is underneath: that is the `Transport` (frames between wand and gateway) and the `Resolver` (registers keys, resolves spells). Both are real interfaces in `MagicCore.h`, with implementations here for ESP-NOW, HTTPS (ESP32), libcurl (host) and an in-memory `LoopbackLink`.

A complete wand is `examples/magic-wand/main.cpp` (about 60 lines of application code) and a complete gateway is `examples/magic-gateway/main.cpp`. Both build for the ESP32-S3 with `pio run -e magic-wand` / `-e magic-gateway`.

```cpp
// gateway: what happens when a spell lands is yours to define
struct Reactions : magic::GatewayHandler {
  void onCast(const magic::CastInfo &c) override { /* instant reaction, before the network round trip */ }
  size_t extra(const magic::CastInfo &, char *out, size_t cap) override { /* up to 128 bytes of JSON, or 0 */ }
  void onResolved(const magic::CastInfo &, bool ok, magic::Status s) override { /* only celebrate if ok */ }
};
```

## Writing your own Transport or Resolver

- **Transport** (`MagicCore.h`): `maxFrameSize()`, `send(peer|null=broadcast, frame, len)`, and a non-blocking `receive(buf, cap, &len, &from)`. `Ok` from `send` means "handed to the link", not "delivered". Frames larger than `cap` are discarded, never truncated. BLE, LoRa and serial are each one small class away.
- **Resolver**: `registerUser(...)` and `resolve(request, &result)`. A resolver saying "no" is `Ok` with `success=false`; only failing to get an answer is an error. Networked resolvers must verify TLS by default. The shared contract code (`buildRegisterBody`, `parseRegisterResponse`, `parseResolveResponse`, `checkResolverUrl`) is in MagicCore and tested on a laptop, so a new HTTP resolver is mostly glue.

## Security defaults

- **TLS is verified by default** in both resolvers, and the opt-out is `TlsVerification::InsecureForDevelopment`. `FountHttpResolver` needs the root CA PEM (an ESP32 has no trust store), and without one it fails with `BadArgument` instead of quietly skipping verification. The original demos call `client.setInsecure()`; these packages don't.
- **URL policy** (`checkResolverUrl`): http(s) only, no `user@host` tricks, and plain `http://` only to loopback unless you set `allowPlainHttp`. Redirects are never followed (a redirect to `http://` would downgrade TLS).
- **Ids from the radio can't inject JSON** (spell names and UUIDs are `[A-Za-z0-9._:-]`, enforced on decode and when writing JSON).
- **An unregistered wand can't be handed an identity:** a `RegisterResponse` is only accepted if the wand asked for one, and a UUID that can't be saved to flash is treated as a failure.
- **The gateway checks what it can before spending a network round trip:** wand registration signatures and timestamp freshness. It can't check a caster's spell signature (it doesn't know the caster's public key), so that is the resolver's job. See "Known issue in fount".

## Known limits (by design, or not solved yet)

- **One spell per wand/gateway pairing.** The v1 `CastSpell` frame (byte-identical to the demos') does not carry the spell, so the wand and gateway are configured with the same `SpellDef`. A gateway serving several spells needs a new frame type naming the spell. That is a protocol decision, not done here.
- **Link frames are unauthenticated.** A `TimeSync` could be spoofed to skew a wand's clock, and a `CastResult` could be forged (results are advisory). `Wand::pinGateway(peer)` ignores every other peer. Authenticating `TimeSync` would need a gateway signature in the frame.
- **No rate limiting or replay memory at the gateway.** It checks freshness (5 minutes by default) but not repeats; the resolver is expected to reject replayed ordinals (see below).
- **`poll()` blocks during the resolver round trip**, as the original target did.
- **The gateway's own ordinal is persisted per cast**, so it writes flash on every cast. Fine for demos; use a `MemoryOrdinalStore` or batch writes for a busy gateway.
- **Multiple wands on ESP-NOW broadcast can see each other's results.** `EspNowTransport(unicast = true)` fixes it, but that mode has never run on hardware.

## fount signature bug: found and fixed

`fount/src/server/node/src/routes/magic.js` `resolve()` set `resolved = false` when a caster or gateway signature failed to verify, then overwrote it a few lines later (`resolved = await user.spendMP(...)` / `user.spendMoney(...)`). A spell signed with the wrong key resolved, and so did one whose `totalCost` was altered after signing.

**Fix (that one file, 8 lines):** the spend step now only runs `if(resolved)`, so a failed signature falls through to the existing HTTP 900 `{"success":false}` path instead of being overwritten.

| Request | Before | After |
|---|---|---|
| valid cast | 200 success | 200 success |
| caster signature made by a different key | 200 success | **900 refused** |
| gateway signature made by a different key | 200 success | **900 refused** |
| caster signature `deadbeef` | 900 | 900 |

`DIRECT=1 test/e2e/run.sh` runs the C++ wand and gateway against fount with no proxy: it passes on the fixed code (including refusing a tampered cast) and fails on the old behavior, so it is a real regression test for this bug.

**Still open in fount (not changed, separate decisions):**
- **Replay:** the same ordinal resolves repeatedly. Within the 5-minute timestamp window, a recorded cast frame can be replayed to spend the caster's MP again. Fix would be requiring `ordinal` to increase per caster.
- **Short gateway signatures are skipped** (`if(gateway.signature.length < 5) continue;`). That gateway is then neither verified nor rewarded, so it is a bypass of gateway verification, though not of caster verification.
- fount's own mocha suite (`test/mocha/magic-spells.js`) cannot run here: the installed `fount-js` has no `castSpell` (unrelated to this fix), so it did not validate it.

The verifying proxy (`test/e2e/strict-proxy.mjs`) is still the default for the e2e because it also enforces replay protection and the 128/2048 limits, which fount does not yet.

## Running the tests

```
cd test/host && make test            # SessionlessKit, MagicCore, wand+gateway (no network, no hardware)
test/e2e/run.sh                      # real fount + verifying proxy + C++ wand/gateway over libcurl
DIRECT=1 test/e2e/run.sh             # same, straight at fount (no proxy): fails if fount stops verifying signatures
test/e2e/tls-test.sh                 # proves certificate verification is on by default
pio run                              # builds every example for the ESP32-S3
```

`run.sh` starts fount from a throwaway copy (your `fount/.../data` is untouched) and needs `FOUNT_DIR` if fount isn't at `../../../../../fount/src/server/node` relative to this folder. Ports 3006 and 3099 must be free.

## Verified (this machine)

- Host suites: SessionlessKit 172 checks, MagicCore 343, wand+gateway 158, all passing. Wand+gateway run over a real `LoopbackLink` against a strict in-process resolver that verifies every signature, rejects replays and enforces the limits. It covers the full lifecycle plus: unsolicited and spoofed frames, stale and tampered casts, spell mismatch, oversized and malformed `extra`, resolver down and recovery, registration abuse, reboot-resume from storage, gateway pinning, TimeSync cadence, stale-sync detection, and failing stores. Negative tests prove the resolver's verification isn't vacuous.
- **End to end against the real fount:** C++ wand and gateway through libcurl and the verifying proxy: registration, 3 casts resolved (with a gateway `extra`), a frame tampered in transit refused, and recovery on the next cast.
- **TLS:** a default-config resolver refuses a self-signed server and a host-name mismatch, trusts a pinned certificate, and only the explicit opt-out skips verification (the server saw exactly the 2 requests that should have connected).
- All four environments build for the ESP32-S3: `keys-demo`, `magic-demo`, `magic-wand` (807 KB flash), `magic-gateway` (1002 KB flash, `um_pros3`).

## Not verified

- **Nothing here has run on hardware.** The ESP-NOW transport, the HTTPS resolver on-device, the NVS stores, the example sketches, and the channel-scan logic compile but have not been flashed. The channel handling and broadcast behavior are taken from what the original demos proved, not re-proven.
- **`FountHttpResolver` on a device has not made a TLS connection.** The shared parsing and URL policy are tested on the host; the ESP32 HTTP/TLS plumbing is not.
- **Stack use on the chip** (secp256k1 plus TLS in the loop task) is assumed from the demo's 32 KB, not measured.
- **Frames between this code and the original demo firmware** have not been exchanged (layouts match by construction and golden vectors).
- AddressSanitizer cannot run on this machine; the earlier UBSan pass was on MagicCore only, not re-run on wand/gateway.
