// Host tests for MagicCore: golden vectors from JS (magic-vectors.json) plus strict limit/negative cases.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "MagicCore.h"

using namespace magic;
typedef std::map<std::string, std::string> Obj;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STATUS(expr, want) do { Status g_ = (expr); checks++; if (g_ != (want)) { failures++; printf("FAIL %s:%d  %s => %s, wanted %s\n", __FILE__, __LINE__, #expr, statusName(g_), statusName(want)); } } while (0)

// Flat JSON objects whose values are all strings (what magic-vectors.json contains).
static std::string readString(const std::string &t, size_t &i) {  // t[i] == '"'
  std::string out;
  for (i++; t[i] != '"'; i++) {
    if (t[i] != '\\') { out += t[i]; continue; }
    char c = t[++i];
    switch (c) {
      case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
      case 'b': out += '\b'; break; case 'f': out += '\f'; break;
      case 'u': { unsigned cp = (unsigned)strtoul(t.substr(i + 1, 4).c_str(), nullptr, 16); i += 4;
        if (cp < 0x80) out += (char)cp; else { printf("test parser: non-ASCII \\u not supported\n"); exit(2); } break; }
      default: out += c;
    }
  }
  i++;
  return out;
}

static std::vector<Obj> loadVectors() {
  std::ifstream in(MAGIC_VECTORS_PATH);
  if (!in) { printf("cannot open %s (run from test/host)\n", MAGIC_VECTORS_PATH); exit(2); }
  std::stringstream ss; ss << in.rdbuf();
  std::string t = ss.str();
  std::vector<Obj> out;
  size_t i = t.find("\"vectors\"");
  i = t.find('[', i);
  for (i++; i < t.size();) {
    while (i < t.size() && t[i] != '{' && t[i] != ']') i++;
    if (t[i] != '{') break;
    Obj o;
    for (i++; t[i] != '}';) {
      if (t[i] == '"') {
        std::string k = readString(t, i);
        while (t[i] != '"') i++;
        o[k] = readString(t, i);
      } else i++;
    }
    i++;
    out.push_back(o);
  }
  return out;
}

static uint64_t u64(const std::string &s) { return strtoull(s.c_str(), nullptr, 10); }
static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> b(h.size() / 2);
  CHECK(sessionless::fromHex(h.c_str(), b.data(), b.size()));
  return b;
}
static sessionless::Identity &importKey(const std::string &priv, sessionless::Identity &id) {
  sessionless::MemoryKeyStore store;
  CHECK(id.importPrivateKeyHex(store, priv.c_str()) == sessionless::Status::Ok);
  return id;
}

static const std::string HEX128(128, 'a');
static const char *UUID = "123e4567-e89b-42d3-a456-426614174000";

static void testGoldenMessages(const std::vector<Obj> &vs) {
  int n = 0;
  for (const Obj &v : vs) {
    const std::string &kind = v.at("kind");
    if (kind != "caster" && kind != "gateway" && kind != "registration") continue;
    n++;
    char msg[MAGIC_MESSAGE_BUFFER_SIZE], sig[SESSIONLESS_SIGNATURE_HEX_LEN];
    size_t len = 0;
    sessionless::Identity id;
    importKey(v.at("privateKey"), id);
    if (kind == "caster") {
      len = casterMessage(msg, sizeof(msg), u64(v.at("timestamp")), v.at("spell").c_str(), v.at("casterUUID").c_str(),
                          u64(v.at("totalCost")), v.at("mp") == "true", u64(v.at("ordinal")));
      CHECK_STATUS(signCaster(id, u64(v.at("timestamp")), v.at("spell").c_str(), v.at("casterUUID").c_str(),
                              u64(v.at("totalCost")), v.at("mp") == "true", u64(v.at("ordinal")), sig), Status::Ok);
    } else if (kind == "gateway") {
      len = gatewayMessage(msg, sizeof(msg), u64(v.at("timestamp")), v.at("uuid").c_str(), u64(v.at("minimumCost")), u64(v.at("ordinal")));
      CHECK_STATUS(signGateway(id, u64(v.at("timestamp")), v.at("uuid").c_str(), u64(v.at("minimumCost")), u64(v.at("ordinal")), sig), Status::Ok);
    } else {
      len = registrationMessage(msg, sizeof(msg), u64(v.at("timestamp")), v.at("pubKey").c_str());
      CHECK_STATUS(signRegistration(id, u64(v.at("timestamp")), sig), Status::Ok);
    }
    CHECK(len > 0 && v.at("message") == msg);
    CHECK(v.at("signature") == sig);  // deterministic, so byte-for-byte equal to JS
    CHECK(sessionless::verifyHex(v.at("signature").c_str(), v.at("message").c_str(), v.count("pubKey") ? v.at("pubKey").c_str() : [&] { static char pub[67]; id.publicKeyHex(pub); return pub; }()));
  }
  printf("  %d message vectors\n", n);
  CHECK(n >= 6);
}

static void testGoldenJson(const std::vector<Obj> &vs) {
  int n = 0;
  for (const Obj &v : vs) {
    if (v.at("kind") != "json") continue;
    n++;
    SpellFields s = {u64(v.at("timestamp")), v.at("spell").c_str(), v.at("casterUUID").c_str(), u64(v.at("totalCost")),
                     v.at("mp") == "true", u64(v.at("ordinal")), v.at("casterSignature").c_str()};
    size_t count = (size_t)u64(v.at("gwCount"));
    GatewayFields gws[2];
    for (size_t i = 0; i < count; i++) {
      std::string p = "g" + std::to_string(i) + "_";
      gws[i] = {u64(v.at(p + "timestamp")), v.at(p + "uuid").c_str(), u64(v.at(p + "minimumCost")), u64(v.at(p + "ordinal")),
                v.at(p + "signature").c_str(), v.at(p + "extra").c_str()};
    }
    char out[MAGIC_MAX_SPELL_BODY_BYTES + 1];
    size_t len = 0;
    CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, gws, count), Status::Ok);
    CHECK(v.at("json") == out);
    CHECK(len == strlen(out));
  }
  printf("  %d json vectors\n", n);
  CHECK(n >= 3);
}

static void testGoldenFrames(const std::vector<Obj> &vs) {
  int n = 0;
  for (const Obj &v : vs) {
    if (v.at("kind") != "frame") continue;
    n++;
    const std::string &type = v.at("type");
    std::vector<uint8_t> bytes = unhex(v.at("hex"));
    uint8_t out[MAGIC_FRAME_MAX_SIZE + 8];
    size_t len = 0;
    CHECK(peekType(bytes.data(), bytes.size()) == bytes[0]);
    if (type == "register_request") {
      RegisterRequest f; CHECK_STATUS(decode(bytes.data(), bytes.size(), f), Status::Ok);
      CHECK(f.timestamp == u64(v.at("timestamp")));
      CHECK(unhex(v.at("pubKey")) == std::vector<uint8_t>(f.pubKey, f.pubKey + 33));
      len = encode(f, out, sizeof(out));
    } else if (type == "register_response_ok" || type == "register_response_fail") {
      RegisterResponse f; CHECK_STATUS(decode(bytes.data(), bytes.size(), f), Status::Ok);
      CHECK(f.success == (type == "register_response_ok"));
      if (f.success) CHECK(v.at("casterUUID") == f.casterUUID); else CHECK(f.casterUUID[0] == 0);
      len = encode(f, out, sizeof(out));
    } else if (type == "cast_spell") {
      CastSpellFrame f; CHECK_STATUS(decode(bytes.data(), bytes.size(), f), Status::Ok);
      CHECK(v.at("casterUUID") == f.casterUUID && f.timestamp == u64(v.at("timestamp")) && f.ordinal == u64(v.at("ordinal")));
      len = encode(f, out, sizeof(out));
    } else if (type == "cast_result_ok" || type == "cast_result_fail") {
      CastResult f; CHECK_STATUS(decode(bytes.data(), bytes.size(), f), Status::Ok);
      CHECK(f.success == (type == "cast_result_ok"));
      len = encode(f, out, sizeof(out));
    } else if (type == "time_sync") {
      TimeSync f; CHECK_STATUS(decode(bytes.data(), bytes.size(), f), Status::Ok);
      CHECK(f.epochMs == u64(v.at("epochMs")));
      len = encode(f, out, sizeof(out));
    } else { CHECK(!"unknown frame type in vectors"); }
    CHECK(len == bytes.size() && memcmp(out, bytes.data(), len) == 0);  // re-encodes to identical bytes
  }
  printf("  %d frame vectors\n", n);
  CHECK(n >= 7);
}

static void testFrameSizesMatchDemoStructs() {
  CHECK(MAGIC_FRAME_CAST_SPELL_SIZE == 1 + 37 + 8 + 4 + 64);
  CHECK(MAGIC_FRAME_REGISTER_REQUEST_SIZE == 1 + 33 + 8 + 64);
  CHECK(MAGIC_FRAME_REGISTER_RESPONSE_SIZE == 1 + 1 + 37);
  CHECK(MAGIC_FRAME_CAST_SPELL_SIZE <= 250);  // ESP-NOW cap
}

static void testFramesStrict() {
  CastSpellFrame cf;
  memset(&cf, 0, sizeof(cf));
  strcpy(cf.casterUUID, UUID);
  cf.timestamp = 1; cf.ordinal = 2;
  uint8_t b[MAGIC_FRAME_CAST_SPELL_SIZE];
  CHECK(encode(cf, b, sizeof(b)) == sizeof(b));
  CHECK(encode(cf, b, sizeof(b) - 1) == 0);  // too small

  CastSpellFrame d;
  CHECK_STATUS(decode(b, sizeof(b) - 1, d), Status::BadFrame);   // short
  uint8_t longer[sizeof(b) + 1]; memcpy(longer, b, sizeof(b)); longer[sizeof(b)] = 0;
  CHECK_STATUS(decode(longer, sizeof(longer), d), Status::BadFrame);  // long
  CHECK_STATUS(decode(nullptr, 0, d), Status::BadFrame);
  uint8_t t[sizeof(b)]; memcpy(t, b, sizeof(b)); t[0] = MSG_CAST_RESULT;
  CHECK_STATUS(decode(t, sizeof(t), d), Status::BadFrame);        // wrong type
  memcpy(t, b, sizeof(b)); memset(t + 1, 'a', 37);
  CHECK_STATUS(decode(t, sizeof(t), d), Status::BadFrame);        // uuid has no terminator
  memcpy(t, b, sizeof(b)); t[1] = '"';
  CHECK_STATUS(decode(t, sizeof(t), d), Status::BadFrame);        // JSON-breaking char in uuid
  memcpy(t, b, sizeof(b)); t[1] = 0;
  CHECK_STATUS(decode(t, sizeof(t), d), Status::BadFrame);        // empty uuid
  memcpy(t, b, sizeof(b)); memset(t + 1, 0, 37); memcpy(t + 1, "abc", 3); memset(t + 10, 0x7f, 20);  // garbage after the NUL is ignored (demo frames may carry it)
  CHECK_STATUS(decode(t, sizeof(t), d), Status::Ok);
  CHECK(strcmp(d.casterUUID, "abc") == 0);

  uint8_t r[MAGIC_FRAME_CAST_RESULT_SIZE] = {MSG_CAST_RESULT, 2};
  CastResult cr;
  CHECK_STATUS(decode(r, sizeof(r), cr), Status::BadFrame);       // success must be 0 or 1
  CastSpellFrame bad = cf; strcpy(bad.casterUUID, "bad uuid");
  CHECK(encode(bad, b, sizeof(b)) == 0);                          // encoder refuses unsafe ids too
  CHECK(peekType(nullptr, 0) == 0);
  uint8_t u = 9; CHECK(peekType(&u, 1) == 0);
}

static void testExtraLimit() {
  // exactly 128 bytes is fine, 129 is not
  std::string pad = "{\"d\":\"" + std::string(128 - 8, 'x') + "\"}";
  CHECK(pad.size() == 128);
  CHECK_STATUS(checkExtra(pad.c_str()), Status::Ok);
  std::string over = "{\"d\":\"" + std::string(128 - 7, 'x') + "\"}";
  CHECK(over.size() == 129);
  CHECK_STATUS(checkExtra(over.c_str()), Status::ExtraTooLarge);
  CHECK_STATUS(checkExtra(std::string(100000, '{').c_str()), Status::ExtraTooLarge);  // bounded read

  const char *good[] = {"{}", "{\"a\":1}", "{ \"a\" : [1, 2.5e3, -0.1, true, false, null] }", "{\"a\":{\"b\":{\"c\":{}}}}",
                        "{\"s\":\"q\\\"\\\\\\/\\n\\u00e9\"}"};
  for (const char *g : good) CHECK_STATUS(checkExtra(g), Status::Ok);
  const char *bad[] = {nullptr, "", "[]", "\"x\"", "1", "{", "}", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{a:1}", "{'a':1}",
                       "{\"a\":01}", "{\"a\":1.}", "{\"a\":+1}", "{\"a\":.5}", "{\"a\":tru}", "{\"a\":1} x", "{\"a\":1}{}",
                       "{\"a\":\"\\x\"}", "{\"a\":\"\\u12\"}", "{\"a\":\"unterminated}", "{\"a\":\"\x01\"}", "{\"a\":\"\xc3\xa9\"}",
                       "{\"a\":\"\x7f\"}", "{\"a\":[1,]}", "{\"a\":[1 2]}", "{\"a\":NaN}", "{\"a\":1e}"};
  for (const char *b : bad) CHECK_STATUS(checkExtra(b), Status::BadExtra);
  // depth counts the outer object: 4 nested containers are ok, 5 are not
  CHECK_STATUS(checkExtra("{\"a\":{\"b\":{\"c\":{\"d\":1}}}}"), Status::Ok);        // 4 objects
  CHECK_STATUS(checkExtra("{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":1}}}}}"), Status::BadExtra);  // 5 objects
  CHECK_STATUS(checkExtra("{\"a\":[[[1]]]}"), Status::Ok);                           // object + 3 arrays
  CHECK_STATUS(checkExtra("{\"a\":[[[[1]]]]}"), Status::BadExtra);                   // object + 4 arrays
}

static void testBodyLimit() {
  SpellFields s = {1700000000000ull, "wandCast", UUID, 50, true, 1, HEX128.c_str()};
  std::string extra = "{\"d\":\"" + std::string(128 - 8, 'x') + "\"}";
  GatewayFields g = {1700000000500ull, UUID, 0, 1, HEX128.c_str(), extra.c_str()};
  GatewayFields gws[10];
  for (auto &x : gws) x = g;
  static char out[4096];
  size_t len = 0;
  // 4 gateways with maximum extras fits (this is why 128/2048 were chosen)
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, gws, 4), Status::Ok);
  CHECK(len <= MAGIC_MAX_SPELL_BODY_BYTES);
  printf("  4 gateways x 128-byte extras = %zu bytes (limit %d)\n", len, MAGIC_MAX_SPELL_BODY_BYTES);
  // more gateways: refused as too large even though the buffer is big enough
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, gws, 10), Status::BodyTooLarge);
  // an over-limit extra is refused
  std::string over = "{\"d\":\"" + std::string(128, 'x') + "\"}";
  GatewayFields og = g; og.extraJson = over.c_str();
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, &og, 1), Status::ExtraTooLarge);
  // no extra is fine, null or empty
  og.extraJson = nullptr; CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, &og, 1), Status::Ok);
  og.extraJson = "";      CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, &og, 1), Status::Ok);

  // small buffers: never write past cap, and the result is a clean error
  char guarded[64 + 16];
  memset(guarded, 0x5a, sizeof(guarded));
  CHECK_STATUS(writeSpellJson(guarded, 64, &len, s, gws, 1), Status::BufferTooSmall);
  for (size_t i = 64; i < sizeof(guarded); i++) CHECK((unsigned char)guarded[i] == 0x5a);
  CHECK_STATUS(writeSpellJson(out, 0, &len, s, gws, 1), Status::BadArgument);

  // exact-fit buffer works, one byte less doesn't
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, gws, 1), Status::Ok);
  std::string exact(out);
  std::vector<char> fit(exact.size() + 1);
  CHECK_STATUS(writeSpellJson(fit.data(), fit.size(), nullptr, s, gws, 1), Status::Ok);
  CHECK(exact == fit.data());
  CHECK_STATUS(writeSpellJson(fit.data(), fit.size() - 1, nullptr, s, gws, 1), Status::BufferTooSmall);

  // bad fields
  SpellFields bs = s; bs.spell = "has space";
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, bs, gws, 0), Status::BadArgument);
  bs = s; bs.casterSignatureHex = "ABC";
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, bs, gws, 0), Status::BadArgument);
  bs = s; bs.casterUUID = "a\",\"mp\":false,\"x\":\"";  // JSON injection attempt via an id
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, bs, gws, 0), Status::BadArgument);
  CHECK_STATUS(writeSpellJson(out, sizeof(out), &len, s, nullptr, 1), Status::BadArgument);
}

static void testMessagesEdgeCases() {
  char m[MAGIC_MESSAGE_BUFFER_SIZE];
  size_t n = casterMessage(m, sizeof(m), 18446744073709551615ull, "s", "u", 18446744073709551615ull, false, 18446744073709551615ull);
  CHECK(n > 0 && std::string(m) == "18446744073709551615su18446744073709551615false18446744073709551615");
  n = casterMessage(m, sizeof(m), 0, "s", "u", 0, true, 0);
  CHECK(std::string(m) == "0su0true0");
  CHECK(casterMessage(m, 5, 1, "wandCast", UUID, 50, true, 1) == 0);   // buffer too small => 0, not truncated
  CHECK(casterMessage(m, sizeof(m), 1, "bad spell", UUID, 50, true, 1) == 0);
  CHECK(casterMessage(m, sizeof(m), 1, "", UUID, 50, true, 1) == 0);
  CHECK(casterMessage(nullptr, sizeof(m), 1, "s", UUID, 50, true, 1) == 0);
  // worst case fits the documented buffer
  std::string longSpell(63, 's'), longUuid(36, 'u');
  CHECK(casterMessage(m, sizeof(m), 18446744073709551615ull, longSpell.c_str(), longUuid.c_str(), 18446744073709551615ull, false, 18446744073709551615ull) > 0);
  CHECK(registrationMessage(m, sizeof(m), 1, "02") == 0);               // pubkey must be 66 hex chars
  CHECK(registrationMessage(m, sizeof(m), 1, std::string(66, 'g').c_str()) == 0);
  CHECK(registrationMessage(m, sizeof(m), 1, std::string(66, 'a').c_str()) == 66 + 1);
  // signing without keys fails cleanly
  sessionless::Identity none;
  char sig[SESSIONLESS_SIGNATURE_HEX_LEN];
  CHECK_STATUS(signCaster(none, 1, "s", UUID, 1, true, 1, sig), Status::SignFailed);
  sessionless::Identity id; id.ephemeral();
  CHECK_STATUS(signCaster(id, 1, "bad spell", UUID, 1, true, 1, sig), Status::BadArgument);
}

static void testClock() {
  EpochClock c;
  uint64_t t = 0;
  CHECK(!c.synced());
  CHECK_STATUS(c.now(1000, &t), Status::NotSynced);  // refuses to guess
  c.sync(1700000000000ull, 5000);
  CHECK_STATUS(c.now(5000, &t), Status::Ok); CHECK(t == 1700000000000ull);
  CHECK_STATUS(c.now(65000, &t), Status::Ok); CHECK(t == 1700000060000ull);
  CHECK_STATUS(c.now(4999, &t), Status::ClockWentBack);
  // beyond 32-bit millis wrap (49.7 days) still correct with a 64-bit monotonic
  uint64_t day60 = 60ull * 24 * 3600 * 1000;
  CHECK_STATUS(c.now(5000 + day60, &t), Status::Ok); CHECK(t == 1700000000000ull + day60);
  c.sync(1800000000000ull, 9000);  // re-sync replaces
  CHECK_STATUS(c.now(9001, &t), Status::Ok); CHECK(t == 1800000000001ull);
}

struct CountingStore : OrdinalStore {
  uint32_t v = 0; bool has = false; bool failSave = false; bool failLoad = false;
  Status load(uint32_t *o) override { if (failLoad) return Status::StoreError; if (!has) return Status::NotFound; *o = v; return Status::Ok; }
  Status save(uint32_t x) override { if (failSave) return Status::StoreError; v = x; has = true; return Status::Ok; }
};

static void testOrdinal() {
  CountingStore store;
  uint32_t o = 0;
  { OrdinalCounter c(store);
    CHECK_STATUS(c.next(&o), Status::Ok); CHECK(o == 1);
    CHECK_STATUS(c.next(&o), Status::Ok); CHECK(o == 2); }
  { OrdinalCounter c(store);  // "after a reset": continues, never repeats
    CHECK_STATUS(c.next(&o), Status::Ok); CHECK(o == 3); }
  store.failSave = true;
  { OrdinalCounter c(store);
    CHECK_STATUS(c.next(&o), Status::StoreError);
    CHECK(store.v == 3);  // nothing handed out, nothing advanced
    store.failSave = false;
    CHECK_STATUS(c.next(&o), Status::Ok); CHECK(o == 4); }
  store.failLoad = true;
  { OrdinalCounter c(store); CHECK_STATUS(c.next(&o), Status::StoreError); }
  store.failLoad = false; store.v = 0xFFFFFFFFu;
  { OrdinalCounter c(store); CHECK_STATUS(c.next(&o), Status::Overflow); }
  MemoryOrdinalStore mem; OrdinalCounter c(mem);
  CHECK_STATUS(c.next(&o), Status::Ok); CHECK(o == 1);
  CHECK_STATUS(c.next(nullptr), Status::BadArgument);
}

static Status get(const std::string &json, const char *key, JsonField *f) { return jsonGetTopLevel(json.c_str(), json.size(), key, f); }

static void testJsonGetTopLevel() {
  JsonField f;
  // the shape fount actually returns
  std::string ok = "{\"success\":true,\"signatureMap\":{}}";
  CHECK_STATUS(get(ok, "success", &f), Status::Ok);
  CHECK(f.type == JsonType::Bool && f.boolean);
  CHECK_STATUS(get("{\"success\":false}", "success", &f), Status::Ok);
  CHECK(f.type == JsonType::Bool && !f.boolean);
  CHECK_STATUS(get(ok, "signatureMap", &f), Status::Ok);
  CHECK(f.type == JsonType::Other);
  CHECK_STATUS(get(ok, "nope", &f), Status::NotFound);

  std::string user = "{\"pubKey\":\"02ab\",\"mp\":1000,\"uuid\":\"9be79b1b-6709-4567-8391-43a95252586c\",\"nested\":{\"uuid\":\"inner\"}}";
  char uuid[40];
  CHECK_STATUS(get(user, "uuid", &f), Status::Ok);
  CHECK(f.type == JsonType::String && jsonCopyString(f, uuid, sizeof(uuid)) && std::string(uuid) == "9be79b1b-6709-4567-8391-43a95252586c");  // top-level, not the nested one
  CHECK_STATUS(get(user, "mp", &f), Status::Ok);
  CHECK(f.type == JsonType::Number && std::string(f.start, f.len) == "1000");

  // a key name appearing inside a string VALUE must not be mistaken for a key
  CHECK_STATUS(get("{\"error\":\"x\\\"success\\\":true\"}", "success", &f), Status::NotFound);
  CHECK_STATUS(get("{\"error\":\"\\\"success\\\":true\"}", "success", &f), Status::NotFound);
  // duplicate keys: the last wins, as JSON.parse does
  CHECK_STATUS(get("{\"a\":false,\"a\":true}", "a", &f), Status::Ok);
  CHECK(f.boolean);
  // whitespace is fine; empty object is valid but empty
  CHECK_STATUS(get(" { \"a\" : 1 } ", "a", &f), Status::Ok);
  CHECK_STATUS(get("{}", "a", &f), Status::NotFound);
  // UTF-8 passes through in resolver replies (unlike `extra`)
  CHECK_STATUS(get("{\"e\":\"caf\xc3\xa9\",\"success\":true}", "success", &f), Status::Ok);
  // garbage is BadResponse, never "key absent"
  const char *bad[] = {"", "not json", "[]", "\"s\"", "{", "{\"a\":1", "{\"a\":1,}", "{\"a\":1} trailing", "{\"a\":tru}", "<html>502 Bad Gateway</html>"};
  for (const char *b : bad) CHECK_STATUS(jsonGetTopLevel(b, strlen(b), "a", &f), Status::BadResponse);
  // depth: 8 is allowed, 9 is not
  std::string d8 = "{\"a\":" + std::string(7, '[') + "1" + std::string(7, ']') + "}";
  std::string d9 = "{\"a\":" + std::string(8, '[') + "1" + std::string(8, ']') + "}";
  CHECK_STATUS(get(d8, "a", &f), Status::Ok);
  CHECK_STATUS(get(d9, "a", &f), Status::BadResponse);
  // argument checks, including the NUL-termination contract
  CHECK_STATUS(jsonGetTopLevel(nullptr, 0, "a", &f), Status::BadArgument);
  CHECK_STATUS(jsonGetTopLevel("{}", 2, nullptr, &f), Status::BadArgument);
  CHECK_STATUS(jsonGetTopLevel("{\"a\":1}xx", 7, "a", &f), Status::BadArgument);  // len doesn't land on the NUL
  // jsonCopyString refuses escapes and overflow
  get("{\"s\":\"a\\nb\"}", "s", &f);
  CHECK(!jsonCopyString(f, uuid, sizeof(uuid)));
  get("{\"s\":\"abcdef\"}", "s", &f);
  CHECK(!jsonCopyString(f, uuid, 6) && jsonCopyString(f, uuid, 7));
}

static void testUuidStoreAndLoopback() {
  MemoryUuidStore store;
  const uint8_t a[] = {1}, b[] = {2};
  PeerId pa(a, 1), pb(b, 1), none;
  char u[MAGIC_FRAME_UUID_BYTES];
  CHECK_STATUS(store.load(pa, u), Status::NotFound);
  CHECK_STATUS(store.save(pa, UUID), Status::Ok);
  CHECK_STATUS(store.load(pa, u), Status::Ok);
  CHECK(std::string(u) == UUID);
  CHECK_STATUS(store.load(pb, u), Status::NotFound);               // scoped per peer
  CHECK_STATUS(store.save(pa, "other-id"), Status::Ok);            // replaces in place
  store.load(pa, u); CHECK(std::string(u) == "other-id");
  CHECK_STATUS(store.save(none, "own-id"), Status::Ok);            // empty PeerId is a valid scope (a gateway's own)
  CHECK_STATUS(store.save(pb, "b-id"), Status::Ok);
  const uint8_t c[] = {3}, d[] = {4}, e[] = {5};
  CHECK_STATUS(store.save(PeerId(c, 1), "c-id"), Status::Ok);
  CHECK_STATUS(store.save(PeerId(d, 1), "d-id"), Status::StoreError);  // full: refuses instead of evicting an identity
  store.load(pa, u); CHECK(std::string(u) == "other-id");
  (void)e;
  CHECK_STATUS(store.save(pa, "bad id"), Status::BadArgument);
  CHECK_STATUS(store.save(pa, ""), Status::BadArgument);

  CHECK(PeerId(a, 1) == pa && PeerId(a, 1) != pb && none.isNone() && !pa.isNone());
  const uint8_t big[12] = {0};
  CHECK(PeerId(big, 12).len == 8);                                  // clamped, never overflows

  LoopbackLink link;
  uint8_t buf[MAGIC_FRAME_MAX_SIZE]; size_t len; PeerId from;
  CHECK(!link.a().receive(buf, sizeof(buf), &len, &from));
  uint8_t f1[] = {1, 2, 3};
  CHECK_STATUS(link.a().send(nullptr, f1, 3), Status::Ok);
  CHECK(!link.a().receive(buf, sizeof(buf), &len, &from));          // not delivered to the sender
  CHECK(link.b().receive(buf, sizeof(buf), &len, &from) && len == 3 && memcmp(buf, f1, 3) == 0 && from == LoopbackLink::idOfA());
  CHECK_STATUS(link.a().send(nullptr, nullptr, 0), Status::BadArgument);
  uint8_t huge[MAGIC_FRAME_MAX_SIZE + 1] = {0};
  CHECK_STATUS(link.a().send(nullptr, huge, sizeof(huge)), Status::BufferTooSmall);
  // a frame bigger than the receiver's buffer is discarded, not truncated; the next one still arrives
  uint8_t f100[100]; memset(f100, 7, sizeof(f100));
  link.a().send(nullptr, f100, sizeof(f100)); link.a().send(nullptr, f1, 3);
  uint8_t small[10];
  CHECK(link.b().receive(small, sizeof(small), &len, &from) && len == 3);
  // a full queue drops the newest and counts it, like a busy radio
  for (int i = 0; i < 12; i++) link.b().send(nullptr, f1, 3);
  CHECK(link.dropped() == 4);
  int got = 0; while (link.a().receive(buf, sizeof(buf), &len, &from)) got++;
  CHECK(got == 8);
}

static Status reg(int http, const std::string &body, char *uuid) { return parseRegisterResponse(http, body.c_str(), body.size(), uuid); }
static Status res(int http, const std::string &body, bool *success) {
  ResolveResult r = {true};  // start true to prove the parser clears it
  Status s = parseResolveResponse(http, body.c_str(), body.size(), &r);
  *success = r.success;
  return s;
}

static void testResolverContract() {
  char uuid[MAGIC_FRAME_UUID_BYTES];
  // exactly what the real fount returns on registration (captured from a live run)
  std::string user = "{\"pubKey\":\"037e81\",\"mp\":1000,\"maxMP\":1000,\"ordinal\":0,\"uuid\":\"9be79b1b-6709-4567-8391-43a95252586c\"}";
  CHECK_STATUS(reg(200, user, uuid), Status::Ok);
  CHECK(std::string(uuid) == "9be79b1b-6709-4567-8391-43a95252586c");
  CHECK_STATUS(reg(200, "{\"pubKey\":\"x\"}", uuid), Status::BadResponse);                    // no uuid
  CHECK_STATUS(reg(200, "{\"uuid\":\"a b\"}", uuid), Status::BadResponse);                    // unsafe characters
  CHECK_STATUS(reg(200, "{\"uuid\":\"\"}", uuid), Status::BadResponse);
  CHECK_STATUS(reg(200, "{\"uuid\":\"" + std::string(37, 'a') + "\"}", uuid), Status::BadResponse);  // too long for the wire
  CHECK_STATUS(reg(200, "{\"uuid\":\"a\\\"b\"}", uuid), Status::BadResponse);              // escapes
  CHECK_STATUS(reg(200, "{\"uuid\":5}", uuid), Status::BadResponse);
  CHECK_STATUS(reg(200, "<html>", uuid), Status::BadResponse);
  CHECK_STATUS(reg(403, "{\"error\":\"auth error\"}", uuid), Status::ResolverRejected);
  CHECK_STATUS(reg(404, "{\"error\":\"not found\"}", uuid), Status::ResolverRejected);
  CHECK_STATUS(reg(502, "<html>Bad Gateway</html>", uuid), Status::ResolverUnreachable);
  CHECK_STATUS(reg(503, "", uuid), Status::ResolverUnreachable);

  bool ok;
  CHECK_STATUS(res(200, "{\"success\":true,\"signatureMap\":{}}", &ok), Status::Ok); CHECK(ok);   // captured from the real fount
  CHECK_STATUS(res(900, "{\"success\":false}", &ok), Status::Ok); CHECK(!ok);                       // fount's non-standard rejection
  CHECK_STATUS(res(200, "{\"error\":\"no time like the present\"}", &ok), Status::Ok); CHECK(!ok);  // answered: no
  CHECK_STATUS(res(200, "{\"success\":\"true\"}", &ok), Status::Ok); CHECK(!ok);                  // a string is not a boolean
  CHECK_STATUS(res(200, "{\"success\":1}", &ok), Status::Ok); CHECK(!ok);
  CHECK_STATUS(res(400, "{\"success\":true}", &ok), Status::Ok); CHECK(!ok);                        // must be HTTP 200 too
  CHECK_STATUS(res(403, "{\"success\":false,\"error\":\"bad caster signature\"}", &ok), Status::Ok); CHECK(!ok);
  CHECK_STATUS(res(200, "{\"error\":\"x \\\"success\\\":true\"}", &ok), Status::Ok); CHECK(!ok);  // key name inside a string value
  CHECK_STATUS(res(200, "garbage", &ok), Status::BadResponse); CHECK(!ok);
  CHECK_STATUS(res(200, "", &ok), Status::BadResponse);
  CHECK_STATUS(res(500, "{\"success\":true}", &ok), Status::ResolverUnreachable); CHECK(!ok);       // never trust a 5xx body
  CHECK_STATUS(res(504, "", &ok), Status::ResolverUnreachable);

  char body[400]; size_t n = 0;
  std::string pub(66, 'a'), sig(128, 'b');
  CHECK_STATUS(buildRegisterBody(body, sizeof(body), &n, 1700000000000ULL, pub.c_str(), sig.c_str()), Status::Ok);
  CHECK(std::string(body) == "{\"timestamp\":\"1700000000000\",\"pubKey\":\"" + pub + "\",\"signature\":\"" + sig + "\"}" && n == strlen(body));
  CHECK_STATUS(buildRegisterBody(body, 50, &n, 1, pub.c_str(), sig.c_str()), Status::BufferTooSmall);
  CHECK_STATUS(buildRegisterBody(body, sizeof(body), &n, 1, "02", sig.c_str()), Status::BadArgument);
  CHECK_STATUS(buildRegisterBody(body, sizeof(body), &n, 1, (std::string(65, 'a') + "\"").c_str(), sig.c_str()), Status::BadArgument);  // quote can't sneak in
  CHECK_STATUS(buildRegisterBody(body, sizeof(body), &n, 1, pub.c_str(), (std::string(127, 'b') + "G").c_str()), Status::BadArgument);
  CHECK_STATUS(buildRegisterBody(body, sizeof(body), &n, 1, nullptr, sig.c_str()), Status::BadArgument);
}

static void testResolverUrlPolicy() {
  struct { const char *url; bool plain; Status want; } cases[] = {
    {"https://dev.fount.allyabase.com", false, Status::Ok},
    {"https://dev.fount.allyabase.com/fount", false, Status::Ok},
    {"https://allyabase-gateway.netlify.app:8443/fount", false, Status::Ok},
    {"http://localhost:3006", false, Status::Ok},
    {"http://localhost", false, Status::Ok},
    {"http://127.0.0.1:3006/x", false, Status::Ok},
    {"http://[::1]:3006", false, Status::Ok},
    {"http://fount.example.com", false, Status::BadArgument},
    {"http://fount.example.com", true, Status::Ok},
    {"http://localhost@evil.example.com", false, Status::BadArgument},
    {"https://user:pw@fount.example.com", false, Status::BadArgument},
    {"http://localhost.evil.example.com", false, Status::BadArgument},
    {"http://127.0.0.1.evil.example.com", false, Status::BadArgument},
    {"http://", false, Status::BadArgument},
    {"https://", false, Status::BadArgument},
    {"https://:443", false, Status::BadArgument},
    {"http://[::1", false, Status::BadArgument},
    {"ftp://localhost", false, Status::BadArgument},
    {"file:///etc/passwd", false, Status::BadArgument},
    {"localhost:3006", false, Status::BadArgument},
    {"", false, Status::BadArgument},
    {nullptr, false, Status::BadArgument},
  };
  for (auto &c : cases) {
    Status got = checkResolverUrl(c.url, c.plain);
    checks++;
    if (got != c.want) { failures++; printf("FAIL url policy '%s' plain=%d => %s\n", c.url ? c.url : "(null)", c.plain, statusName(got)); }
  }
  bool https = false;
  CHECK_STATUS(checkResolverUrl("https://x.example.com", false, &https), Status::Ok); CHECK(https);
  CHECK_STATUS(checkResolverUrl("http://localhost", false, &https), Status::Ok); CHECK(!https);
  CHECK_STATUS(checkResolverUrl(("https://" + std::string(3000, 'a')).c_str(), false), Status::BadArgument);  // absurd length
}

int main() {
  std::vector<Obj> vs = loadVectors();
  CHECK(vs.size() >= 16);
  printf("golden messages\n"); testGoldenMessages(vs);
  printf("golden json\n");     testGoldenJson(vs);
  printf("golden frames\n");   testGoldenFrames(vs);
  printf("frame sizes\n");     testFrameSizesMatchDemoStructs();
  printf("frames strict\n");   testFramesStrict();
  printf("extra limit\n");     testExtraLimit();
  printf("body limit\n");      testBodyLimit();
  printf("messages\n");        testMessagesEdgeCases();
  printf("clock\n");           testClock();
  printf("ordinal\n");         testOrdinal();
  printf("json getter\n");     testJsonGetTopLevel();
  printf("uuid store + loopback\n"); testUuidStoreAndLoopback();
  printf("resolver contract\n"); testResolverContract();
  printf("resolver url policy\n"); testResolverUrlPolicy();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
