# What Rust will need

A running list, added to as the C++ packages uncover things. Rust is parked for now, and the target is **stock Rust** (no forks, no `espup` forks of the compiler). Each item says what C++ taught us and where the Rust crate (`rust/sessionless-embedded`) stands.

Target hardware: ESP32-C3/C6 are RISC-V and build with stock Rust. The ESP32-S3 used in the demos is Xtensa and does not, so Rust on those boards would mean moving to a RISC-V board. Nothing here has run on any chip.

## Keys (learned building `sessionless-kit-cpp`)

| # | C++ lesson | Rust status |
|---|---|---|
| 1 | **Stack.** C libsecp256k1 silently corrupts memory under the Arduino 8 KB loop stack and needs 32 KB. | k256 stack use is **unmeasured**. Needs a high-water-mark measurement on a C3/C6 before any claim. |
| 2 | **RNG must be a true hardware source.** On ESP32 `esp_fill_random` is only true-random while the radio is on. Old code used `rand()`+`time()`. | Crate takes `CryptoRngCore`, so no weak default. Needs a verified esp-hal TRNG binding and a documented "radio on before keygen" rule. |
| 3 | **Store the public key next to the private key**, and treat a mismatch as corruption. Needed so existing NVS layouts (`privKey`/`pubKey`) stay readable. | **Gap.** The crate's `KeyStore` stores only the 32-byte private key. Add an optional public key + mismatch check to match C++ and JS. |
| 4 | **Same NVS layout as the demos**, so devices keep their identity and fount registration. | **Gap.** Needs an NVS-compatible store (or a one-time migration) on the Rust side. |
| 5 | **Self-test on adopt:** derive pubkey, reject degenerate, sign and verify a probe. Caught a class of broken vendored builds (two repo copies return an all-zero pubkey). | Not ported. Cheap to add. |
| 6 | **Never overwrite corrupt keys; never keep an identity that failed to persist.** | Done in the crate (`CorruptStoredKey`, `SaveNotPersisted`). |
| 7 | **Shared vectors** (`test-vectors.json`) with byte-for-byte signing. | Done. Extend when MAGIC message vectors exist (see below). |
| 8 | **High-S rejected on verify**, matching sessionless-node and libsecp256k1. | Done. |

## MAGIC messages and wire (learned reading the wand/target demos; not yet built in C++)

- **Message strings** are plain string concatenation, signed with the identity:
  - caster: `timestamp + spell + casterUUID + totalCost + mp + ordinal`, with `mp` as `true`/`false`
  - gateway: `timestamp + uuid + minimumCost + ordinal`
  - registration: `timestamp + pubKeyHex`

  Rust needs allocation-free decimal formatting for u64/u32 into fixed buffers. These strings should get golden test vectors shared by C++, Rust and JS.
- **Wire frames** (ESP-NOW, 250-byte cap) are packed little-endian structs, today copy-pasted between the wand and target with a comment that they "must stay byte-identical". One spec plus golden byte vectors replaces that. In Rust use explicit byte encode/decode, not `repr(packed)` casts.
- **Clock.** The wand has no NTP and calibrates from the gateway's `TimeSync` broadcast (`epoch_ms + (monotonic_now - monotonic_at_sync)`). Rust needs a monotonic ms source and the same offset logic. fount rejects timestamps more than 5 minutes off.
- **Ordinal** is a persisted counter (NVS), incremented before send.

## MagicCore specifics (built; C++ only)

`magic-vectors.json` holds the golden vectors Rust must reproduce exactly: message strings plus deterministic signatures, spell JSON bodies, and frame bytes. Rust-specific work:

- **Limits are part of the contract:** extra <= 128 bytes, body <= 2048 bytes, same `Status` outcomes (`ExtraTooLarge`, `BadExtra`, `BodyTooLarge`).
- **A strict JSON validator for `extra`** (RFC 8259 grammar, ASCII only, depth <= 4, bounded stack). Do not pull in a general JSON crate for this; a ~100-line recursive-descent checker with an explicit depth cap is what C++ does, and the test cases in `test/host/test_magic.cpp` (`testExtraLimit`) are the spec.
- **Allocation-free spell JSON writer** with fixed key order; it counts the full length even when the buffer is too small, then reports `BodyTooLarge` before `BufferTooSmall`.
- **Safe-id rule** `[A-Za-z0-9._:-]` on spell names and UUIDs, enforced on encode and decode (it is what stops JSON injection from radio frames).
- **Clock takes a 64-bit monotonic.** Rust should expose the same (`u64` ms), never a wrapping 32-bit counter.
- **Ordinal counter saves before returning.** Rust needs the same store trait with `NotFound` vs `StoreError`, and the same `Overflow` at `u32::MAX`.
- **Decode ignores bytes after a UUID's NUL** (demo firmware may send garbage there). Keep it, with a test.
- **Verification gap:** AddressSanitizer could not run in the C++ work, so run Miri (or `cargo test` under sanitizers) on any `unsafe` if Rust adds some. Prefer none.

## Wand, gateway, Transport, Resolver (built in C++; the shape Rust should copy)

- **Two traits, same semantics.** `Transport`: `max_frame_size`, `send(Option<&PeerId>, &[u8])` (`None` = broadcast; `Ok` = handed to the link, not delivered), non-blocking `receive(&mut [u8]) -> Option<(len, PeerId)>` where an oversized frame is discarded, never truncated. `Resolver`: `register_user` and `resolve`, where a resolver saying "no" is `Ok(success=false)` and only failing to get an answer is an error. Rust wants these synchronous and poll-driven (like the C++), so they work without an async runtime; an async adapter can sit on top.
- **`PeerId`** is up to 8 opaque bytes (an ESP-NOW MAC, a BLE address). No heap.
- **The fount contract lives in one tested place.** `parse_register_response` / `parse_resolve_response` / `build_register_body` / `check_resolver_url` must match the C++ cases in `test_magic.cpp` (`testResolverContract`, `testResolverUrlPolicy`) exactly, including: fount rejects with HTTP **900** + `{"success":false}`; "stale time" is HTTP 200 + `{"error":...}` with no `success` key (answered: no); success means HTTP 200 **and** `success` is the JSON boolean `true` (not `"true"`, not `1`); any 5xx is "unreachable", never trusted as an answer; a garbled body is `BadResponse`, never "key absent".
- **`jsonGetTopLevel` needs a Rust twin:** strict RFC 8259 walk, depth <= 8, UTF-8 pass-through, last duplicate key wins, only top-level members. Don't use a general JSON crate on an MCU for this.
- **TLS on the MCU is the biggest open question for a Rust gateway.** C++ gets `WiFiClientSecure` for free; stock Rust needs an embedded TLS stack (e.g. `embedded-tls`/`rustls` variants) that supports certificate verification against a supplied root. Verification must stay ON by default, with the same long-named opt-out. Unverified: whether such a stack handles Let's Encrypt/Amazon chains within MCU memory.
- **ESP-NOW lessons that apply to any language** (learned in the C++ demos): `set_channel` on an unassociated radio only works with promiscuous mode toggled around it; a wand must scan channels 1..13 until it hears a gateway, and resume scanning if sync goes stale; a WiFi-joined gateway stays on the router's channel; the broadcast peer is added with channel 0; the demo's proven path is broadcast (unicast is untested).
- **The receive callback runs on the WiFi task**, so the queue between it and `poll()` needs a real critical section (C++ uses a spinlock mux). In Rust that's a `critical_section::Mutex` or lock-free ring, not a `RefCell`.
- **State that must survive reboot** (all behind store traits): keys (+ pubkey consistency), ordinal, UUID per peer/scope. Order matters: ordinal is saved **before** it is used; a UUID that can't be saved is not "registered".
- **Never accept unsolicited identity or results:** `RegisterResponse` only while a registration is outstanding, `CastResult` only while a cast is pending, both only from the current (or pinned) gateway.
- **Known protocol gaps to decide once, for every language:** the v1 `CastSpell` frame doesn't name the spell (one spell per pairing); `TimeSync` is unauthenticated.

## Gateway-only (hard parts for Rust, unverified)

- WiFi + NTP + HTTPS/TLS to the resolver, plus JSON. In C++ this is Arduino `WiFiClientSecure`, `HTTPClient` and ArduinoJson. In stock Rust it means an embedded TLS and HTTP stack, which is the biggest unknown. Today's demos also call `client.setInsecure()`, so the resolver's certificate is not verified. A packaged gateway should not ship that default.
- ESP-NOW in Rust on a RISC-V board: availability and API are unchecked.

## Things C++ would hide that Rust should keep explicit

- Every fallible call returns a typed error, with no silent fallbacks (matches repo rules).
- No global crypto context. The C++ lib keeps one lazy static; in Rust, pass or own it.
