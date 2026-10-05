use sessionless_embedded::*;
use rand_core::{CryptoRng, Error, OsRng, RngCore};
use serde_json::Value;

fn vectors() -> Vec<Value> {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../test-vectors.json");
    let v: Value = serde_json::from_str(&std::fs::read_to_string(path).expect("test-vectors.json")).unwrap();
    v["vectors"].as_array().unwrap().clone()
}

fn hex<const N: usize>(s: &str) -> [u8; N] {
    let mut out = [0u8; N];
    from_hex(s, &mut out).expect("valid hex");
    out
}

#[test]
fn all_vectors_verify() {
    let vs = vectors();
    assert_eq!(vs.len(), 20);
    for v in &vs {
        let msg = v["message"].as_str().unwrap();
        assert!(verify_hex(v["signature"].as_str().unwrap(), msg.as_bytes(), v["pubKey"].as_str().unwrap()), "{msg:?}");
    }
}

#[test]
fn signing_reproduces_vectors_byte_for_byte() {
    for v in &vectors() {
        let id = Identity::from_private_key(&hex::<32>(v["privateKey"].as_str().unwrap())).unwrap();
        assert_eq!(&id.public_key_hex()[..], v["pubKey"].as_str().unwrap().as_bytes());
        let msg = v["message"].as_str().unwrap().as_bytes();
        assert_eq!(&id.sign_hex(msg)[..], v["signature"].as_str().unwrap().as_bytes());
    }
}

#[test]
fn tamper_and_malformed_fail() {
    let v = &vectors()[1];
    let sig = v["signature"].as_str().unwrap();
    let pk = v["pubKey"].as_str().unwrap();
    assert!(verify_hex(sig, b"hello", pk));
    assert!(!verify_hex(sig, b"hellO", pk));
    let mut bad = sig.to_string();
    bad.replace_range(0..1, if &sig[0..1] == "0" { "1" } else { "0" });
    assert!(!verify_hex(&bad, b"hello", pk));
    assert!(!verify_hex("zz", b"hello", pk));
    assert!(!verify_hex(&sig[..126], b"hello", pk));
    assert!(!verify_hex(sig, b"hello", &"00".repeat(33)));
    assert!(!verify(&[0u8; 64], b"hello", &[0u8; 33]));
    assert!(!verify(&[], &[], &[]));
    // a different key must not verify
    let other = Identity::generate(&mut OsRng).unwrap();
    assert!(!verify(&hex::<64>(sig), b"hello", other.public_key()));
}

#[test]
fn high_s_signature_rejected() {
    // n - s of a valid signature is the malleable twin; sessionless-node rejects it too.
    let v = &vectors()[1];
    let sig: [u8; 64] = hex(v["signature"].as_str().unwrap());
    let n = hex::<32>("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
    let mut s = [0u8; 32];
    let mut borrow = 0i16;
    for i in (0..32).rev() {
        let d = n[i] as i16 - sig[32 + i] as i16 - borrow;
        borrow = (d < 0) as i16;
        s[i] = (d + 256) as u8;
    }
    let mut twin = sig;
    twin[32..].copy_from_slice(&s);
    assert!(!verify(&twin, b"hello", &hex::<33>(v["pubKey"].as_str().unwrap())));
}

#[test]
fn zero_and_out_of_range_keys_rejected() {
    assert!(Identity::from_private_key(&[0u8; 32]).is_none());
    assert!(Identity::from_private_key(&[0xffu8; 32]).is_none());
    assert!(Identity::from_private_key(&[1u8; 31]).is_none());
    let mut store = MemoryStore::with_key([0u8; 32]);
    assert!(matches!(Identity::load_or_create(&mut store, &mut OsRng), Err(sessionless_embedded::Error::CorruptStoredKey)));
}

#[test]
fn create_then_reload_same_identity_and_never_overwrite() {
    let mut store = MemoryStore::new();
    let a = Identity::load_or_create(&mut store, &mut OsRng).unwrap();

    struct NoRng;
    impl RngCore for NoRng {
        fn next_u32(&mut self) -> u32 { panic!("rng must not be used when a key exists") }
        fn next_u64(&mut self) -> u64 { panic!("rng must not be used when a key exists") }
        fn fill_bytes(&mut self, _: &mut [u8]) { panic!("rng must not be used when a key exists") }
        fn try_fill_bytes(&mut self, _: &mut [u8]) -> Result<(), Error> { panic!("rng must not be used when a key exists") }
    }
    impl CryptoRng for NoRng {}

    let b = Identity::load_or_create(&mut store, &mut NoRng).unwrap();
    assert_eq!(a.public_key(), b.public_key());
    assert_eq!(&a.private_key_bytes()[..], &b.private_key_bytes()[..]);
}

#[test]
fn rng_that_returns_zeros_is_an_error_not_a_weak_key() {
    struct Zeros;
    impl RngCore for Zeros {
        fn next_u32(&mut self) -> u32 { 0 }
        fn next_u64(&mut self) -> u64 { 0 }
        fn fill_bytes(&mut self, d: &mut [u8]) { d.fill(0) }
        fn try_fill_bytes(&mut self, d: &mut [u8]) -> Result<(), Error> { d.fill(0); Ok(()) }
    }
    impl CryptoRng for Zeros {}
    let mut store = MemoryStore::new();
    assert!(matches!(Identity::load_or_create(&mut store, &mut Zeros), Err(sessionless_embedded::Error::Rng)));
    assert_eq!(store.load(&mut [0u8; 32]).unwrap(), None, "nothing saved");
}

#[test]
fn store_that_loses_writes_is_detected() {
    struct Amnesiac;
    impl KeyStore for Amnesiac {
        type Error = ();
        fn load(&mut self, _: &mut [u8; 32]) -> Result<Option<usize>, ()> { Ok(None) }
        fn save(&mut self, _: &[u8; 32]) -> Result<(), ()> { Ok(()) }
    }
    assert!(matches!(Identity::load_or_create(&mut Amnesiac, &mut OsRng), Err(sessionless_embedded::Error::SaveNotPersisted)));
}

#[test]
fn wrong_length_stored_key_is_corrupt() {
    struct Short;
    impl KeyStore for Short {
        type Error = ();
        fn load(&mut self, _: &mut [u8; 32]) -> Result<Option<usize>, ()> { Ok(Some(16)) }
        fn save(&mut self, _: &[u8; 32]) -> Result<(), ()> { panic!("must not overwrite") }
    }
    assert!(matches!(Identity::load_or_create(&mut Short, &mut OsRng), Err(sessionless_embedded::Error::CorruptStoredKey)));
}

#[test]
fn file_store_roundtrip_0600_and_no_overwrite() {
    use std::os::unix::fs::PermissionsExt;
    let dir = std::env::temp_dir().join(format!("sl-emb-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("key.hex");
    let a = Identity::load_or_create(&mut FileStore::new(&path), &mut OsRng).unwrap();
    assert_eq!(std::fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
    let b = Identity::load_or_create(&mut FileStore::new(&path), &mut OsRng).unwrap();
    assert_eq!(a.public_key(), b.public_key());

    // corrupt file: loud error, file untouched
    std::fs::write(&path, "not hex").unwrap();
    assert!(matches!(Identity::load_or_create(&mut FileStore::new(&path), &mut OsRng), Err(sessionless_embedded::Error::CorruptStoredKey)));
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "not hex");
    std::fs::remove_dir_all(&dir).unwrap();
}

#[test]
fn hex_roundtrip() {
    let bytes = [0x00u8, 0x0f, 0xa5, 0xff];
    let h: [u8; 8] = to_hex(&bytes);
    assert_eq!(&h, b"000fa5ff");
    let mut out = [0u8; 4];
    assert!(from_hex("000FA5ff", &mut out).is_some());
    assert_eq!(out, bytes);
    assert!(from_hex("000fa5f", &mut out).is_none());
    assert!(from_hex("000fa5fg", &mut out).is_none());
}
