// MagicGateway: receives casts from wands over any magic::Transport, adds its own signed gateway
// entry (and optional `extra`, <= 128 bytes), and resolves the spell through any magic::Resolver.
// What the gateway DOES when a spell arrives (light an LED, move a servo, draw on a screen) is
// the GatewayHandler you supply: that is the "arbitrary" part, bounded by MAGIC_MAX_* limits.
//
//   magic::Gateway gw(identity, wandLink, resolver, ordinals, uuids, monotonicMs, config, &handler);
//   gw.setEpoch(ntpEpochMs);  gw.begin();           // once the clock is known
//   void loop() { gw.poll(); }
//
// poll() blocks while the resolver round trip is in flight (as the original target did).
#ifndef MAGIC_GATEWAY_H
#define MAGIC_GATEWAY_H

#include "MagicCore.h"

namespace magic {

// A cast as the handler sees it.
struct CastInfo {
  char casterUUID[MAGIC_FRAME_UUID_BYTES];
  uint64_t timestamp;
  uint32_t ordinal;
  const SpellDef *spell;
  PeerId from;
};

class GatewayHandler {
 public:
  virtual ~GatewayHandler() {}
  // A well-formed, fresh cast arrived and the gateway is about to resolve it. Fires BEFORE the
  // resolver round trip, so a reaction can be instant. The cast may still fail to resolve, so
  // for anything that should only happen on success, use onResolved.
  virtual void onCast(const CastInfo &cast) { (void)cast; }
  // Write this gateway's `extra` JSON object for the spell into `out` (at most `cap` bytes, no NUL
  // needed) and return its length, or 0 for none. It is validated (valid JSON object, <= 128 bytes,
  // ASCII, depth <= 4); an invalid extra aborts the cast rather than being sent.
  virtual size_t extra(const CastInfo &cast, char *out, size_t cap) { (void)cast; (void)out; (void)cap; return 0; }
  // The cast finished. `status` is Ok when the resolver answered (see `success`), otherwise why not.
  virtual void onResolved(const CastInfo &cast, bool success, Status status) { (void)cast; (void)success; (void)status; }
};

struct GatewayConfig {
  SpellDef spell;                       // the one spell this gateway serves (v1 frames don't name the spell)
  uint64_t minimumCost = 0;             // this gateway's fee, signed into its entry
  uint32_t timeSyncIntervalMs = 30000;  // how often to broadcast TimeSync to wands
  uint64_t maxClockSkewMs = 300000;     // refuse casts/registrations stamped further than this from our clock (resolver uses 5 min)
};

class Gateway {
 public:
  Gateway(sessionless::Identity &identity, Transport &wandLink, Resolver &resolver, OrdinalStore &ordinals,
          UuidStore &uuids, MonotonicFn monotonicMs, const GatewayConfig &config, GatewayHandler *handler = nullptr);

  // Tell the gateway real time (from NTP, an RTC, GPS...). Required before begin(): signatures and
  // freshness checks need it. Call again periodically to correct drift.
  void setEpoch(uint64_t epochMs);

  // Register this gateway with the resolver unless it already has a stored UUID. Safe to call again.
  // NotSynced (no setEpoch yet), ResolverUnreachable/ResolverRejected/BadResponse from the resolver.
  Status begin();

  bool ready() const { return ready_; }
  const char *uuid() const { return uuid_; }

  // Serve wands: send periodic TimeSync, handle registrations and casts. Call every loop().
  void poll();

 private:
  void handleRegister(const PeerId &from, const uint8_t *frame, size_t len);
  void handleCast(const PeerId &from, const uint8_t *frame, size_t len);
  void replyRegister(const PeerId &to, bool success, const char *uuid);
  void replyResult(const PeerId &to, bool success);
  Status resolveCast(const CastSpellFrame &f, const CastInfo &info, bool *success);

  sessionless::Identity &id_;
  Transport &link_;
  Resolver &resolver_;
  OrdinalCounter counter_;
  UuidStore &uuids_;
  MonotonicFn mono_;
  GatewayConfig cfg_;
  GatewayHandler *handler_;

  EpochClock clock_;
  char uuid_[MAGIC_FRAME_UUID_BYTES] = {0};
  bool ready_ = false;
  bool sentSync_ = false;
  uint64_t lastSyncSent_ = 0;
  char body_[MAGIC_MAX_SPELL_BODY_BYTES + 1];
};

}  // namespace magic

#endif
