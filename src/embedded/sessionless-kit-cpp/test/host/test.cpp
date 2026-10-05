// Host tests for SessionlessKit. Plain asserts, no framework, so it runs anywhere with a C++ compiler.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "SessionlessFileStore.h"
#include "SessionlessKit.h"

using namespace sessionless;

static int checks = 0, failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define CHECK_STATUS(expr, want) \
  do { Status got_ = (expr); checks++; if (got_ != (want)) { failures++; printf("FAIL %s:%d  %s => %s, wanted %s\n", __FILE__, __LINE__, #expr, statusName(got_), statusName(want)); } } while (0)

// The vectors file is flat and machine-generated: pull "key": "value" strings in order.
struct Vector { std::string priv, pub, message, sig; };

static std::string unescape(const std::string &s) {  // JSON string body -> UTF-8 (handles \uXXXX BMP + simple escapes)
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\') { out += s[i]; continue; }
    char c = s[++i];
    if (c == 'n') out += '\n'; else if (c == 't') out += '\t'; else if (c == 'r') out += '\r';
    else if (c == 'u') {
      unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
      i += 4;
      if (cp < 0x80) out += (char)cp;
      else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
      else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    } else out += c;
  }
  return out;
}

static std::vector<Vector> loadVectors() {
  std::ifstream in(VECTORS_PATH);
  if (!in) { printf("FAIL cannot open %s (run from test/host)\n", VECTORS_PATH); exit(2); }
  std::stringstream ss; ss << in.rdbuf();
  std::string text = ss.str();
  size_t start = text.find("\"vectors\"");
  std::vector<Vector> out;
  auto field = [&](const char *name, size_t &pos) {
    std::string key = std::string("\"") + name + "\": \"";
    size_t k = text.find(key, pos);
    if (k == std::string::npos) return std::string("\x01");
    size_t b = k + key.size(), e = b;
    while (text[e] != '"') e += (text[e] == '\\') ? 2 : 1;
    pos = e;
    return unescape(text.substr(b, e - b));
  };
  size_t pos = start;
  while (true) {
    Vector v;
    v.priv = field("privateKey", pos);
    if (v.priv == "\x01") break;
    v.pub = field("pubKey", pos);
    v.message = field("message", pos);
    v.sig = field("signature", pos);
    out.push_back(v);
  }
  return out;
}

static void testVectors() {
  std::vector<Vector> vs = loadVectors();
  CHECK(vs.size() >= 20);
  for (const Vector &v : vs) {
    // verify the JS signature
    CHECK(verifyHex(v.sig.c_str(), v.message.c_str(), v.pub.c_str()));

    // import the JS private key, derive the same pubkey, and reproduce the signature byte-for-byte
    MemoryKeyStore store;
    Identity id;
    CHECK_STATUS(id.importPrivateKeyHex(store, v.priv.c_str()), Status::Ok);
    char pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN], sig[SESSIONLESS_SIGNATURE_HEX_LEN];
    id.publicKeyHex(pub);
    CHECK(v.pub == pub);
    CHECK_STATUS(id.signHex(v.message.c_str(), sig), Status::Ok);
    CHECK(v.sig == sig);

    // tampering is rejected
    std::string bad = v.message + "!";
    CHECK(!verifyHex(v.sig.c_str(), bad.c_str(), v.pub.c_str()));
  }
  printf("  %zu vectors\n", vs.size());
}

static void testMalformed() {
  Identity id;
  CHECK_STATUS(id.ephemeral(), Status::Ok);
  char pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN], sig[SESSIONLESS_SIGNATURE_HEX_LEN];
  id.publicKeyHex(pub);
  id.signHex("m", sig);
  CHECK(verifyHex(sig, "m", pub));
  CHECK(!verifyHex(nullptr, "m", pub));
  CHECK(!verifyHex(sig, nullptr, pub));
  CHECK(!verifyHex("", "m", pub));
  CHECK(!verifyHex("zz", "m", pub));
  CHECK(!verifyHex(sig, "m", "02"));
  std::string shortSig(sig, 126);
  CHECK(!verifyHex(shortSig.c_str(), "m", pub));
  std::string longSig = std::string(sig) + "00";
  CHECK(!verifyHex(longSig.c_str(), "m", pub));
  std::string zeroPub = "02" + std::string(64, '0');  // not on the curve
  CHECK(!verifyHex(sig, "m", zeroPub.c_str()));
  std::string zeroSig(128, '0');
  CHECK(!verifyHex(zeroSig.c_str(), "m", pub));
  // wrong key
  Identity other;
  other.ephemeral();
  char otherPub[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  other.publicKeyHex(otherPub);
  CHECK(!verifyHex(sig, "m", otherPub));
  // empty message is valid and distinct
  char emptySig[SESSIONLESS_SIGNATURE_HEX_LEN];
  CHECK_STATUS(id.signHex("", emptySig), Status::Ok);
  CHECK(verifyHex(emptySig, "", pub));
  CHECK(!verifyHex(emptySig, "m", pub));
  // fromHex strictness
  uint8_t b[2];
  CHECK(fromHex("00ff", b, 2) && b[0] == 0 && b[1] == 0xff);
  CHECK(fromHex("00FF", b, 2));
  CHECK(!fromHex("00f", b, 2));
  CHECK(!fromHex("00ffff", b, 2));
  CHECK(!fromHex("00gg", b, 2));
}

static void testStores() {
  MemoryKeyStore mem;
  Identity a, b;
  CHECK_STATUS(a.load(mem), Status::NoKeys);
  CHECK_STATUS(a.loadOrCreate(mem), Status::Ok);
  CHECK_STATUS(b.loadOrCreate(mem), Status::Ok);  // loads, does not recreate
  CHECK(memcmp(a.publicKey(), b.publicKey(), SESSIONLESS_PUBLIC_KEY_SIZE) == 0);
  Identity c;
  CHECK_STATUS(c.create(mem), Status::KeysExist);
  CHECK(!c.ready());
  uint8_t dummy[64];
  CHECK_STATUS(c.sign("x", dummy), Status::NotReady);

  // import: same key ok, different key refused, garbage refused
  char privHex[SESSIONLESS_PRIVATE_KEY_HEX_LEN];
  a.exportPrivateKeyHex(privHex);
  Identity d;
  CHECK_STATUS(d.importPrivateKeyHex(mem, privHex), Status::Ok);
  Identity e;
  e.ephemeral();
  char otherPriv[SESSIONLESS_PRIVATE_KEY_HEX_LEN];
  e.exportPrivateKeyHex(otherPriv);
  CHECK_STATUS(d.importPrivateKeyHex(mem, otherPriv), Status::KeysExist);
  MemoryKeyStore fresh;
  CHECK_STATUS(d.importPrivateKeyHex(fresh, "abc"), Status::BadArgument);
  CHECK_STATUS(d.importPrivateKeyHex(fresh, std::string(64, '0').c_str()), Status::BadArgument);  // zero key
  CHECK_STATUS(d.importPrivateKeyHex(fresh, std::string(64, 'f').c_str()), Status::BadArgument);  // >= curve order

  // a store that fails to save must not leave a usable (unpersisted) identity
  struct FailingStore : KeyStore {
    Status load(uint8_t *, uint8_t *, bool &) override { return Status::NoKeys; }
    Status save(const uint8_t *, const uint8_t *) override { return Status::StoreError; }
  } failing;
  Identity f;
  CHECK_STATUS(f.loadOrCreate(failing), Status::StoreError);
  CHECK(!f.ready());

  // a store that reports corrupt data is never overwritten
  struct CorruptStore : KeyStore {
    int saves = 0;
    Status load(uint8_t *, uint8_t *, bool &) override { return Status::CorruptKeys; }
    Status save(const uint8_t *, const uint8_t *) override { saves++; return Status::Ok; }
  } corrupt;
  Identity g;
  CHECK_STATUS(g.loadOrCreate(corrupt), Status::CorruptKeys);
  CHECK_STATUS(g.create(corrupt), Status::KeysExist);
  CHECK(corrupt.saves == 0);

  // a stored pubkey that disagrees with its private key is corruption
  struct MismatchStore : KeyStore {
    Status load(uint8_t *priv, uint8_t *pub, bool &has) override {
      memset(priv, 0x11, 32);
      memset(pub, 0x02, 33);
      has = true;
      return Status::Ok;
    }
    Status save(const uint8_t *, const uint8_t *) override { return Status::Ok; }
  } mismatch;
  Identity h;
  CHECK_STATUS(h.load(mismatch), Status::CorruptKeys);
}

static void testRng() {
  // no RNG => loud failure, never silent weak keys
  MemoryKeyStore mem;
  Identity id;
  CHECK_STATUS(id.loadOrCreate(mem, nullptr), Status::RngError);
  CHECK_STATUS(id.ephemeral(nullptr), Status::RngError);
  // failing RNG
  RandomFn failing = [](uint8_t *, size_t) { return false; };
  CHECK_STATUS(id.loadOrCreate(mem, failing), Status::RngError);
  // stuck RNG (all zero => invalid key every time)
  RandomFn zeros = [](uint8_t *out, size_t n) { memset(out, 0, n); return true; };
  CHECK_STATUS(id.loadOrCreate(mem, zeros), Status::RngError);
  CHECK(!id.ready());
  // two real identities differ
  Identity x, y;
  x.ephemeral();
  y.ephemeral();
  CHECK(memcmp(x.publicKey(), y.publicKey(), SESSIONLESS_PUBLIC_KEY_SIZE) != 0);
}

static void testFileStore() {
  char dir[] = "/tmp/sessionless-kit-test-XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  std::string path = std::string(dir) + "/keys.txt";
  {
    FileKeyStore store(path);
    Identity a;
    CHECK_STATUS(a.loadOrCreate(store), Status::Ok);
    FileKeyStore again(path);
    Identity b;
    CHECK_STATUS(b.load(again), Status::Ok);
    CHECK(memcmp(a.publicKey(), b.publicKey(), SESSIONLESS_PUBLIC_KEY_SIZE) == 0);
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  }
  {  // corrupt file: refuse, don't overwrite
    FILE *f = fopen(path.c_str(), "w");
    fputs("not hex at all\n", f);
    fclose(f);
    FileKeyStore store(path);
    Identity c;
    CHECK_STATUS(c.loadOrCreate(store), Status::CorruptKeys);
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    CHECK(line == "not hex at all");
  }
  unlink(path.c_str());
  rmdir(dir);
}

int main() {
  printf("vectors\n");   testVectors();
  printf("malformed\n"); testMalformed();
  printf("stores\n");    testStores();
  printf("rng\n");       testRng();
  printf("file store\n"); testFileStore();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
