// ESP32 flash storage for the ordinal counter. Same key the wand demo already uses
// ("ordinal", a 32-bit uint), so a device keeps counting where it left off.
#ifndef MAGIC_NVS_H
#define MAGIC_NVS_H

#if !defined(ARDUINO_ARCH_ESP32)
#error "MagicNvs.h needs the ESP32 Arduino core (Preferences). Use MemoryOrdinalStore or your own OrdinalStore elsewhere."
#endif

#include <Preferences.h>

#include "MagicCore.h"

namespace magic {

// Uses a Preferences you already begin()-ed read-write (e.g. the same one as NvsKeyStore).
class NvsOrdinalStore : public OrdinalStore {
 public:
  explicit NvsOrdinalStore(Preferences &open, const char *key = "ordinal") : prefs_(open), key_(key) {}
  Status load(uint32_t *value) override {
    if (!prefs_.isKey(key_)) return Status::NotFound;
    *value = prefs_.getUInt(key_, 0);
    return Status::Ok;
  }
  Status save(uint32_t value) override {
    return prefs_.putUInt(key_, value) == sizeof(uint32_t) ? Status::Ok : Status::StoreError;
  }
 private:
  Preferences &prefs_;
  const char *key_;
};

}  // namespace magic

#endif
