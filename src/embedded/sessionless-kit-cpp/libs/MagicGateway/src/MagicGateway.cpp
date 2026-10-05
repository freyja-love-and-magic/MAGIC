#include "MagicGateway.h"

#include <string.h>

namespace magic {

Gateway::Gateway(sessionless::Identity &identity, Transport &wandLink, Resolver &resolver, OrdinalStore &ordinals,
                 UuidStore &uuids, MonotonicFn monotonicMs, const GatewayConfig &config, GatewayHandler *handler)
    : id_(identity), link_(wandLink), resolver_(resolver), counter_(ordinals), uuids_(uuids), mono_(monotonicMs),
      cfg_(config), handler_(handler) {
  body_[0] = '\0';
}

void Gateway::setEpoch(uint64_t epochMs) { clock_.sync(epochMs, mono_()); }

Status Gateway::begin() {
  if (!id_.ready()) return Status::SignFailed;
  if (!isSafeId(cfg_.spell.name, MAGIC_SPELL_NAME_MAX_CHARS)) return Status::BadArgument;
  if (uuids_.load(PeerId(), uuid_) == Status::Ok) {
    ready_ = true;
    return Status::Ok;
  }
  uint64_t epoch;
  Status s = clock_.now(mono_(), &epoch);
  if (s != Status::Ok) return s;  // NotSynced: registration is signed with the time

  char pubHex[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  id_.publicKeyHex(pubHex);
  char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN];
  s = signRegistration(id_, epoch, sigHex);
  if (s != Status::Ok) return s;
  char uuid[MAGIC_FRAME_UUID_BYTES];
  s = resolver_.registerUser(epoch, pubHex, sigHex, uuid);
  if (s != Status::Ok) return s;
  if (uuids_.save(PeerId(), uuid) != Status::Ok) return Status::StoreError;
  memcpy(uuid_, uuid, MAGIC_FRAME_UUID_BYTES);
  ready_ = true;
  return Status::Ok;
}

void Gateway::poll() {
  uint64_t now = mono_();
  uint64_t epoch;
  if (clock_.now(now, &epoch) == Status::Ok && (!sentSync_ || now - lastSyncSent_ >= cfg_.timeSyncIntervalMs)) {
    TimeSync ts = {epoch};
    uint8_t frame[MAGIC_FRAME_TIME_SYNC_SIZE];
    size_t n = encode(ts, frame, sizeof(frame));
    if (n && link_.send(nullptr, frame, n) == Status::Ok) {
      sentSync_ = true;
      lastSyncSent_ = now;
    }
  }

  uint8_t frame[MAGIC_FRAME_MAX_SIZE];
  size_t len = 0;
  PeerId from;
  for (int i = 0; i < 8 && link_.receive(frame, sizeof(frame), &len, &from); i++) {
    switch (peekType(frame, len)) {
      case MSG_REGISTER_REQUEST: handleRegister(from, frame, len); break;
      case MSG_CAST_SPELL: handleCast(from, frame, len); break;
      default: break;  // TimeSync/results are for wands; garbage is dropped
    }
  }
}

void Gateway::replyRegister(const PeerId &to, bool success, const char *uuid) {
  RegisterResponse r;
  memset(&r, 0, sizeof(r));
  r.success = success;
  if (success) memcpy(r.casterUUID, uuid, strlen(uuid));
  uint8_t frame[MAGIC_FRAME_REGISTER_RESPONSE_SIZE];
  size_t n = encode(r, frame, sizeof(frame));
  if (n) link_.send(&to, frame, n);
}

void Gateway::replyResult(const PeerId &to, bool success) {
  CastResult r = {success};
  uint8_t frame[MAGIC_FRAME_CAST_RESULT_SIZE];
  size_t n = encode(r, frame, sizeof(frame));
  if (n) link_.send(&to, frame, n);
}

static bool withinSkew(uint64_t a, uint64_t b, uint64_t skew) { return (a > b ? a - b : b - a) <= skew; }

void Gateway::handleRegister(const PeerId &from, const uint8_t *frame, size_t len) {
  RegisterRequest req;
  if (decode(frame, len, req) != Status::Ok) return;
  uint64_t epoch;
  if (clock_.now(mono_(), &epoch) != Status::Ok || !withinSkew(epoch, req.timestamp, cfg_.maxClockSkewMs)) {
    replyRegister(from, false, nullptr);
    return;
  }

  // Check the wand's signature ourselves before spending a network round trip on it.
  char pubHex[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  sessionless::toHex(req.pubKey, SESSIONLESS_PUBLIC_KEY_SIZE, pubHex);
  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  size_t msgLen = registrationMessage(msg, sizeof(msg), req.timestamp, pubHex);
  if (msgLen == 0 || !sessionless::verify(req.signature, reinterpret_cast<const uint8_t *>(msg), msgLen, req.pubKey)) {
    replyRegister(from, false, nullptr);
    return;
  }

  char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN];
  sessionless::toHex(req.signature, SESSIONLESS_SIGNATURE_SIZE, sigHex);
  char uuid[MAGIC_FRAME_UUID_BYTES];
  if (resolver_.registerUser(req.timestamp, pubHex, sigHex, uuid) != Status::Ok) {
    replyRegister(from, false, nullptr);
    return;
  }
  replyRegister(from, true, uuid);
}

Status Gateway::resolveCast(const CastSpellFrame &f, const CastInfo &info, bool *success) {
  *success = false;
  uint64_t epoch;
  Status s = clock_.now(mono_(), &epoch);
  if (s != Status::Ok) return s;

  uint32_t gwOrdinal;
  s = counter_.next(&gwOrdinal);
  if (s != Status::Ok) return s;
  char gwSig[SESSIONLESS_SIGNATURE_HEX_LEN];
  s = signGateway(id_, epoch, uuid_, cfg_.minimumCost, gwOrdinal, gwSig);
  if (s != Status::Ok) return s;

  char extra[MAGIC_MAX_GATEWAY_EXTRA_BYTES + 1];
  extra[0] = '\0';
  if (handler_) {
    size_t n = handler_->extra(info, extra, MAGIC_MAX_GATEWAY_EXTRA_BYTES);
    if (n > MAGIC_MAX_GATEWAY_EXTRA_BYTES) return Status::ExtraTooLarge;
    extra[n] = '\0';
  }

  char casterSigHex[SESSIONLESS_SIGNATURE_HEX_LEN];
  sessionless::toHex(f.casterSignature, SESSIONLESS_SIGNATURE_SIZE, casterSigHex);
  SpellFields spell = {f.timestamp, cfg_.spell.name, f.casterUUID, cfg_.spell.totalCost, cfg_.spell.mp, f.ordinal, casterSigHex};
  GatewayFields gateway = {epoch, uuid_, cfg_.minimumCost, gwOrdinal, gwSig, extra};

  size_t bodyLen = 0;
  s = writeSpellJson(body_, sizeof(body_), &bodyLen, spell, &gateway, 1);  // enforces the 128/2048 limits
  if (s != Status::Ok) return s;

  ResolveRequest req = {cfg_.spell.name, body_, bodyLen, &spell, &gateway, 1};
  ResolveResult result = {false};
  s = resolver_.resolve(req, &result);
  if (s == Status::Ok) *success = result.success;
  return s;
}

void Gateway::handleCast(const PeerId &from, const uint8_t *frame, size_t len) {
  CastSpellFrame f;
  if (decode(frame, len, f) != Status::Ok) return;

  CastInfo info;
  memset(&info, 0, sizeof(info));
  memcpy(info.casterUUID, f.casterUUID, MAGIC_FRAME_UUID_BYTES);
  info.timestamp = f.timestamp;
  info.ordinal = f.ordinal;
  info.spell = &cfg_.spell;
  info.from = from;

  bool success = false;
  Status s = Status::Ok;
  uint64_t epoch;
  if (!ready_) {
    s = Status::NotRegistered;
  } else if ((s = clock_.now(mono_(), &epoch)) == Status::Ok && !withinSkew(epoch, f.timestamp, cfg_.maxClockSkewMs)) {
    s = Status::ClockSkew;  // stale or from the future: don't spend a round trip, and don't light the LED
  }

  if (s == Status::Ok) {
    if (handler_) handler_->onCast(info);
    s = resolveCast(f, info, &success);
  }
  replyResult(from, s == Status::Ok && success);
  if (handler_) handler_->onResolved(info, s == Status::Ok && success, s);
}

}  // namespace magic
