//! Sessionless keys for microcontrollers: create, save, sign, verify.
//!
//! Wire format matches `sessionless-node` / `sessionless-kit`:
//! ECDSA secp256k1 over `keccak256(message)`, RFC6979 deterministic nonce,
//! low-S, 64-byte compact `r || s`; public keys are 33-byte compressed.
//!
//! # Entropy
//! Key quality is only as good as the RNG you pass to [`Identity::load_or_create`].
//! On ESP32 use a true hardware RNG: `esp_random()` is only a TRNG while the
//! RF subsystem (WiFi/BT) is active or `bootloader_random` is enabled. This
//! crate cannot check that for you.
#![cfg_attr(not(feature = "std"), no_std)]

use k256::ecdsa::{signature::hazmat::PrehashVerifier, Signature, SigningKey, VerifyingKey};
use k256::ecdsa::signature::hazmat::PrehashSigner;
use rand_core::CryptoRngCore;
use sha3::{Digest, Keccak256};
use zeroize::Zeroizing;

#[cfg(feature = "std")]
mod file_store;
#[cfg(feature = "std")]
pub use file_store::FileStore;

pub const PRIVATE_KEY_LEN: usize = 32;
pub const PUBLIC_KEY_LEN: usize = 33;
pub const SIGNATURE_LEN: usize = 64;
pub const PUBLIC_KEY_HEX_LEN: usize = PUBLIC_KEY_LEN * 2;
pub const SIGNATURE_HEX_LEN: usize = SIGNATURE_LEN * 2;

#[derive(Debug, PartialEq, Eq)]
pub enum Error<E> {
    /// The store failed to read or write.
    Store(E),
    /// The store holds bytes that are not a usable private key (wrong length,
    /// zero, or >= curve order). Never overwritten: fix or erase it deliberately.
    CorruptStoredKey,
    /// The RNG produced no valid key within the attempt limit.
    Rng,
    /// The store reported success on save but returned different bytes on re-read.
    SaveNotPersisted,
}

/// Persistent storage for the 32-byte private key (NVS, flash, a file...).
pub trait KeyStore {
    type Error;
    /// Return the stored key, or `None` if nothing is stored yet.
    /// Return the raw bytes found, whatever their length (validation is ours).
    fn load(&mut self, out: &mut [u8; PRIVATE_KEY_LEN]) -> Result<Option<usize>, Self::Error>;
    /// Persist the key. Called only for newly created keys.
    fn save(&mut self, key: &[u8; PRIVATE_KEY_LEN]) -> Result<(), Self::Error>;
}

/// RAM-only store, for tests and throwaway identities.
#[derive(Default)]
pub struct MemoryStore {
    key: Option<[u8; PRIVATE_KEY_LEN]>,
}

impl MemoryStore {
    pub const fn new() -> Self {
        Self { key: None }
    }
    pub const fn with_key(key: [u8; PRIVATE_KEY_LEN]) -> Self {
        Self { key: Some(key) }
    }
}

impl KeyStore for MemoryStore {
    type Error = core::convert::Infallible;
    fn load(&mut self, out: &mut [u8; PRIVATE_KEY_LEN]) -> Result<Option<usize>, Self::Error> {
        Ok(self.key.map(|k| {
            *out = k;
            PRIVATE_KEY_LEN
        }))
    }
    fn save(&mut self, key: &[u8; PRIVATE_KEY_LEN]) -> Result<(), Self::Error> {
        self.key = Some(*key);
        Ok(())
    }
}

pub struct Identity {
    key: SigningKey,
    public: [u8; PUBLIC_KEY_LEN],
}

fn keccak(message: &[u8]) -> [u8; 32] {
    let mut h = Keccak256::new();
    h.update(message);
    h.finalize().into()
}

impl Identity {
    /// Build from raw private key bytes. Rejects zero and out-of-range keys.
    pub fn from_private_key(bytes: &[u8]) -> Option<Self> {
        // k256 left-pads short slices, which would silently accept a truncated key.
        if bytes.len() != PRIVATE_KEY_LEN {
            return None;
        }
        let key = SigningKey::from_slice(bytes).ok()?;
        let point = key.verifying_key().to_encoded_point(true);
        let mut public = [0u8; PUBLIC_KEY_LEN];
        public.copy_from_slice(point.as_bytes());
        Some(Self { key, public })
    }

    /// Load the identity from `store`, creating and saving a new one only if
    /// the store is empty. An existing but invalid key is an error, never
    /// replaced.
    pub fn load_or_create<S: KeyStore>(
        store: &mut S,
        rng: &mut impl CryptoRngCore,
    ) -> Result<Self, Error<S::Error>> {
        let mut buf = Zeroizing::new([0u8; PRIVATE_KEY_LEN]);
        match store.load(&mut buf).map_err(Error::Store)? {
            Some(PRIVATE_KEY_LEN) => {
                Self::from_private_key(&buf[..]).ok_or(Error::CorruptStoredKey)
            }
            Some(_) => Err(Error::CorruptStoredKey),
            None => {
                let id = Self::generate(rng).ok_or(Error::Rng)?;
                let raw = id.private_key_bytes();
                store.save(&raw).map_err(Error::Store)?;
                // Confirm it persisted, so we never hand out an identity that vanishes on reboot.
                let mut check = Zeroizing::new([0u8; PRIVATE_KEY_LEN]);
                match store.load(&mut check).map_err(Error::Store)? {
                    Some(PRIVATE_KEY_LEN) if check[..] == raw[..] => Ok(id),
                    _ => Err(Error::SaveNotPersisted),
                }
            }
        }
    }

    /// Like `load_or_create` but fails if nothing is stored.
    pub fn load<S: KeyStore>(store: &mut S) -> Result<Option<Self>, Error<S::Error>> {
        let mut buf = Zeroizing::new([0u8; PRIVATE_KEY_LEN]);
        match store.load(&mut buf).map_err(Error::Store)? {
            Some(PRIVATE_KEY_LEN) => Self::from_private_key(&buf[..])
                .map(Some)
                .ok_or(Error::CorruptStoredKey),
            Some(_) => Err(Error::CorruptStoredKey),
            None => Ok(None),
        }
    }

    /// Generate a key from `rng` without saving it. Retries a few times in the
    /// astronomically unlikely case of an out-of-range draw; returns `None`
    /// if the RNG keeps failing (e.g. returns all zeros).
    pub fn generate(rng: &mut impl CryptoRngCore) -> Option<Self> {
        let mut buf = Zeroizing::new([0u8; PRIVATE_KEY_LEN]);
        for _ in 0..8 {
            rng.try_fill_bytes(&mut buf[..]).ok()?;
            if let Some(id) = Self::from_private_key(&buf[..]) {
                return Some(id);
            }
        }
        None
    }

    pub fn public_key(&self) -> &[u8; PUBLIC_KEY_LEN] {
        &self.public
    }

    pub fn public_key_hex(&self) -> [u8; PUBLIC_KEY_HEX_LEN] {
        to_hex(&self.public)
    }

    /// Copy of the private key for backup. Zeroize it when done.
    pub fn private_key_bytes(&self) -> Zeroizing<[u8; PRIVATE_KEY_LEN]> {
        let mut out = Zeroizing::new([0u8; PRIVATE_KEY_LEN]);
        out.copy_from_slice(&self.key.to_bytes());
        out
    }

    /// Sign `message` (hashed with Keccak-256). Deterministic (RFC6979), low-S.
    pub fn sign(&self, message: &[u8]) -> [u8; SIGNATURE_LEN] {
        let hash = keccak(message);
        // Prehash signing only fails for a hash of the wrong length, which cannot happen here.
        let sig: Signature = self
            .key
            .sign_prehash(&hash)
            .expect("32-byte prehash is always signable");
        let sig = sig.normalize_s().unwrap_or(sig);
        sig.to_bytes().into()
    }

    pub fn sign_hex(&self, message: &[u8]) -> [u8; SIGNATURE_HEX_LEN] {
        to_hex(&self.sign(message))
    }
}

/// Verify a compact signature. Returns false for malformed signatures or keys.
pub fn verify(signature: &[u8], message: &[u8], public_key: &[u8]) -> bool {
    let Ok(sig) = Signature::from_slice(signature) else {
        return false;
    };
    let Ok(vk) = VerifyingKey::from_sec1_bytes(public_key) else {
        return false;
    };
    // Match sessionless-node: reject high-S (malleable) signatures.
    if sig.normalize_s().is_some() {
        return false;
    }
    vk.verify_prehash(&keccak(message), &sig).is_ok()
}

/// Verify with hex-encoded signature and public key (as sent over the wire).
pub fn verify_hex(signature_hex: &str, message: &[u8], public_key_hex: &str) -> bool {
    let mut sig = [0u8; SIGNATURE_LEN];
    let mut pk = [0u8; PUBLIC_KEY_LEN];
    from_hex(signature_hex, &mut sig).is_some()
        && from_hex(public_key_hex, &mut pk).is_some()
        && verify(&sig, message, &pk)
}

/// Lowercase hex into a fixed array, no allocation.
pub fn to_hex<const N: usize, const M: usize>(bytes: &[u8; N]) -> [u8; M] {
    assert!(M == N * 2, "hex buffer must be twice the input length");
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = [0u8; M];
    for (i, b) in bytes.iter().enumerate() {
        out[i * 2] = DIGITS[(b >> 4) as usize];
        out[i * 2 + 1] = DIGITS[(b & 0xf) as usize];
    }
    out
}

/// Decode hex into `out`; `None` unless `hex` is exactly `2 * out.len()` valid hex chars.
pub fn from_hex(hex: &str, out: &mut [u8]) -> Option<()> {
    let h = hex.as_bytes();
    if h.len() != out.len() * 2 {
        return None;
    }
    let nib = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    };
    for (i, o) in out.iter_mut().enumerate() {
        *o = nib(h[i * 2])? << 4 | nib(h[i * 2 + 1])?;
    }
    Some(())
}
