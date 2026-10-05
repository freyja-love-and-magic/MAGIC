#include "MagicCore.h"

#include <string.h>

namespace magic {

const char *statusName(Status s) {
  switch (s) {
    case Status::Ok: return "Ok";
    case Status::BadArgument: return "BadArgument";
    case Status::BufferTooSmall: return "BufferTooSmall";
    case Status::BadExtra: return "BadExtra";
    case Status::ExtraTooLarge: return "ExtraTooLarge";
    case Status::BodyTooLarge: return "BodyTooLarge";
    case Status::BadFrame: return "BadFrame";
    case Status::NotSynced: return "NotSynced";
    case Status::ClockWentBack: return "ClockWentBack";
    case Status::NotFound: return "NotFound";
    case Status::StoreError: return "StoreError";
    case Status::Overflow: return "Overflow";
    case Status::SignFailed: return "SignFailed";
    case Status::ResolverUnreachable: return "ResolverUnreachable";
    case Status::ResolverRejected: return "ResolverRejected";
    case Status::NotRegistered: return "NotRegistered";
    case Status::Busy: return "Busy";
    case Status::Timeout: return "Timeout";
    case Status::BadResponse: return "BadResponse";
    case Status::ClockSkew: return "ClockSkew";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// validation helpers

static size_t boundedLen(const char *s, size_t max) {  // strlen, but never reads past max+1 bytes
  size_t n = 0;
  while (n <= max && s[n]) n++;
  return n;
}

bool isSafeId(const char *s, size_t maxChars) {
  if (!s) return false;
  size_t n = boundedLen(s, maxChars);
  if (n == 0 || n > maxChars) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
    if (!ok) return false;
  }
  return true;
}

static bool isLowerHex(const char *s, size_t len) {  // exactly `len` chars, lowercase hex
  if (!s || boundedLen(s, len) != len) return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// output builder: counts everything, writes only what fits

namespace {
struct Out {
  char *buf;
  size_t cap;
  size_t n = 0;
  Out(char *b, size_t c) : buf(b), cap(c) {}
  void put(char c) {
    if (buf && n + 1 < cap) buf[n] = c;
    n++;
  }
  void str(const char *s) { while (*s) put(*s++); }
  void u64(uint64_t v) {
    char tmp[20];
    int k = 0;
    do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) put(tmp[--k]);
  }
  bool fits() const { return n + 1 <= cap; }
  void terminate() { if (buf && cap) buf[n < cap ? n : cap - 1] = '\0'; }
};
}  // namespace

// ---------------------------------------------------------------------------
// message strings

size_t casterMessage(char *out, size_t cap, uint64_t timestamp, const char *spell, const char *casterUUID,
                     uint64_t totalCost, bool mp, uint64_t ordinal) {
  if (!out || !isSafeId(spell, MAGIC_SPELL_NAME_MAX_CHARS) || !isSafeId(casterUUID, MAGIC_UUID_MAX_CHARS)) return 0;
  Out o(out, cap);
  o.u64(timestamp); o.str(spell); o.str(casterUUID); o.u64(totalCost); o.str(mp ? "true" : "false"); o.u64(ordinal);
  if (!o.fits()) return 0;
  o.terminate();
  return o.n;
}

size_t gatewayMessage(char *out, size_t cap, uint64_t timestamp, const char *uuid, uint64_t minimumCost, uint64_t ordinal) {
  if (!out || !isSafeId(uuid, MAGIC_UUID_MAX_CHARS)) return 0;
  Out o(out, cap);
  o.u64(timestamp); o.str(uuid); o.u64(minimumCost); o.u64(ordinal);
  if (!o.fits()) return 0;
  o.terminate();
  return o.n;
}

size_t registrationMessage(char *out, size_t cap, uint64_t timestamp, const char *pubKeyHex) {
  if (!out || !isLowerHex(pubKeyHex, SESSIONLESS_PUBLIC_KEY_SIZE * 2)) return 0;
  Out o(out, cap);
  o.u64(timestamp); o.str(pubKeyHex);
  if (!o.fits()) return 0;
  o.terminate();
  return o.n;
}

static Status signMessage(const sessionless::Identity &id, size_t len, const char *msg, char *sigHex) {
  if (len == 0) return Status::BadArgument;
  if (!id.ready() || id.signHex(msg, sigHex) != sessionless::Status::Ok) return Status::SignFailed;
  return Status::Ok;
}

Status signCaster(const sessionless::Identity &id, uint64_t timestamp, const char *spell, const char *casterUUID,
                  uint64_t totalCost, bool mp, uint64_t ordinal, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]) {
  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  return signMessage(id, casterMessage(msg, sizeof(msg), timestamp, spell, casterUUID, totalCost, mp, ordinal), msg, sigHex);
}

Status signGateway(const sessionless::Identity &id, uint64_t timestamp, const char *uuid, uint64_t minimumCost,
                   uint64_t ordinal, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]) {
  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  return signMessage(id, gatewayMessage(msg, sizeof(msg), timestamp, uuid, minimumCost, ordinal), msg, sigHex);
}

Status signRegistration(const sessionless::Identity &id, uint64_t timestamp, char sigHex[SESSIONLESS_SIGNATURE_HEX_LEN]) {
  if (!id.ready()) return Status::SignFailed;
  char pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  id.publicKeyHex(pub);
  char msg[MAGIC_MESSAGE_BUFFER_SIZE];
  return signMessage(id, registrationMessage(msg, sizeof(msg), timestamp, pub), msg, sigHex);
}

// ---------------------------------------------------------------------------
// extra: a small strict JSON validator (RFC 8259 grammar), ASCII only, depth <= 4

namespace {
struct Json {
  const char *p;
  int maxDepth;    // deepest allowed nesting (the top-level object is depth 1)
  bool asciiOnly;  // reject bytes >= 0x7F inside strings (extra); resolver replies pass UTF-8 through

  void ws() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

  bool string() {  // at opening quote
    p++;
    while (true) {
      unsigned char c = (unsigned char)*p;
      if (c == '"') { p++; return true; }
      if (c < 0x20 || c == 0x7F || (asciiOnly && c > 0x7F)) return false;  // control, NUL (unterminated), or non-ASCII when asciiOnly
      if (c == '\\') {
        p++;
        char e = *p;
        if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') { p++; continue; }
        if (e != 'u') return false;
        p++;
        for (int i = 0; i < 4; i++, p++) {
          char h = *p;
          if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return false;
        }
        continue;
      }
      p++;
    }
  }

  bool number() {
    if (*p == '-') p++;
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') { while (*p >= '0' && *p <= '9') p++; }
    else return false;
    if (*p == '.') {
      p++;
      if (!(*p >= '0' && *p <= '9')) return false;
      while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'e' || *p == 'E') {
      p++;
      if (*p == '+' || *p == '-') p++;
      if (!(*p >= '0' && *p <= '9')) return false;
      while (*p >= '0' && *p <= '9') p++;
    }
    return true;
  }

  bool literal(const char *word) {
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) return false;
    p += n;
    return true;
  }

  bool value(int depth) {
    ws();
    switch (*p) {
      case '{': return object(depth + 1);
      case '[': return array(depth + 1);
      case '"': return string();
      case 't': return literal("true");
      case 'f': return literal("false");
      case 'n': return literal("null");
      default: return number();
    }
  }

  bool object(int depth) {
    if (depth > maxDepth) return false;
    p++;
    ws();
    if (*p == '}') { p++; return true; }
    while (true) {
      ws();
      if (*p != '"' || !string()) return false;
      ws();
      if (*p != ':') return false;
      p++;
      if (!value(depth)) return false;
      ws();
      if (*p == ',') { p++; continue; }
      if (*p == '}') { p++; return true; }
      return false;
    }
  }

  bool array(int depth) {
    if (depth > maxDepth) return false;
    p++;
    ws();
    if (*p == ']') { p++; return true; }
    while (true) {
      if (!value(depth)) return false;
      ws();
      if (*p == ',') { p++; continue; }
      if (*p == ']') { p++; return true; }
      return false;
    }
  }
};
}  // namespace

Status checkExtra(const char *extraJson) {
  if (!extraJson) return Status::BadExtra;
  size_t n = boundedLen(extraJson, MAGIC_MAX_GATEWAY_EXTRA_BYTES);
  if (n > MAGIC_MAX_GATEWAY_EXTRA_BYTES) return Status::ExtraTooLarge;
  Json j{extraJson, 4, true};
  j.ws();
  if (*j.p != '{') return Status::BadExtra;  // must be an object
  if (!j.object(1)) return Status::BadExtra;
  j.ws();
  return *j.p == '\0' ? Status::Ok : Status::BadExtra;
}

Status jsonGetTopLevel(const char *json, size_t len, const char *key, JsonField *out) {
  if (!json || !key || !out) return Status::BadArgument;
  if (boundedLen(json, len) != len) return Status::BadArgument;  // not NUL-terminated at json[len]
  Json j{json, 8, false};
  j.ws();
  if (*j.p != '{') return Status::BadResponse;
  j.p++;
  j.ws();
  const size_t keyLen = strlen(key);
  bool found = false;
  JsonField best = {JsonType::Other, nullptr, 0, false};
  if (*j.p == '}') {
    j.p++;
  } else {
    while (true) {
      j.ws();
      if (*j.p != '"') return Status::BadResponse;
      const char *keyStart = j.p + 1;
      if (!j.string()) return Status::BadResponse;
      size_t thisKeyLen = (size_t)(j.p - 1 - keyStart);
      j.ws();
      if (*j.p != ':') return Status::BadResponse;
      j.p++;
      j.ws();
      const char *valueStart = j.p;
      if (!j.value(1)) return Status::BadResponse;
      size_t valueLen = (size_t)(j.p - valueStart);
      if (thisKeyLen == keyLen && memcmp(keyStart, key, keyLen) == 0) {
        found = true;
        char c0 = *valueStart;
        if (c0 == '"') best = {JsonType::String, valueStart + 1, valueLen - 2, false};
        else if (c0 == 't' || c0 == 'f') best = {JsonType::Bool, valueStart, valueLen, c0 == 't'};
        else if (c0 == '-' || (c0 >= '0' && c0 <= '9')) best = {JsonType::Number, valueStart, valueLen, false};
        else best = {JsonType::Other, valueStart, valueLen, false};
      }
      j.ws();
      if (*j.p == ',') { j.p++; continue; }
      if (*j.p == '}') { j.p++; break; }
      return Status::BadResponse;
    }
  }
  j.ws();
  if (*j.p != '\0') return Status::BadResponse;
  if (!found) return Status::NotFound;
  *out = best;
  return Status::Ok;
}

bool jsonCopyString(const JsonField &f, char *out, size_t cap) {
  if (f.type != JsonType::String || !out || f.len + 1 > cap) return false;
  for (size_t i = 0; i < f.len; i++) if (f.start[i] == '\\') return false;
  memcpy(out, f.start, f.len);
  out[f.len] = '\0';
  return true;
}

// ---------------------------------------------------------------------------
// spell body

Status writeSpellJson(char *out, size_t cap, size_t *len, const SpellFields &s, const GatewayFields *gws, size_t gwCount) {
  if (!out || cap == 0) return Status::BadArgument;
  if (!isSafeId(s.spell, MAGIC_SPELL_NAME_MAX_CHARS) || !isSafeId(s.casterUUID, MAGIC_UUID_MAX_CHARS) ||
      !isLowerHex(s.casterSignatureHex, SESSIONLESS_SIGNATURE_SIZE * 2) || (gwCount && !gws)) {
    return Status::BadArgument;
  }
  for (size_t i = 0; i < gwCount; i++) {
    if (!isSafeId(gws[i].uuid, MAGIC_UUID_MAX_CHARS) || !isLowerHex(gws[i].signatureHex, SESSIONLESS_SIGNATURE_SIZE * 2)) {
      return Status::BadArgument;
    }
    if (gws[i].extraJson && gws[i].extraJson[0]) {
      Status e = checkExtra(gws[i].extraJson);
      if (e != Status::Ok) return e;
    }
  }

  Out o(out, cap);
  o.str("{\"timestamp\":\""); o.u64(s.timestamp);
  o.str("\",\"spell\":\""); o.str(s.spell);
  o.str("\",\"casterUUID\":\""); o.str(s.casterUUID);
  o.str("\",\"totalCost\":"); o.u64(s.totalCost);
  o.str(",\"mp\":"); o.str(s.mp ? "true" : "false");
  o.str(",\"ordinal\":"); o.u64(s.ordinal);
  o.str(",\"casterSignature\":\""); o.str(s.casterSignatureHex);
  o.str("\",\"gateways\":[");
  for (size_t i = 0; i < gwCount; i++) {
    if (i) o.put(',');
    o.str("{\"timestamp\":\""); o.u64(gws[i].timestamp);
    o.str("\",\"uuid\":\""); o.str(gws[i].uuid);
    o.str("\",\"minimumCost\":"); o.u64(gws[i].minimumCost);
    o.str(",\"ordinal\":"); o.u64(gws[i].ordinal);
    o.str(",\"signature\":\""); o.str(gws[i].signatureHex);
    o.put('"');
    if (gws[i].extraJson && gws[i].extraJson[0]) { o.str(",\"extra\":"); o.str(gws[i].extraJson); }
    o.put('}');
  }
  o.str("]}");

  if (o.n > MAGIC_MAX_SPELL_BODY_BYTES) return Status::BodyTooLarge;
  if (!o.fits()) return Status::BufferTooSmall;
  o.terminate();
  if (len) *len = o.n;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// frames

namespace {
void putU64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint64_t getU64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
uint32_t getU32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }

// A frame UUID field: NUL within 37 bytes, then 1..36 safe chars before it.
bool frameUuidValid(const char *u) {
  size_t n = 0;
  while (n < MAGIC_FRAME_UUID_BYTES && u[n]) n++;
  if (n == MAGIC_FRAME_UUID_BYTES) return false;  // no terminator
  return isSafeId(u, MAGIC_UUID_MAX_CHARS) && n <= MAGIC_UUID_MAX_CHARS;
}

void putUuid(uint8_t *p, const char *uuid) {
  memset(p, 0, MAGIC_FRAME_UUID_BYTES);
  memcpy(p, uuid, strlen(uuid));
}
}  // namespace

uint8_t peekType(const uint8_t *frame, size_t len) {
  if (!frame || len == 0 || frame[0] < MSG_REGISTER_REQUEST || frame[0] > MSG_TIME_SYNC) return 0;
  return frame[0];
}

size_t encode(const RegisterRequest &f, uint8_t *out, size_t cap) {
  if (!out || cap < MAGIC_FRAME_REGISTER_REQUEST_SIZE) return 0;
  out[0] = MSG_REGISTER_REQUEST;
  memcpy(out + 1, f.pubKey, 33);
  putU64(out + 34, f.timestamp);
  memcpy(out + 42, f.signature, 64);
  return MAGIC_FRAME_REGISTER_REQUEST_SIZE;
}

size_t encode(const RegisterResponse &f, uint8_t *out, size_t cap) {
  if (!out || cap < MAGIC_FRAME_REGISTER_RESPONSE_SIZE) return 0;
  if (f.success && !frameUuidValid(f.casterUUID)) return 0;
  out[0] = MSG_REGISTER_RESPONSE;
  out[1] = f.success ? 1 : 0;
  if (f.success) putUuid(out + 2, f.casterUUID);
  else memset(out + 2, 0, MAGIC_FRAME_UUID_BYTES);
  return MAGIC_FRAME_REGISTER_RESPONSE_SIZE;
}

size_t encode(const CastSpellFrame &f, uint8_t *out, size_t cap) {
  if (!out || cap < MAGIC_FRAME_CAST_SPELL_SIZE || !frameUuidValid(f.casterUUID)) return 0;
  out[0] = MSG_CAST_SPELL;
  putUuid(out + 1, f.casterUUID);
  putU64(out + 38, f.timestamp);
  putU32(out + 46, f.ordinal);
  memcpy(out + 50, f.casterSignature, 64);
  return MAGIC_FRAME_CAST_SPELL_SIZE;
}

size_t encode(const CastResult &f, uint8_t *out, size_t cap) {
  if (!out || cap < MAGIC_FRAME_CAST_RESULT_SIZE) return 0;
  out[0] = MSG_CAST_RESULT;
  out[1] = f.success ? 1 : 0;
  return MAGIC_FRAME_CAST_RESULT_SIZE;
}

size_t encode(const TimeSync &f, uint8_t *out, size_t cap) {
  if (!out || cap < MAGIC_FRAME_TIME_SYNC_SIZE) return 0;
  out[0] = MSG_TIME_SYNC;
  putU64(out + 1, f.epochMs);
  return MAGIC_FRAME_TIME_SYNC_SIZE;
}

static bool frameShapeOk(const uint8_t *in, size_t len, uint8_t type, size_t size) {
  return in && len == size && in[0] == type;
}

Status decode(const uint8_t *in, size_t len, RegisterRequest &f) {
  if (!frameShapeOk(in, len, MSG_REGISTER_REQUEST, MAGIC_FRAME_REGISTER_REQUEST_SIZE)) return Status::BadFrame;
  memcpy(f.pubKey, in + 1, 33);
  f.timestamp = getU64(in + 34);
  memcpy(f.signature, in + 42, 64);
  return Status::Ok;
}

Status decode(const uint8_t *in, size_t len, RegisterResponse &f) {
  if (!frameShapeOk(in, len, MSG_REGISTER_RESPONSE, MAGIC_FRAME_REGISTER_RESPONSE_SIZE)) return Status::BadFrame;
  if (in[1] > 1) return Status::BadFrame;
  f.success = in[1] == 1;
  memcpy(f.casterUUID, in + 2, MAGIC_FRAME_UUID_BYTES);
  if (f.success) {
    if (!frameUuidValid(f.casterUUID)) { memset(f.casterUUID, 0, sizeof(f.casterUUID)); return Status::BadFrame; }
  } else {
    memset(f.casterUUID, 0, sizeof(f.casterUUID));
  }
  return Status::Ok;
}

Status decode(const uint8_t *in, size_t len, CastSpellFrame &f) {
  if (!frameShapeOk(in, len, MSG_CAST_SPELL, MAGIC_FRAME_CAST_SPELL_SIZE)) return Status::BadFrame;
  memcpy(f.casterUUID, in + 1, MAGIC_FRAME_UUID_BYTES);
  if (!frameUuidValid(f.casterUUID)) { memset(f.casterUUID, 0, sizeof(f.casterUUID)); return Status::BadFrame; }
  f.timestamp = getU64(in + 38);
  f.ordinal = getU32(in + 46);
  memcpy(f.casterSignature, in + 50, 64);
  return Status::Ok;
}

Status decode(const uint8_t *in, size_t len, CastResult &f) {
  if (!frameShapeOk(in, len, MSG_CAST_RESULT, MAGIC_FRAME_CAST_RESULT_SIZE) || in[1] > 1) return Status::BadFrame;
  f.success = in[1] == 1;
  return Status::Ok;
}

Status decode(const uint8_t *in, size_t len, TimeSync &f) {
  if (!frameShapeOk(in, len, MSG_TIME_SYNC, MAGIC_FRAME_TIME_SYNC_SIZE)) return Status::BadFrame;
  f.epochMs = getU64(in + 1);
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// ordinal

Status OrdinalCounter::next(uint32_t *ordinal) {
  if (!ordinal) return Status::BadArgument;
  if (!loaded_) {
    uint32_t stored = 0;
    Status s = store_.load(&stored);
    if (s == Status::NotFound) stored = 0;
    else if (s != Status::Ok) return s;
    last_ = stored;
    loaded_ = true;
  }
  if (last_ == 0xFFFFFFFFu) return Status::Overflow;
  Status s = store_.save(last_ + 1);
  if (s != Status::Ok) return Status::StoreError;
  last_++;
  *ordinal = last_;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// resolver URL policy

Status checkResolverUrl(const char *baseUrl, bool allowPlainHttp, bool *isHttps) {
  if (!baseUrl) return Status::BadArgument;
  size_t total = boundedLen(baseUrl, 2048);
  if (total > 2048) return Status::BadArgument;
  bool https;
  size_t start;
  if (strncmp(baseUrl, "https://", 8) == 0) { https = true; start = 8; }
  else if (strncmp(baseUrl, "http://", 7) == 0) { https = false; start = 7; }
  else return Status::BadArgument;
  size_t end = start;
  while (end < total && baseUrl[end] != '/' && baseUrl[end] != '?' && baseUrl[end] != '#') end++;
  size_t authLen = end - start;
  if (authLen == 0) return Status::BadArgument;
  const char *auth = baseUrl + start;
  for (size_t i = 0; i < authLen; i++) if (auth[i] == '@') return Status::BadArgument;

  size_t hostLen = authLen;
  if (auth[0] == '[') {
    size_t close = 0;
    while (close < authLen && auth[close] != ']') close++;
    if (close == authLen) return Status::BadArgument;
    hostLen = close + 1;
  } else {
    for (size_t i = 0; i < authLen; i++) if (auth[i] == ':') { hostLen = i; break; }
  }
  if (hostLen == 0) return Status::BadArgument;
  auto is = [&](const char *lit) { return strlen(lit) == hostLen && memcmp(auth, lit, hostLen) == 0; };
  bool loopback = is("localhost") || is("127.0.0.1") || is("[::1]");
  if (!https && !loopback && !allowPlainHttp) return Status::BadArgument;
  if (isHttps) *isHttps = https;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// the fount HTTP contract

Status buildRegisterBody(char *out, size_t cap, size_t *len, uint64_t timestamp, const char *pubKeyHex, const char *signatureHex) {
  if (!out || !isLowerHex(pubKeyHex, SESSIONLESS_PUBLIC_KEY_SIZE * 2) || !isLowerHex(signatureHex, SESSIONLESS_SIGNATURE_SIZE * 2)) {
    return Status::BadArgument;  // only hex goes into this body; anything else could break the JSON
  }
  Out o(out, cap);
  o.str("{\"timestamp\":\""); o.u64(timestamp);
  o.str("\",\"pubKey\":\""); o.str(pubKeyHex);
  o.str("\",\"signature\":\""); o.str(signatureHex);
  o.str("\"}");
  if (!o.fits()) return Status::BufferTooSmall;
  o.terminate();
  if (len) *len = o.n;
  return Status::Ok;
}

Status parseRegisterResponse(int httpStatus, const char *body, size_t len, char uuidOut[MAGIC_FRAME_UUID_BYTES]) {
  if (httpStatus >= 500 && httpStatus <= 599) return Status::ResolverUnreachable;
  if (httpStatus != 200) return Status::ResolverRejected;  // 403 auth error, 404, ...
  JsonField f;
  if (jsonGetTopLevel(body, len, "uuid", &f) != Status::Ok) return Status::BadResponse;
  char uuid[MAGIC_FRAME_UUID_BYTES + 8];
  if (!jsonCopyString(f, uuid, sizeof(uuid)) || !isSafeId(uuid, MAGIC_UUID_MAX_CHARS)) return Status::BadResponse;
  memcpy(uuidOut, uuid, strlen(uuid) + 1);
  return Status::Ok;
}

Status parseResolveResponse(int httpStatus, const char *body, size_t len, ResolveResult *out) {
  out->success = false;
  if (httpStatus >= 500 && httpStatus <= 599) return Status::ResolverUnreachable;
  JsonField f;
  Status s = jsonGetTopLevel(body, len, "success", &f);
  if (s == Status::NotFound) return Status::Ok;  // valid JSON without "success" (e.g. {"error":"no time like the present"}): answered, and the answer is no
  if (s != Status::Ok) return Status::BadResponse;
  // success means exactly: HTTP 200 and `success` is the JSON boolean true (not "true", not 1).
  out->success = httpStatus == 200 && f.type == JsonType::Bool && f.boolean;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// MemoryUuidStore

Status MemoryUuidStore::load(const PeerId &scope, char uuid[MAGIC_FRAME_UUID_BYTES]) {
  for (int i = 0; i < kSlots; i++) {
    if (used_[i] && scope_[i] == scope) {
      memcpy(uuid, uuid_[i], MAGIC_FRAME_UUID_BYTES);
      return Status::Ok;
    }
  }
  return Status::NotFound;
}

Status MemoryUuidStore::save(const PeerId &scope, const char *uuid) {
  if (!isSafeId(uuid, MAGIC_UUID_MAX_CHARS)) return Status::BadArgument;
  int slot = -1;
  for (int i = 0; i < kSlots; i++) {
    if (used_[i] && scope_[i] == scope) { slot = i; break; }
    if (!used_[i] && slot < 0) slot = i;
  }
  if (slot < 0) return Status::StoreError;  // full: refuse rather than evict someone's identity
  scope_[slot] = scope;
  memset(uuid_[slot], 0, MAGIC_FRAME_UUID_BYTES);
  memcpy(uuid_[slot], uuid, strlen(uuid));
  used_[slot] = true;
  return Status::Ok;
}

// ---------------------------------------------------------------------------
// LoopbackLink

Status LoopbackLink::End::send(const PeerId *, const uint8_t *frame, size_t len) {
  if (!frame || len == 0) return Status::BadArgument;
  if (len > MAGIC_FRAME_MAX_SIZE) return Status::BufferTooSmall;
  End &to = isA_ ? link_->b_ : link_->a_;
  if (to.count_ == kQueue) {
    to.dropped_++;  // like a busy radio: the sender can't tell
    return Status::Ok;
  }
  int slot = (to.head_ + to.count_) % kQueue;
  memcpy(to.frames_[slot], frame, len);
  to.lens_[slot] = len;
  to.from_[slot] = isA_ ? idOfA() : idOfB();
  to.count_++;
  return Status::Ok;
}

bool LoopbackLink::End::receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) {
  while (count_ > 0) {
    int slot = head_;
    head_ = (head_ + 1) % kQueue;
    count_--;
    if (lens_[slot] > cap) continue;  // too big for the caller's buffer: discard, never truncate
    memcpy(buf, frames_[slot], lens_[slot]);
    *len = lens_[slot];
    if (from) *from = from_[slot];
    return true;
  }
  return false;
}

}  // namespace magic
