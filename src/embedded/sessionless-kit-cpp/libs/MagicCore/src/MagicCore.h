// MagicCore: the transport-independent part of MAGIC for wands and gateways.
// Include this one header. It needs no Arduino, WiFi, or radio, so everything in
// it runs and is tested on a laptop.
#ifndef MAGIC_CORE_H
#define MAGIC_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "SessionlessKit.h"

// ---------------------------------------------------------------------------
// Limits. Gateways can attach arbitrary data, but "arbitrary" needs a bound,
// and smaller is better. These are enforced when a spell body is built
// (writeSpellJson) and when a frame or extra is validated, not just documented.
//
// Why these numbers (measured): the caster's spell is ~309 bytes of JSON and each
// gateway's mandatory signature entry is ~256 bytes before any extra. 4 gateways
// with 128-byte extras come to 1880 bytes; 256-byte extras would not fit in 2048.
// 128 also fits in the 136 bytes left in a 250-byte ESP-NOW frame after a cast.
// Raise only with a reason: every receiver must be able to buffer MAGIC_MAX_SPELL_BODY_BYTES.
// ---------------------------------------------------------------------------
#define MAGIC_MAX_GATEWAY_EXTRA_BYTES 128
#define MAGIC_MAX_SPELL_BODY_BYTES 2048

// Identifier limits (spell names, UUIDs). Characters are restricted to
// [A-Za-z0-9._:-] so these strings can go into signed messages and JSON
// without escaping, and an untrusted radio frame can't inject JSON.
#define MAGIC_UUID_MAX_CHARS 36
#define MAGIC_SPELL_NAME_MAX_CHARS 63

// Largest message string any builder below can produce, including the NUL.
#define MAGIC_MESSAGE_BUFFER_SIZE 192

namespace magic {

enum class Status {
  Ok = 0,
  BadArgument,     // null, empty, too long, or disallowed characters
  BufferTooSmall,  // output buffer can't hold the result
  BadExtra,        // extra is not a compact-enough, valid JSON object (see checkExtra)
  ExtraTooLarge,   // extra exceeds MAGIC_MAX_GATEWAY_EXTRA_BYTES
  BodyTooLarge,    // spell body would exceed MAGIC_MAX_SPELL_BODY_BYTES
  BadFrame,        // wire frame has the wrong length, type, or contents
  NotSynced,       // clock has never been synced to real time
  ClockWentBack,   // monotonic time moved backwards
  NotFound,        // store has nothing saved yet
  StoreError,
  Overflow,
  SignFailed,
  ResolverUnreachable,  // network/transport failure talking to the resolver (retryable)
  ResolverRejected,     // the resolver answered and refused (e.g. bad registration)
  NotRegistered,        // no UUID for this peer/resolver yet
  Busy,                 // a cast is already waiting for its result
  Timeout,
  BadResponse,          // resolver reply was not valid JSON or lacked the expected field
  ClockSkew             // a timestamp is too far from our clock (replay window / unsynced peer)
};
const char *statusName(Status s);

// True if `s` is non-empty, <= maxChars, and only [A-Za-z0-9._:-].
bool isSafeId(const char *s, size_t maxChars);

// ---------------------------------------------------------------------------
// Signed message strings. Plain concatenation, exactly what fount's resolve()
// reconstructs. Each returns the length (excluding NUL), or 0 on any problem
// (null/invalid ids, buffer too small).
//   caster:       timestamp + spell + casterUUID + totalCost + mp + ordinal   (mp as "true"/"false")
//   gateway:      timestamp + uuid + minimumCost + ordinal
//   registration: timestamp + pubKeyHex
// Timestamps are epoch milliseconds, formatted as decimal.
// ---------------------------------------------------------------------------
size_t casterMessage(char *out, size_t cap, uint64_t timestamp, const char *spell, const char *casterUUID,
                     uint64_t totalCost, bool mp, uint64_t ordinal);
size_t gatewayMessage(char *out, size_t cap, uint64_t timestamp, const char *uuid, uint64_t minimumCost, uint64_t ordinal);
size_t registrationMessage(char *out, size_t cap, uint64_t timestamp, const char *pubKeyHex);

// Build the message and sign it with `id`. `sigHex` receives 128 hex chars + NUL.
Status signCaster(const sessionless::Identity &id, uint64_t timestamp, const char *spell, const char *casterUUID,
                  uint64_t totalCost, bool mp, uint64_t ordinal, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]);
Status signGateway(const sessionless::Identity &id, uint64_t timestamp, const char *uuid, uint64_t minimumCost,
                   uint64_t ordinal, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]);
Status signRegistration(const sessionless::Identity &id, uint64_t timestamp, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]);

// ---------------------------------------------------------------------------
// Spell body (the JSON POSTed to the resolver), written without heap or a JSON
// library, with the size limits enforced.
// ---------------------------------------------------------------------------
struct SpellFields {
  uint64_t timestamp;
  const char *spell;
  const char *casterUUID;
  uint64_t totalCost;
  bool mp;
  uint64_t ordinal;
  const char *casterSignatureHex;  // 128 hex chars
};

struct GatewayFields {
  uint64_t timestamp;
  const char *uuid;
  uint64_t minimumCost;
  uint64_t ordinal;
  const char *signatureHex;  // 128 hex chars
  const char *extraJson;     // optional; nullptr or "" for none. See checkExtra.
};

// A gateway's extra must be a valid JSON object, ASCII only, at most
// MAGIC_MAX_GATEWAY_EXTRA_BYTES bytes, nested at most 4 deep (bounded stack on MCUs).
// Use "\uXXXX" escapes for non-ASCII text. Returns Ok, BadExtra, or ExtraTooLarge.
Status checkExtra(const char *extraJson);

// Writes {"timestamp":"..","spell":..,..,"gateways":[{..,"extra":{..}},..]} and a NUL.
// Key order is fixed (see magic-vectors.json). `*len` (optional) gets the length without NUL.
// Fails with BodyTooLarge if the body would exceed MAGIC_MAX_SPELL_BODY_BYTES, even if `cap` is larger.
Status writeSpellJson(char *out, size_t cap, size_t *len, const SpellFields &spell, const GatewayFields *gateways,
                      size_t gatewayCount);

// ---------------------------------------------------------------------------
// Wire frames (ESP-NOW; 250-byte cap). Little-endian, packed, byte-identical to the
// structs in espnow-wand/src/espnow_protocol.h, but encoded explicitly so they don't
// depend on compiler struct layout. UUIDs travel as char[37] (NUL-terminated).
// decode*() are strict: exact length, right type byte, success flags 0 or 1,
// UUIDs NUL-terminated and made of safe characters. Anything else is BadFrame.
// ---------------------------------------------------------------------------
enum MsgType : uint8_t {
  MSG_REGISTER_REQUEST = 1,   // wand -> gateway
  MSG_REGISTER_RESPONSE = 2,  // gateway -> wand
  MSG_CAST_SPELL = 3,         // wand -> gateway
  MSG_CAST_RESULT = 4,        // gateway -> wand
  MSG_TIME_SYNC = 5           // gateway -> wand, periodic broadcast
};

#define MAGIC_FRAME_UUID_BYTES 37
#define MAGIC_FRAME_REGISTER_REQUEST_SIZE 106
#define MAGIC_FRAME_REGISTER_RESPONSE_SIZE 39
#define MAGIC_FRAME_CAST_SPELL_SIZE 114
#define MAGIC_FRAME_CAST_RESULT_SIZE 2
#define MAGIC_FRAME_TIME_SYNC_SIZE 9
#define MAGIC_FRAME_MAX_SIZE 114

// The type byte, or 0 if the frame is empty or the type is unknown.
uint8_t peekType(const uint8_t *frame, size_t len);

struct RegisterRequest {
  uint8_t pubKey[SESSIONLESS_PUBLIC_KEY_SIZE];
  uint64_t timestamp;
  uint8_t signature[SESSIONLESS_SIGNATURE_SIZE];  // signs registrationMessage()
};
struct RegisterResponse {
  bool success;
  char casterUUID[MAGIC_FRAME_UUID_BYTES];  // empty when !success
};
struct CastSpellFrame {
  char casterUUID[MAGIC_FRAME_UUID_BYTES];
  uint64_t timestamp;
  uint32_t ordinal;
  uint8_t casterSignature[SESSIONLESS_SIGNATURE_SIZE];
};
struct CastResult {
  bool success;
};
struct TimeSync {
  uint64_t epochMs;
};

// encode returns the frame length, or 0 if `cap` is too small or a field is invalid.
size_t encode(const RegisterRequest &f, uint8_t *out, size_t cap);
size_t encode(const RegisterResponse &f, uint8_t *out, size_t cap);
size_t encode(const CastSpellFrame &f, uint8_t *out, size_t cap);
size_t encode(const CastResult &f, uint8_t *out, size_t cap);
size_t encode(const TimeSync &f, uint8_t *out, size_t cap);

Status decode(const uint8_t *in, size_t len, RegisterRequest &f);
Status decode(const uint8_t *in, size_t len, RegisterResponse &f);
Status decode(const uint8_t *in, size_t len, CastSpellFrame &f);
Status decode(const uint8_t *in, size_t len, CastResult &f);
Status decode(const uint8_t *in, size_t len, TimeSync &f);

// ---------------------------------------------------------------------------
// Clock. fount rejects any signed timestamp more than 5 minutes from its own time,
// and a wand has no NTP, so it calibrates from the gateway's TimeSync frames.
// Pure arithmetic: you pass in a 64-bit monotonic millisecond reading. Use
// esp_timer_get_time()/1000, NOT Arduino millis(), which wraps at ~49.7 days.
// An unsynced clock refuses to answer (NotSynced) rather than guessing.
// ---------------------------------------------------------------------------
class EpochClock {
 public:
  void sync(uint64_t epochMs, uint64_t monotonicMs) {
    epochAtSync_ = epochMs;
    monotonicAtSync_ = monotonicMs;
    synced_ = true;
  }
  bool synced() const { return synced_; }
  Status now(uint64_t monotonicMs, uint64_t *epochMs) const {
    if (!synced_) return Status::NotSynced;
    if (monotonicMs < monotonicAtSync_) return Status::ClockWentBack;
    *epochMs = epochAtSync_ + (monotonicMs - monotonicAtSync_);
    return Status::Ok;
  }
 private:
  uint64_t epochAtSync_ = 0, monotonicAtSync_ = 0;
  bool synced_ = false;
};

// ---------------------------------------------------------------------------
// Ordinal: a persisted, strictly increasing counter, one per caster/gateway, so a
// spell can never be replayed with the same ordinal. next() saves BEFORE returning,
// so a reset can't hand out the same value twice; if saving fails you get StoreError
// and no value.
// ---------------------------------------------------------------------------
class OrdinalStore {
 public:
  virtual ~OrdinalStore() {}
  virtual Status load(uint32_t *value) = 0;  // Ok, NotFound (nothing saved yet), or StoreError
  virtual Status save(uint32_t value) = 0;
};

class MemoryOrdinalStore : public OrdinalStore {
 public:
  Status load(uint32_t *value) override {
    if (!has_) return Status::NotFound;
    *value = value_;
    return Status::Ok;
  }
  Status save(uint32_t value) override {
    value_ = value;
    has_ = true;
    return Status::Ok;
  }
 private:
  uint32_t value_ = 0;
  bool has_ = false;
};

class OrdinalCounter {
 public:
  explicit OrdinalCounter(OrdinalStore &store) : store_(store) {}
  Status next(uint32_t *ordinal);
 private:
  OrdinalStore &store_;
  uint32_t last_ = 0;
  bool loaded_ = false;
};

// ---------------------------------------------------------------------------
// Resolver response reading. Resolver replies are untrusted network input, so they are
// parsed with the same strict JSON grammar as `extra` (but with a deeper depth cap and
// UTF-8 passed through, since resolvers may echo arbitrary service data).
// The buffer must be NUL-terminated at json[len].
// ---------------------------------------------------------------------------
enum class JsonType { String, Bool, Number, Other };
struct JsonField {
  JsonType type;
  const char *start;  // String: first char after the opening quote. Others: first char of the token.
  size_t len;         // String: raw length before the closing quote (escapes NOT decoded). Others: token length.
  bool boolean;       // valid when type == Bool
};
// Ok and `*out` filled if `key` is a top-level member (the last one wins, as in JSON.parse).
// NotFound if the body is valid JSON but lacks it. BadResponse if the body is not a valid JSON
// object (or nests deeper than 8), so a garbled reply is never mistaken for "key absent".
Status jsonGetTopLevel(const char *json, size_t len, const char *key, JsonField *out);
// Copies a String field with no escapes into `out` (NUL-terminated). False if it has escapes or won't fit.
bool jsonCopyString(const JsonField &f, char *out, size_t cap);

// ---------------------------------------------------------------------------
// The two real interfaces. Everything above is transport- and resolver-independent;
// implementations plug in underneath.
// ---------------------------------------------------------------------------

// Opaque link-layer address of a peer (an ESP-NOW MAC, a BLE address, a LoRa node id...).
// len == 0 means "no specific peer" (broadcast when sending).
struct PeerId {
  uint8_t bytes[8];
  uint8_t len;
  PeerId() : bytes{0, 0, 0, 0, 0, 0, 0, 0}, len(0) {}
  PeerId(const uint8_t *b, uint8_t n) : bytes{0, 0, 0, 0, 0, 0, 0, 0}, len(n > 8 ? 8 : n) {
    for (uint8_t i = 0; i < len; i++) bytes[i] = b[i];
  }
  bool isNone() const { return len == 0; }
  bool operator==(const PeerId &o) const {
    if (len != o.len) return false;
    for (uint8_t i = 0; i < len; i++) if (bytes[i] != o.bytes[i]) return false;
    return true;
  }
  bool operator!=(const PeerId &o) const { return !(*this == o); }
};

// A frame pipe between a wand and a gateway: ESP-NOW, BLE, LoRa, serial, loopback...
// Frames are the MagicCore wire frames (at most MAGIC_FRAME_MAX_SIZE bytes).
// Implementations must be non-blocking, and must not assume frames are delivered:
// radios drop. Links are unauthenticated, so MagicCore never trusts a frame beyond its own checks.
class Transport {
 public:
  virtual ~Transport() {}
  // Largest frame this link can carry. Must be >= MAGIC_FRAME_MAX_SIZE for MAGIC to run on it.
  virtual size_t maxFrameSize() const = 0;
  // Send one frame to `peer`, or to everyone in range if `peer` is null. Ok means "handed to the
  // link", NOT "delivered" (broadcast radios have no ACK). Errors: BadArgument, BufferTooSmall
  // (frame too big for the link), or StoreError-free link failures reported as Timeout.
  virtual Status send(const PeerId *peer, const uint8_t *frame, size_t len) = 0;
  // If a frame is waiting, copy it into `buf` (up to `cap`), set `*len` and `*from`, and return true.
  // Never blocks. A frame larger than `cap` must be discarded, not truncated.
  virtual bool receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) = 0;
};

// What a resolver is asked to resolve. `body` is the exact JSON written (and limit-checked) by
// writeSpellJson; the structured fields are the same data for resolvers that don't speak JSON.
struct ResolveRequest {
  const char *spell;
  const char *body;
  size_t bodyLen;
  const SpellFields *spellFields;
  const GatewayFields *gateways;
  size_t gatewayCount;
};
struct ResolveResult {
  bool success;  // the resolver processed the spell and said yes
};

// The party that registers casters/gateways and resolves spells (fount, or anything speaking
// the same contract). Implementations own TLS: a networked resolver must verify the server
// certificate by default and only skip that through an explicit, loudly named opt-out.
class Resolver {
 public:
  virtual ~Resolver() {}
  // Register a public key (message signed = registrationMessage). Idempotent for a known key.
  // Ok + uuid filled; ResolverRejected if refused; ResolverUnreachable on network failure;
  // BadResponse if the reply had no usable uuid.
  virtual Status registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                              char uuidOut[MAGIC_FRAME_UUID_BYTES]) = 0;
  // Ok with `out->success` telling whether the spell resolved. A resolver saying "no" is Ok +
  // success=false; only failing to get an answer is an error (ResolverUnreachable/BadResponse).
  virtual Status resolve(const ResolveRequest &request, ResolveResult *out) = 0;
};

// URL policy shared by every networked Resolver: must be http:// or https://, no userinfo
// (http://localhost@evil.example), and plain http:// only for loopback hosts (localhost, 127.0.0.1,
// [::1]) unless `allowPlainHttp`. Ok, or BadArgument. `*isHttps` (optional) reports the scheme.
Status checkResolverUrl(const char *baseUrl, bool allowPlainHttp, bool *isHttps = nullptr);

// How a networked Resolver treats the server certificate. Verify is the default everywhere.
// The opt-out is deliberately long and ugly: it is for talking to a dev server with a self-signed
// certificate, never for a device that ships.
enum class TlsVerification { Verify, InsecureForDevelopment };

// The fount HTTP contract, written once so every networked Resolver interprets replies identically
// (and so it is tested on a laptop, whatever the HTTP stack is on the device):
//   register: PUT {base}/user/create   {"timestamp":"..","pubKey":"..","signature":".."}  -> 200 + {"uuid":..}
//   resolve:  POST {base}/resolve/<spell>  <spell body>  -> 200 + {"success":true} on success. fount says "no"
//             with HTTP 900 + {"success":false} (a non-standard status), or HTTP 200 + {"error":..} (e.g. stale time).
// parse* take the HTTP status you got and the NUL-terminated body. 5xx is ResolverUnreachable (server
// trouble, retryable, not an answer). Anything not valid JSON where JSON is required is BadResponse.
Status buildRegisterBody(char *out, size_t cap, size_t *len, uint64_t timestamp, const char *pubKeyHex, const char *signatureHex);
Status parseRegisterResponse(int httpStatus, const char *body, size_t len, char uuidOut[MAGIC_FRAME_UUID_BYTES]);
Status parseResolveResponse(int httpStatus, const char *body, size_t len, ResolveResult *out);

// Monotonic milliseconds since boot, 64-bit (ESP32: esp_timer_get_time()/1000).
typedef uint64_t (*MonotonicFn)();

// What a spell costs and whether it spends MP. Caster and gateway must agree (the wire frame
// in v1 does not carry the spell, as in the original demos: one spell per wand/gateway pairing).
struct SpellDef {
  const char *name;
  uint64_t totalCost;
  bool mp;
};

// Persists the UUID a resolver assigned. `scope` says whose: a wand stores one per gateway it
// has registered through; a gateway stores its own under an empty PeerId.
class UuidStore {
 public:
  virtual ~UuidStore() {}
  virtual Status load(const PeerId &scope, char uuid[MAGIC_FRAME_UUID_BYTES]) = 0;  // Ok or NotFound
  virtual Status save(const PeerId &scope, const char *uuid) = 0;
};

class MemoryUuidStore : public UuidStore {
 public:
  Status load(const PeerId &scope, char uuid[MAGIC_FRAME_UUID_BYTES]) override;
  Status save(const PeerId &scope, const char *uuid) override;
 private:
  static const int kSlots = 4;
  PeerId scope_[kSlots];
  char uuid_[kSlots][MAGIC_FRAME_UUID_BYTES] = {{0}};
  bool used_[kSlots] = {false};
};

// A real in-memory Transport pair for tests, simulators and single-process setups: whatever one
// end sends, the other receives (a broadcast and a unicast both arrive at the only other
// endpoint). Each end holds up to 8 frames and drops the newest when full, like a busy radio.
class LoopbackLink {
 public:
  Transport &a() { return a_; }
  Transport &b() { return b_; }
  static PeerId idOfA() { static const uint8_t id[] = {0xA0}; return PeerId(id, 1); }
  static PeerId idOfB() { static const uint8_t id[] = {0xB0}; return PeerId(id, 1); }
  unsigned dropped() const { return a_.dropped_ + b_.dropped_; }
 private:
  class End : public Transport {
   public:
    End(LoopbackLink *link, bool isA) : link_(link), isA_(isA) {}
    size_t maxFrameSize() const override { return 250; }
    Status send(const PeerId *peer, const uint8_t *frame, size_t len) override;
    bool receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) override;
    static const int kQueue = 8;
    uint8_t frames_[kQueue][MAGIC_FRAME_MAX_SIZE];
    size_t lens_[kQueue] = {0};
    PeerId from_[kQueue];
    int head_ = 0, count_ = 0;
    unsigned dropped_ = 0;
   private:
    LoopbackLink *link_;
    bool isA_;
  };
  End a_{this, true};
  End b_{this, false};
};

}  // namespace magic

#endif
