# sessionless-embedded

`no_std` Sessionless keys for microcontrollers in Rust: create, save, sign, verify. An exploratory counterpart to the C++ library and to `sessionless-kit` (JS).

Wire format is identical to `sessionless-node`: ECDSA secp256k1 over `keccak256(message)` (Keccak-256, not SHA3-256), RFC6979 deterministic nonce, low-S, 64-byte compact `r||s`, 33-byte compressed public keys. Pure Rust (`k256`, `sha3`), no C, no allocator.

```rust
let mut store = MyNvsStore::new();           // impl KeyStore (32 bytes in NVS/flash)
let mut rng = /* a TRUE hardware RNG */;
let id = Identity::load_or_create(&mut store, &mut rng)?;   // never overwrites an existing key

let sig = id.sign_hex(message);              // [u8; 128], lowercase hex
let pk  = id.public_key_hex();               // [u8; 66]
assert!(verify_hex(core::str::from_utf8(&sig)?, message, core::str::from_utf8(&pk)?));
```

## Behaviour worth knowing

- `load_or_create` only generates when the store reports *nothing stored*. A stored key that is the wrong length, zero, or out of range is `Error::CorruptStoredKey`; it is never replaced.
- After saving a new key it reads it back; if the store lost it you get `Error::SaveNotPersisted` instead of an identity that vanishes on reboot.
- An RNG that yields only invalid keys (e.g. all zeros) is `Error::Rng`; nothing is saved.
- `verify` returns `false` for malformed signatures or keys and for high-S signatures (as `sessionless-node` does).
- The private key is zeroized on drop; `private_key_bytes()` returns a `Zeroizing` copy.
- `FileStore` (feature `std`, Unix) writes a 0600 file with `create_new`, so it can't clobber a file.

## Entropy is your job

The crate cannot tell whether your RNG is good. On ESP32, `esp_random()` is only a true random source while WiFi/BT is on or `bootloader_random` is enabled; otherwise it is pseudo-random and keys generated from it are weak. Pass a hardware TRNG, and generate keys when that is true.

## Verified vs not

Verified (this machine, rustc 1.92):
- `cargo test --features std`: 11 tests pass, 0 fail. They load `../../test-vectors.json` (produced by `sessionless-kit`, which is cross-verified against `sessionless-node`): all 20 vectors verify, and signing with each vector's private key **reproduces the signature byte-for-byte** and the public key. Also covered: tamper, malformed hex/lengths, wrong key, high-S twin rejected, zero/out-of-range/short keys, corrupt store left untouched, no overwrite (RNG not touched when a key exists), zero-RNG, lost-write store, file store 0600.
- The test suite found one real bug (k256's `from_slice` left-pads short keys, so a 31-byte key was accepted); `from_private_key` now requires exactly 32 bytes.
- `cargo build --release --no-default-features` succeeds for `thumbv7em-none-eabihf` and `riscv32imc-unknown-none-elf`.
- Code size, `size-probe/` (load-or-create + sign + verify, `opt-level="s"`, LTO, flat binary via `rust-objcopy`): **~38 KB (Cortex-M4F), ~51 KB (RV32IMC)**. Includes a stub volatile-read RNG, so slightly under what a real HAL adds. Reproduce: `cd size-probe && RUSTFLAGS="-C link-arg=-Tlink.ld" cargo build --release --target thumbv7em-none-eabihf`.

NOT verified:
- Running on any real chip. The ESP32-S3 used in the demos is Xtensa and needs `espup` plus `esp-hal`; I did not install it. ESP32-C3/C6 are RISC-V and work with stock Rust.
- **Stack usage and signing speed on an MCU: not measured.** (The C++ secp256k1 needed a 32 KB loop stack on ESP32. I make no claim here about k256; measure on hardware.)
- The esp-hal sketch below.

## esp-hal sketch (UNTESTED, API names will drift)

```rust
// esp-hal 0.2x, ESP32-C3 or S3 with esp toolchain
let mut rng = esp_hal::rng::Trng::new(peripherals.RNG, &mut peripherals.ADC1); // true RNG; check your esp-hal version
struct NvsStore(/* esp-storage / esp-nvs handle */);
impl KeyStore for NvsStore { /* load: Some(32) or None; save: write 32 bytes */ }
let id = Identity::load_or_create(&mut NvsStore(..), &mut rng).unwrap();
```
`Trng` implements `rand_core::RngCore + CryptoRng` in recent esp-hal versions; confirm for the version you pin.

## Rust vs the C++ route (honest)

- Rust: one pure-Rust dependency tree instead of three divergent hand-patched vendored C copies; memory-safe wrapper; strict APIs (no `rand()` footgun: you must supply an RNG); the same crate runs on host for tests and tools. Cost: the Xtensa toolchain (S3) is a separate install and a less mature path than Arduino/PlatformIO, so it means rewriting the firmware (WiFi, ESP-NOW, BLE, NVS) in esp-hal/esp-idf-hal, not just swapping the crypto. Size here is ~40-50 KB, unmeasured against the C++ build.
- C++: works today in every demo project, with Arduino/PlatformIO and `Preferences` already there. Cost: the stack-size workaround, weak RNG in current `generateKeys`, duplicated vendored code.
