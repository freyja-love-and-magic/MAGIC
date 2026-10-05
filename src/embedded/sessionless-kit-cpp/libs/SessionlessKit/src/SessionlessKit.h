// SessionlessKit: Sessionless keys for microcontrollers (and hosts).
// Create, save, sign, verify - in a handful of lines, with no global state.
//
// Wire format (matches sessionless-node / sessionless-kit JS):
//   signature = secp256k1 ECDSA (RFC6979, low-S) over keccak256(message bytes),
//               64-byte compact r||s
//   public key = 33-byte compressed point
//
//   sessionless::NvsKeyStore store("wand");        // ESP32 flash; see SessionlessNvs.h
//   sessionless::Identity me;
//   if (me.loadOrCreate(store) != sessionless::Status::Ok) { /* handle it */ }
//   char sig[SESSIONLESS_SIGNATURE_HEX_LEN];
//   me.signHex("hello", sig);
#ifndef SESSIONLESS_KIT_H
#define SESSIONLESS_KIT_H

#include <stddef.h>
#include <stdint.h>

#define SESSIONLESS_PRIVATE_KEY_SIZE 32
#define SESSIONLESS_PUBLIC_KEY_SIZE 33
#define SESSIONLESS_SIGNATURE_SIZE 64

// Buffer sizes for hex output, including the terminating NUL.
#define SESSIONLESS_PRIVATE_KEY_HEX_LEN (SESSIONLESS_PRIVATE_KEY_SIZE * 2 + 1)
#define SESSIONLESS_PUBLIC_KEY_HEX_LEN (SESSIONLESS_PUBLIC_KEY_SIZE * 2 + 1)
#define SESSIONLESS_SIGNATURE_HEX_LEN (SESSIONLESS_SIGNATURE_SIZE * 2 + 1)

// libsecp256k1 is stack-heavy. On the ESP32 Arduino core the default 8KB loop
// task stack is NOT enough for key generation: it does not fail cleanly, it
// silently corrupts memory (symptom: an "illegal argument" abort claiming an
// all-zero pubkey). Put this at file scope in the sketch that owns setup():
//   SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);
// If you run crypto in your own FreeRTOS task, give that task at least this much.
#define SESSIONLESS_STACK_BYTES (32 * 1024)

namespace sessionless {

enum class Status {
  Ok = 0,
  NoKeys,       // nothing stored yet
  StoreError,   // the store could not be read or written
  CorruptKeys,  // stored keys are invalid or inconsistent; never overwritten
  KeysExist,    // create()/import() would replace different existing keys
  RngError,     // no usable random source (or it failed)
  CryptoError,  // library failure, including the self-test after key creation
  BadArgument,
  NotReady      // Identity has no keys loaded
};

const char *statusName(Status s);

// Fill `out` with `len` cryptographically secure random bytes; false on failure.
typedef bool (*RandomFn)(uint8_t *out, size_t len);

// The platform's secure random source, or nullptr if there isn't one.
//   ESP32: esp_fill_random(). That is a TRUE random source only while the RF
//          subsystem (WiFi/BT) is active or bootloader_random is enabled;
//          otherwise it is pseudo-random. Create keys with WiFi on (the wand
//          and targets all do) or pass your own RandomFn.
//   macOS/Linux: getentropy().
// There is deliberately NO rand()/time() fallback. Unknown platform => nullptr
// => RngError, rather than quietly weak keys.
RandomFn defaultRandom();

// Where the keys live. Implement this for your platform's storage.
// The private key is authoritative. The public key is stored alongside it so
// other firmware/tools can read it and so a mismatched pair is caught: if
// load() reports hasPublicKey, Identity checks it against the one derived from
// the private key and returns CorruptKeys on disagreement.
// load() returns Ok, NoKeys (nothing stored), CorruptKeys, or StoreError.
class KeyStore {
 public:
  virtual ~KeyStore() {}
  virtual Status load(uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
                      uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE], bool &hasPublicKey) = 0;
  virtual Status save(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
                      const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) = 0;
};

// RAM only. Keys vanish on reset. For tests and throwaway identities.
class MemoryKeyStore : public KeyStore {
 public:
  ~MemoryKeyStore() override;
  Status load(uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
              uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE], bool &hasPublicKey) override;
  Status save(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE],
              const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) override;
 private:
  uint8_t key_[SESSIONLESS_PRIVATE_KEY_SIZE] = {0};
  uint8_t pub_[SESSIONLESS_PUBLIC_KEY_SIZE] = {0};
  bool has_ = false;
};

class Identity {
 public:
  Identity() {}
  ~Identity();
  Identity(const Identity &) = delete;
  Identity &operator=(const Identity &) = delete;

  // Load keys, or create and save a new pair on first use. Never overwrites existing keys.
  Status loadOrCreate(KeyStore &store, RandomFn rng = defaultRandom());
  // Load keys that must already exist (NoKeys if they don't).
  Status load(KeyStore &store);
  // Create new keys. KeysExist if the store already holds any.
  Status create(KeyStore &store, RandomFn rng = defaultRandom());
  // Store a known private key (64 hex chars), e.g. a recovery key.
  // KeysExist if the store holds a different key.
  Status importPrivateKeyHex(KeyStore &store, const char *privateKeyHex);
  // In-memory only, never saved.
  Status ephemeral(RandomFn rng = defaultRandom());

  bool ready() const { return ready_; }
  const uint8_t *publicKey() const { return publicKey_; }  // 33 bytes, valid when ready()
  Status publicKeyHex(char out[SESSIONLESS_PUBLIC_KEY_HEX_LEN]) const;

  // Sign `len` bytes of message. Output is 64 bytes.
  Status sign(const uint8_t *message, size_t len, uint8_t signature[SESSIONLESS_SIGNATURE_SIZE]) const;
  // Sign a NUL-terminated string (how Sessionless messages are normally built).
  Status sign(const char *message, uint8_t signature[SESSIONLESS_SIGNATURE_SIZE]) const;
  // Same, as 128 lowercase hex chars + NUL (what servers like fount expect).
  Status signHex(const char *message, char out[SESSIONLESS_SIGNATURE_HEX_LEN]) const;
  bool verify(const uint8_t signature[SESSIONLESS_SIGNATURE_SIZE], const uint8_t *message, size_t len) const;

  // Backup/migration. Treat as a password. Returns false if not ready.
  Status exportPrivateKeyHex(char out[SESSIONLESS_PRIVATE_KEY_HEX_LEN]) const;

 private:
  Status adopt(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], const uint8_t *storedPublicKey = nullptr);
  Status generate(RandomFn rng, uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE]);
  void clear();
  uint8_t privateKey_[SESSIONLESS_PRIVATE_KEY_SIZE] = {0};
  uint8_t publicKey_[SESSIONLESS_PUBLIC_KEY_SIZE] = {0};
  bool ready_ = false;
};

// Stateless: needs no keys, so servers and gateways can verify without an Identity.
// Returns false (never crashes) on malformed input or a bad public key.
bool verify(const uint8_t signature[SESSIONLESS_SIGNATURE_SIZE], const uint8_t *message, size_t len,
            const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]);
bool verifyHex(const char *signatureHex, const char *message, const char *publicKeyHex);

// Lowercase hex helpers. toHex writes 2*len chars + NUL. fromHex returns false
// unless `hex` is exactly 2*len valid hex characters (either case).
void toHex(const uint8_t *bytes, size_t len, char *out);
bool fromHex(const char *hex, uint8_t *out, size_t len);

}  // namespace sessionless

#endif
