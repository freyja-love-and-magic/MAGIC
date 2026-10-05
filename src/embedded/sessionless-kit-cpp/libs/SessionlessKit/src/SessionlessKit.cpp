#include "SessionlessKit.h"

#include <string.h>

#include "keccak.h"
#include "secp256k1.h"

#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_random.h>
#define SESSIONLESS_HAVE_ESP_RNG 1
#elif defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#include <sys/random.h>
#define SESSIONLESS_HAVE_GETENTROPY 1
#endif

namespace sessionless {

const char *statusName(Status s) {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::NoKeys: return "NoKeys";
    case Status::StoreError: return "StoreError";
    case Status::CorruptKeys: return "CorruptKeys";
    case Status::KeysExist: return "KeysExist";
    case Status::RngError: return "RngError";
    case Status::CryptoError: return "CryptoError";
    case Status::BadArgument: return "BadArgument";
    case Status::NotReady: return "NotReady";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// helpers

// Not optimized away, unlike a plain memset before a destructor.
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
  while (n--) *v++ = 0;
}

static bool allZero(const uint8_t *p, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; i++) acc |= p[i];
  return acc == 0;
}

void toHex(const uint8_t *bytes, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = digits[bytes[i] >> 4];
    out[i * 2 + 1] = digits[bytes[i] & 0xF];
  }
  out[len * 2] = '\0';
}

static int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const char *hex, uint8_t *out, size_t len) {
  if (!hex) return false;
  for (size_t i = 0; i < len * 2; i++) {
    if (nibble(hex[i]) < 0) return false;  // also stops at an early NUL
  }
  if (hex[len * 2] != '\0') return false;
  for (size_t i = 0; i < len; i++) {
    out[i] = (uint8_t)((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
  }
  return true;
}

// ---------------------------------------------------------------------------
// randomness

#if defined(SESSIONLESS_HAVE_ESP_RNG)
static bool platformRandom(uint8_t *out, size_t len) {
  esp_fill_random(out, len);
  return true;
}
RandomFn defaultRandom() { return platformRandom; }
#elif defined(SESSIONLESS_HAVE_GETENTROPY)
static bool platformRandom(uint8_t *out, size_t len) {
  while (len > 0) {
    size_t chunk = len > 256 ? 256 : len;  // getentropy limit
    if (getentropy(out, chunk) != 0) return false;
    out += chunk;
    len -= chunk;
  }
  return true;
}
RandomFn defaultRandom() { return platformRandom; }
#else
RandomFn defaultRandom() { return nullptr; }
#endif

// ---------------------------------------------------------------------------
// secp256k1 context: created once, lazily. Verification needs no randomness;
// signing/keygen blind the context when an RNG is available.

static secp256k1_context *context() {
  static secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
  return ctx;
}

static void blindContext(RandomFn rng) {
  static bool blinded = false;
  if (blinded || !rng) return;
  uint8_t seed[32];
  if (rng(seed, sizeof(seed)) && secp256k1_context_randomize(context(), seed)) blinded = true;
  wipe(seed, sizeof(seed));
}

static void hashMessage(const uint8_t *message, size_t len, uint8_t out[32]) {
  union ethash_hash256 h = ethash_keccak256(message, len);
  memcpy(out, h.bytes, 32);
}

static bool derivePublicKey(const uint8_t priv[32], uint8_t pub[33]) {
  secp256k1_context *ctx = context();
  if (!ctx || !secp256k1_ec_seckey_verify(ctx, priv)) return false;
  secp256k1_pubkey point;
  if (!secp256k1_ec_pubkey_create(ctx, &point, priv)) return false;
  size_t len = SESSIONLESS_PUBLIC_KEY_SIZE;
  if (!secp256k1_ec_pubkey_serialize(ctx, pub, &len, &point, SECP256K1_EC_COMPRESSED)) return false;
  // Catches a broken or hacked secp256k1 build (e.g. one whose pubkey_create
  // returns success with an all-zero key).
  return len == SESSIONLESS_PUBLIC_KEY_SIZE && (pub[0] == 0x02 || pub[0] == 0x03) && !allZero(pub + 1, 32);
}

bool verify(const uint8_t signature[SESSIONLESS_SIGNATURE_SIZE], const uint8_t *message, size_t len,
            const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) {
  if (!signature || !publicKey || (!message && len)) return false;
  secp256k1_context *ctx = context();
  if (!ctx) return false;
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_parse(ctx, &pub, publicKey, SESSIONLESS_PUBLIC_KEY_SIZE)) return false;
  secp256k1_ecdsa_signature sig;
  if (!secp256k1_ecdsa_signature_parse_compact(ctx, &sig, signature)) return false;
  uint8_t hash[32];
  hashMessage(message, len, hash);
  return secp256k1_ecdsa_verify(ctx, &sig, hash, &pub) == 1;
}

bool verifyHex(const char *signatureHex, const char *message, const char *publicKeyHex) {
  uint8_t sig[SESSIONLESS_SIGNATURE_SIZE];
  uint8_t pub[SESSIONLESS_PUBLIC_KEY_SIZE];
  if (!message || !fromHex(signatureHex, sig, sizeof(sig)) || !fromHex(publicKeyHex, pub, sizeof(pub))) return false;
  return verify(sig, reinterpret_cast<const uint8_t *>(message), strlen(message), pub);
}

// ---------------------------------------------------------------------------
// stores

MemoryKeyStore::~MemoryKeyStore() { wipe(key_, sizeof(key_)); }

Status MemoryKeyStore::load(uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
                            uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE], bool &hasPublicKey) {
  if (!has_) return Status::NoKeys;
  memcpy(privateKey, key_, sizeof(key_));
  memcpy(publicKey, pub_, sizeof(pub_));
  hasPublicKey = true;
  return Status::Ok;
}

Status MemoryKeyStore::save(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
                            const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) {
  memcpy(key_, privateKey, sizeof(key_));
  memcpy(pub_, publicKey, sizeof(pub_));
  has_ = true;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// Identity

Identity::~Identity() { clear(); }

void Identity::clear() {
  wipe(privateKey_, sizeof(privateKey_));
  memset(publicKey_, 0, sizeof(publicKey_));
  ready_ = false;
}

Status Identity::adopt(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], const uint8_t *storedPublicKey) {
  uint8_t pub[SESSIONLESS_PUBLIC_KEY_SIZE];
  if (!derivePublicKey(privateKey, pub)) return Status::CorruptKeys;
  if (storedPublicKey && memcmp(storedPublicKey, pub, sizeof(pub)) != 0) return Status::CorruptKeys;
  blindContext(defaultRandom());

  // Self-test: a key we can't sign and verify with must never be used or saved.
  static const uint8_t probe[] = "sessionless-kit self-test";
  uint8_t hash[32];
  hashMessage(probe, sizeof(probe) - 1, hash);
  secp256k1_ecdsa_signature sig;
  uint8_t compact[SESSIONLESS_SIGNATURE_SIZE];
  secp256k1_context *ctx = context();
  secp256k1_pubkey point;
  bool ok = secp256k1_ecdsa_sign(ctx, &sig, hash, privateKey, secp256k1_nonce_function_rfc6979, nullptr) &&
            secp256k1_ecdsa_signature_serialize_compact(ctx, compact, &sig) &&
            secp256k1_ec_pubkey_parse(ctx, &point, pub, sizeof(pub)) &&
            secp256k1_ecdsa_verify(ctx, &sig, hash, &point);
  if (!ok) return Status::CryptoError;

  memcpy(privateKey_, privateKey, SESSIONLESS_PRIVATE_KEY_SIZE);
  memcpy(publicKey_, pub, sizeof(pub));
  ready_ = true;
  return Status::Ok;
}

Status Identity::generate(RandomFn rng, uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE]) {
  if (!rng) return Status::RngError;
  secp256k1_context *ctx = context();
  if (!ctx) return Status::CryptoError;
  // A random 32 bytes is out of range with probability ~2^-128; retry a few times to be exact.
  for (int attempt = 0; attempt < 8; attempt++) {
    if (!rng(privateKey, SESSIONLESS_PRIVATE_KEY_SIZE)) return Status::RngError;
    if (!allZero(privateKey, SESSIONLESS_PRIVATE_KEY_SIZE) && secp256k1_ec_seckey_verify(ctx, privateKey)) {
      blindContext(rng);
      return Status::Ok;
    }
  }
  return Status::RngError;  // the source keeps returning invalid keys: it is broken or stuck
}

Status Identity::load(KeyStore &store) {
  uint8_t priv[SESSIONLESS_PRIVATE_KEY_SIZE];
  uint8_t storedPub[SESSIONLESS_PUBLIC_KEY_SIZE];
  bool hasPub = false;
  Status s = store.load(priv, storedPub, hasPub);
  if (s == Status::Ok) s = adopt(priv, hasPub ? storedPub : nullptr);
  wipe(priv, sizeof(priv));
  return s;
}

Status Identity::create(KeyStore &store, RandomFn rng) {
  uint8_t existing[SESSIONLESS_PRIVATE_KEY_SIZE];
  uint8_t existingPub[SESSIONLESS_PUBLIC_KEY_SIZE];
  bool hasPub = false;
  Status s = store.load(existing, existingPub, hasPub);
  wipe(existing, sizeof(existing));
  if (s == Status::Ok || s == Status::CorruptKeys) return Status::KeysExist;  // corrupt keys are never silently replaced
  if (s != Status::NoKeys) return s;

  uint8_t priv[SESSIONLESS_PRIVATE_KEY_SIZE];
  s = generate(rng, priv);
  if (s == Status::Ok) s = adopt(priv);
  if (s == Status::Ok) {
    s = store.save(priv, publicKey_);
    if (s != Status::Ok) clear();  // a key that wasn't persisted would give a different identity after reset
  }
  wipe(priv, sizeof(priv));
  return s;
}

Status Identity::loadOrCreate(KeyStore &store, RandomFn rng) {
  Status s = load(store);
  if (s == Status::NoKeys) return create(store, rng);
  return s;
}

Status Identity::importPrivateKeyHex(KeyStore &store, const char *privateKeyHex) {
  uint8_t priv[SESSIONLESS_PRIVATE_KEY_SIZE];
  if (!fromHex(privateKeyHex, priv, sizeof(priv))) return Status::BadArgument;

  uint8_t existing[SESSIONLESS_PRIVATE_KEY_SIZE];
  uint8_t existingPub[SESSIONLESS_PUBLIC_KEY_SIZE];
  bool hasPub = false;
  Status s = store.load(existing, existingPub, hasPub);
  bool same = (s == Status::Ok) && memcmp(existing, priv, sizeof(priv)) == 0;
  wipe(existing, sizeof(existing));
  if (s == Status::CorruptKeys || (s == Status::Ok && !same)) { wipe(priv, sizeof(priv)); return Status::KeysExist; }
  if (s != Status::Ok && s != Status::NoKeys) { wipe(priv, sizeof(priv)); return s; }

  s = adopt(priv);
  if (s == Status::CorruptKeys) s = Status::BadArgument;  // out-of-range key supplied by the caller
  if (s == Status::Ok && !same) {
    s = store.save(priv, publicKey_);
    if (s != Status::Ok) clear();
  }
  wipe(priv, sizeof(priv));
  return s;
}

Status Identity::ephemeral(RandomFn rng) {
  uint8_t priv[SESSIONLESS_PRIVATE_KEY_SIZE];
  Status s = generate(rng, priv);
  if (s == Status::Ok) s = adopt(priv);
  wipe(priv, sizeof(priv));
  return s;
}

Status Identity::publicKeyHex(char out[SESSIONLESS_PUBLIC_KEY_HEX_LEN]) const {
  if (!ready_) return Status::NotReady;
  toHex(publicKey_, SESSIONLESS_PUBLIC_KEY_SIZE, out);
  return Status::Ok;
}

Status Identity::sign(const uint8_t *message, size_t len, uint8_t signature[SESSIONLESS_SIGNATURE_SIZE]) const {
  if (!ready_) return Status::NotReady;
  if (!signature || (!message && len)) return Status::BadArgument;
  uint8_t hash[32];
  hashMessage(message, len, hash);
  secp256k1_context *ctx = context();
  secp256k1_ecdsa_signature sig;
  // RFC6979 deterministic nonce: no RNG needed at signing time, same input => same signature.
  if (!secp256k1_ecdsa_sign(ctx, &sig, hash, privateKey_, secp256k1_nonce_function_rfc6979, nullptr)) return Status::CryptoError;
  if (!secp256k1_ecdsa_signature_serialize_compact(ctx, signature, &sig)) return Status::CryptoError;
  return Status::Ok;
}

Status Identity::sign(const char *message, uint8_t signature[SESSIONLESS_SIGNATURE_SIZE]) const {
  if (!message) return Status::BadArgument;
  return sign(reinterpret_cast<const uint8_t *>(message), strlen(message), signature);
}

Status Identity::signHex(const char *message, char out[SESSIONLESS_SIGNATURE_HEX_LEN]) const {
  uint8_t sig[SESSIONLESS_SIGNATURE_SIZE];
  Status s = sign(message, sig);
  if (s == Status::Ok) toHex(sig, sizeof(sig), out);
  return s;
}

bool Identity::verify(const uint8_t signature[SESSIONLESS_SIGNATURE_SIZE], const uint8_t *message, size_t len) const {
  return ready_ && sessionless::verify(signature, message, len, publicKey_);
}

Status Identity::exportPrivateKeyHex(char out[SESSIONLESS_PRIVATE_KEY_HEX_LEN]) const {
  if (!ready_) return Status::NotReady;
  toHex(privateKey_, SESSIONLESS_PRIVATE_KEY_SIZE, out);
  return Status::Ok;
}

}  // namespace sessionless
