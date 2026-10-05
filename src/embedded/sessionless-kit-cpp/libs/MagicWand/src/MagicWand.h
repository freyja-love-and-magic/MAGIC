// MagicWand: the caster side of MAGIC. Runs over any magic::Transport (ESP-NOW, BLE, LoRa, loopback...)
// and has no WiFi/network dependency: a wand never talks to the resolver itself, its gateway does.
//
//   magic::Wand wand(identity, transport, ordinalStore, uuidStore, monotonicMs, &listener);
//   void loop() { wand.poll(); if (buttonPressed) wand.cast(spell); }
//
// Lifecycle: poll() until timeSynced() (a gateway's TimeSync frame arrives), call
// registerWithGateway() once (or whenever registered() is false), then cast().
#ifndef MAGIC_WAND_H
#define MAGIC_WAND_H

#include "MagicCore.h"

namespace magic {

// Override what you care about. Called from poll(), on the caller's thread.
class WandListener {
 public:
  virtual ~WandListener() {}
  virtual void onTimeSynced(uint64_t epochMs) { (void)epochMs; }
  virtual void onRegistered(const char *casterUUID) { (void)casterUUID; }
  virtual void onRegistrationFailed() {}
  // The gateway reported how the spell resolved. Advisory: link frames are unauthenticated.
  virtual void onResult(bool success) { (void)success; }
  // No CastResult arrived in time (the gateway may still have resolved the spell).
  virtual void onTimeout() {}
};

struct WandOptions {
  uint32_t resultTimeoutMs = 10000;        // wait this long for a CastResult
  uint32_t registrationTimeoutMs = 10000;  // wait this long for a RegisterResponse
};

class Wand {
 public:
  Wand(sessionless::Identity &identity, Transport &link, OrdinalStore &ordinals, UuidStore &uuids,
       MonotonicFn monotonicMs, WandListener *listener = nullptr, const WandOptions &options = WandOptions());

  // Process incoming frames and timeouts. Call often (every loop()). Bounded work per call.
  void poll();

  // By default the wand follows whichever gateway most recently sent it a TimeSync. TimeSync frames
  // are NOT authenticated, so in a hostile radio environment pin the one gateway you trust: frames
  // from any other peer are then ignored.
  void pinGateway(const PeerId &gateway);

  bool timeSynced() const { return clock_.synced(); }
  // True if there has been no TimeSync for more than `maxAgeMs` (or never). A wand that moved to a new
  // venue stops hearing its old gateway; use this to resume scanning for a new one. Casts still work off
  // the extrapolated clock meanwhile, which stays accurate to well within the resolver's 5 minute window.
  bool timeSyncStale(uint64_t maxAgeMs) const { return !clock_.synced() || mono_() - lastSyncMono_ > maxAgeMs; }
  bool registered() const { return registered_; }
  bool castPending() const { return pending_; }
  const PeerId &gateway() const { return gateway_; }
  const char *casterUUID() const { return uuid_; }  // empty until registered()

  // Ask the current gateway to register this wand with its resolver. Result arrives via the listener.
  // NotSynced until a TimeSync from a gateway has been received.
  Status registerWithGateway();

  // Sign and send a spell. Ok means "sent" (see onResult for the outcome). Errors, checked in order,
  // before any ordinal is used: SignFailed (no keys), Busy, NotSynced, NotRegistered, BadArgument.
  // `spell` must match what the gateway is configured for (the v1 frame doesn't carry it).
  Status cast(const SpellDef &spell);

 private:
  void handleTimeSync(const PeerId &from, const uint8_t *frame, size_t len);
  void handleRegisterResponse(const uint8_t *frame, size_t len);
  void handleCastResult(const uint8_t *frame, size_t len);

  sessionless::Identity &id_;
  Transport &link_;
  OrdinalCounter counter_;
  UuidStore &uuids_;
  MonotonicFn mono_;
  WandListener *listener_;
  WandOptions options_;

  EpochClock clock_;
  uint64_t lastSyncMono_ = 0;
  PeerId gateway_;
  bool havePeer_ = false;
  bool pinned_ = false;
  char uuid_[MAGIC_FRAME_UUID_BYTES] = {0};
  bool registered_ = false;
  bool awaitingRegistration_ = false;
  uint64_t registrationDeadline_ = 0;
  bool pending_ = false;
  uint64_t resultDeadline_ = 0;
};

}  // namespace magic

#endif
