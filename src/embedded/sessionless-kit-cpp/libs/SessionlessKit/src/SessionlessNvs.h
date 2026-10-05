// ESP32 flash storage (NVS, via the Arduino Preferences API) for SessionlessKit.
//
// Defaults match the key names the existing MAGIC demos already use ("privKey"
// and "pubKey"), so a device flashed with the old firmware keeps its identity
// (and its fount registration) when you switch to this library. Only the
// namespace differs between demos ("wand", ...), so pass yours.
//
//   sessionless::NvsKeyStore store("wand");
//   // or share a Preferences you already opened with begin():
//   sessionless::NvsKeyStore store(preferences);
#ifndef SESSIONLESS_NVS_H
#define SESSIONLESS_NVS_H

#if !defined(ARDUINO_ARCH_ESP32)
#error "SessionlessNvs.h needs the ESP32 Arduino core (Preferences). Use MemoryKeyStore, SessionlessFileStore.h, or your own KeyStore elsewhere."
#endif

#include <Preferences.h>
#include <string.h>

#include "SessionlessKit.h"

namespace sessionless {

class NvsKeyStore : public KeyStore {
 public:
  // Opens its own handle to `ns` on first use.
  explicit NvsKeyStore(const char *ns, const char *privateKeyName = "privKey", const char *publicKeyName = "pubKey")
      : ns_(ns), priv_(privateKeyName), pub_(publicKeyName) {}
  // Uses a Preferences the caller already begin()-ed (read-write).
  explicit NvsKeyStore(Preferences &open, const char *privateKeyName = "privKey", const char *publicKeyName = "pubKey")
      : prefs_(&open), priv_(privateKeyName), pub_(publicKeyName) {}
  ~NvsKeyStore() override {
    if (opened_) own_.end();
  }

  Status load(uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE],
              bool &hasPublicKey) override {
    Preferences *p = prefs();
    if (!p) return Status::StoreError;
    size_t privLen = p->getBytesLength(priv_);
    if (privLen == 0) return Status::NoKeys;
    if (privLen != SESSIONLESS_PRIVATE_KEY_SIZE) return Status::CorruptKeys;
    if (p->getBytes(priv_, privateKey, SESSIONLESS_PRIVATE_KEY_SIZE) != SESSIONLESS_PRIVATE_KEY_SIZE) return Status::StoreError;

    hasPublicKey = false;
    size_t pubLen = p->getBytesLength(pub_);
    if (pubLen == SESSIONLESS_PUBLIC_KEY_SIZE) {
      hasPublicKey = p->getBytes(pub_, publicKey, SESSIONLESS_PUBLIC_KEY_SIZE) == SESSIONLESS_PUBLIC_KEY_SIZE;
    } else if (pubLen != 0) {
      return Status::CorruptKeys;
    }
    return Status::Ok;
  }

  Status save(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) override {
    Preferences *p = prefs();
    if (!p) return Status::StoreError;
    if (p->putBytes(priv_, privateKey, SESSIONLESS_PRIVATE_KEY_SIZE) != SESSIONLESS_PRIVATE_KEY_SIZE) return Status::StoreError;
    if (p->putBytes(pub_, publicKey, SESSIONLESS_PUBLIC_KEY_SIZE) != SESSIONLESS_PUBLIC_KEY_SIZE) return Status::StoreError;
    return Status::Ok;
  }

 private:
  Preferences *prefs() {
    if (prefs_) return prefs_;
    if (!opened_ && own_.begin(ns_, false)) opened_ = true;
    return opened_ ? &own_ : nullptr;
  }

  const char *ns_ = nullptr;
  const char *priv_;
  const char *pub_;
  Preferences own_;
  Preferences *prefs_ = nullptr;
  bool opened_ = false;
};

}  // namespace sessionless

#endif
