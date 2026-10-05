#include "MagicWand.h"

#include <string.h>

namespace magic {

Wand::Wand(sessionless::Identity &identity, Transport &link, OrdinalStore &ordinals, UuidStore &uuids,
           MonotonicFn monotonicMs, WandListener *listener, const WandOptions &options)
    : id_(identity), link_(link), counter_(ordinals), uuids_(uuids), mono_(monotonicMs), listener_(listener), options_(options) {}

void Wand::pinGateway(const PeerId &gateway) {
  gateway_ = gateway;
  havePeer_ = true;
  pinned_ = true;
  // Pinning is how a wand regains its identity after a reboot without waiting for a TimeSync.
  registered_ = uuids_.load(gateway_, uuid_) == Status::Ok;
  if (!registered_) uuid_[0] = '\0';
}

void Wand::poll() {
  uint8_t frame[MAGIC_FRAME_MAX_SIZE];
  size_t len = 0;
  PeerId from;
  for (int i = 0; i < 8 && link_.receive(frame, sizeof(frame), &len, &from); i++) {
    if (pinned_ && from != gateway_) continue;
    switch (peekType(frame, len)) {
      case MSG_TIME_SYNC: handleTimeSync(from, frame, len); break;
      case MSG_REGISTER_RESPONSE:
        if (havePeer_ && from == gateway_) handleRegisterResponse(frame, len);
        break;
      case MSG_CAST_RESULT:
        if (havePeer_ && from == gateway_) handleCastResult(frame, len);
        break;
      default: break;  // not for us (a wand ignores casts, registrations, garbage)
    }
  }

  uint64_t now = mono_();
  if (pending_ && now >= resultDeadline_) {
    pending_ = false;
    if (listener_) listener_->onTimeout();
  }
  if (awaitingRegistration_ && now >= registrationDeadline_) {
    awaitingRegistration_ = false;
    if (listener_) listener_->onRegistrationFailed();
  }
}

void Wand::handleTimeSync(const PeerId &from, const uint8_t *frame, size_t len) {
  TimeSync ts;
  if (decode(frame, len, ts) != Status::Ok) return;
  if (!pinned_ && (!havePeer_ || from != gateway_)) {
    gateway_ = from;
    havePeer_ = true;
    registered_ = uuids_.load(gateway_, uuid_) == Status::Ok;
    if (!registered_) uuid_[0] = '\0';
  }
  lastSyncMono_ = mono_();
  clock_.sync(ts.epochMs, lastSyncMono_);
  if (listener_) listener_->onTimeSynced(ts.epochMs);
}

void Wand::handleRegisterResponse(const uint8_t *frame, size_t len) {
  if (!awaitingRegistration_) return;  // unsolicited: never accept an identity we didn't ask for
  RegisterResponse r;
  if (decode(frame, len, r) != Status::Ok) return;
  awaitingRegistration_ = false;
  if (!r.success || uuids_.save(gateway_, r.casterUUID) != Status::Ok) {
    // A UUID we could not persist would be forgotten on reboot, so it is not "registered".
    if (listener_) listener_->onRegistrationFailed();
    return;
  }
  memcpy(uuid_, r.casterUUID, MAGIC_FRAME_UUID_BYTES);
  registered_ = true;
  if (listener_) listener_->onRegistered(uuid_);
}

void Wand::handleCastResult(const uint8_t *frame, size_t len) {
  if (!pending_) return;
  CastResult r;
  if (decode(frame, len, r) != Status::Ok) return;
  pending_ = false;
  if (listener_) listener_->onResult(r.success);
}

Status Wand::registerWithGateway() {
  if (!id_.ready()) return Status::SignFailed;
  if (!havePeer_ || !clock_.synced()) return Status::NotSynced;
  uint64_t epoch;
  Status s = clock_.now(mono_(), &epoch);
  if (s != Status::Ok) return s;

  char pubHex[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  id_.publicKeyHex(pubHex);
  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  if (registrationMessage(msg, sizeof(msg), epoch, pubHex) == 0) return Status::BadArgument;

  RegisterRequest req;
  memcpy(req.pubKey, id_.publicKey(), SESSIONLESS_PUBLIC_KEY_SIZE);
  req.timestamp = epoch;
  if (id_.sign(msg, req.signature) != sessionless::Status::Ok) return Status::SignFailed;

  uint8_t frame[MAGIC_FRAME_MAX_SIZE];
  size_t n = encode(req, frame, sizeof(frame));
  if (n == 0) return Status::BadArgument;
  s = link_.send(&gateway_, frame, n);
  if (s != Status::Ok) return s;
  awaitingRegistration_ = true;
  registrationDeadline_ = mono_() + options_.registrationTimeoutMs;
  return Status::Ok;
}

Status Wand::cast(const SpellDef &spell) {
  if (!id_.ready()) return Status::SignFailed;
  if (pending_) return Status::Busy;
  if (!havePeer_ || !clock_.synced()) return Status::NotSynced;
  if (!registered_) return Status::NotRegistered;
  if (!spell.name || !isSafeId(spell.name, MAGIC_SPELL_NAME_MAX_CHARS)) return Status::BadArgument;

  uint64_t epoch;
  Status s = clock_.now(mono_(), &epoch);
  if (s != Status::Ok) return s;
  uint32_t ordinal;
  s = counter_.next(&ordinal);  // persisted before use; gaps are fine, repeats are not
  if (s != Status::Ok) return s;

  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  if (casterMessage(msg, sizeof(msg), epoch, spell.name, uuid_, spell.totalCost, spell.mp, ordinal) == 0) return Status::BadArgument;

  CastSpellFrame f;
  memset(&f, 0, sizeof(f));
  memcpy(f.casterUUID, uuid_, MAGIC_FRAME_UUID_BYTES);
  f.timestamp = epoch;
  f.ordinal = ordinal;
  if (id_.sign(msg, f.casterSignature) != sessionless::Status::Ok) return Status::SignFailed;

  uint8_t frame[MAGIC_FRAME_MAX_SIZE];
  size_t n = encode(f, frame, sizeof(frame));
  if (n == 0) return Status::BadArgument;
  s = link_.send(&gateway_, frame, n);
  if (s != Status::Ok) return s;
  pending_ = true;
  resultDeadline_ = mono_() + options_.resultTimeoutMs;
  return Status::Ok;
}

}  // namespace magic
