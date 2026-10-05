// Flash (NVS) storage of resolver-assigned UUIDs for the ESP32. Pairs with SessionlessNvs.h (keys)
// and MagicNvs.h (ordinal): all three can share one Preferences opened with begin(ns, false).
#ifndef MAGIC_ESP_STORES_H
#define MAGIC_ESP_STORES_H

#if !defined(ARDUINO_ARCH_ESP32)
#error "MagicEspStores.h needs the ESP32 Arduino core (Preferences)."
#endif

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "MagicCore.h"

namespace magic {

// One string per scope. A scope is a peer (a wand stores one per gateway it registered through) or the
// empty PeerId (a gateway's own UUID). NVS keys are limited to 15 chars: "u" + up to 7 address bytes
// as hex, or "uself" for the empty scope. Longer peer ids are refused (BadArgument), not truncated.
class NvsUuidStore : public UuidStore {
 public:
  explicit NvsUuidStore(Preferences &open) : prefs_(open) {}

  Status load(const PeerId &scope, char uuid[MAGIC_FRAME_UUID_BYTES]) override {
    char key[16];
    if (!keyFor(scope, key)) return Status::BadArgument;
    if (!prefs_.isKey(key)) return Status::NotFound;
    char tmp[MAGIC_FRAME_UUID_BYTES + 4];
    size_t n = prefs_.getString(key, tmp, sizeof(tmp));
    if (n == 0 || n >= MAGIC_FRAME_UUID_BYTES || !isSafeId(tmp, MAGIC_UUID_MAX_CHARS)) return Status::StoreError;  // corrupt: say so, don't guess
    memcpy(uuid, tmp, n + 1);
    return Status::Ok;
  }

  Status save(const PeerId &scope, const char *uuid) override {
    char key[16];
    if (!keyFor(scope, key) || !isSafeId(uuid, MAGIC_UUID_MAX_CHARS)) return Status::BadArgument;
    return prefs_.putString(key, uuid) == strlen(uuid) ? Status::Ok : Status::StoreError;
  }

 private:
  static bool keyFor(const PeerId &scope, char key[16]) {
    if (scope.len > 7) return false;
    if (scope.len == 0) { strcpy(key, "uself"); return true; }
    int n = snprintf(key, 16, "u");
    for (uint8_t i = 0; i < scope.len; i++) n += snprintf(key + n, 16 - n, "%02x", scope.bytes[i]);
    return true;
  }
  Preferences &prefs_;
};

}  // namespace magic

#endif
