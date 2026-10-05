// End to end on the host: Wand -> LoopbackLink -> Gateway -> CurlResolver -> strict-proxy.mjs -> REAL fount.
// Uses the real clock. Run through test/e2e/run.sh, which starts fount and the proxy.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <string>

#include "MagicCurlResolver.h"
#include "MagicGateway.h"
#include "MagicWand.h"

using namespace magic;

static int failures = 0;
#define EXPECT(cond, ...) do { if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); printf("     " __VA_ARGS__); printf("\n"); } } while (0)

static uint64_t monoMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static uint64_t epochMs() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Listener : WandListener {
  int results = 0; bool last = false; int registered = 0;
  void onResult(bool s) override { results++; last = s; }
  void onRegistered(const char *) override { registered++; }
};
struct Handler : GatewayHandler {
  int casts = 0; std::string extraJson;
  void onCast(const CastInfo &) override { casts++; }
  size_t extra(const CastInfo &, char *out, size_t) override { memcpy(out, extraJson.data(), extraJson.size()); return extraJson.size(); }
};

int main(int argc, char **argv) {
  std::string base = argc > 1 ? argv[1] : "http://127.0.0.1:3099";
  CurlResolverConfig rc;
  rc.baseUrl = base;
  CurlResolver resolver(rc);
  EXPECT(resolver.check() == Status::Ok, "config rejected");

  LoopbackLink link;
  sessionless::Identity wandId, gwId;
  EXPECT(wandId.ephemeral() == sessionless::Status::Ok && gwId.ephemeral() == sessionless::Status::Ok, "keys");
  MemoryOrdinalStore wandOrd, gwOrd;
  MemoryUuidStore wandUuids, gwUuids;
  Handler handler;
  Listener listener;
  GatewayConfig cfg;
  cfg.spell = {"wandCast", 50, true};
  Gateway gw(gwId, link.b(), resolver, gwOrd, gwUuids, monoMs, cfg, &handler);
  Wand wand(wandId, link.a(), wandOrd, wandUuids, monoMs, &listener);

  gw.setEpoch(epochMs());
  Status s = gw.begin();
  EXPECT(s == Status::Ok, "gateway register: %s (http %ld)", statusName(s), resolver.lastHttpStatus());
  printf("gateway uuid %s\n", gw.uuid());

  gw.poll(); wand.poll();
  EXPECT(wand.timeSynced(), "no time sync");
  s = wand.registerWithGateway();
  EXPECT(s == Status::Ok, "registerWithGateway: %s", statusName(s));
  gw.poll(); wand.poll();
  EXPECT(wand.registered(), "wand not registered");
  printf("wand uuid    %s\n", wand.casterUUID());

  // 1. a normal cast, with a gateway extra
  handler.extraJson = "{\"effect\":\"sparkle\",\"n\":3}";
  s = wand.cast(cfg.spell);
  EXPECT(s == Status::Ok, "cast: %s", statusName(s));
  gw.poll(); wand.poll();
  EXPECT(listener.results == 1 && listener.last, "cast 1 should resolve (results=%d last=%d, http %ld)", listener.results, listener.last, resolver.lastHttpStatus());

  // 2. a second cast: ordinals keep increasing, still resolves
  s = wand.cast(cfg.spell);
  gw.poll(); wand.poll();
  EXPECT(listener.results == 2 && listener.last, "cast 2 should resolve");

  // 3. a cast whose frame is altered in transit: the resolver side must refuse it
  s = wand.cast(cfg.spell);
  uint8_t frame[MAGIC_FRAME_MAX_SIZE]; size_t len; PeerId from;
  EXPECT(link.b().receive(frame, sizeof(frame), &len, &from), "no frame to tamper with");
  frame[70] ^= 0x01;
  link.a().send(nullptr, frame, len);
  gw.poll(); wand.poll();
  EXPECT(listener.results == 3 && !listener.last, "tampered cast must NOT resolve");

  // 4. the wand is not left stuck: the next honest cast works
  s = wand.cast(cfg.spell);
  gw.poll(); wand.poll();
  EXPECT(listener.results == 4 && listener.last, "recovery cast should resolve");

  // 5. an oversized extra never leaves the gateway
  handler.extraJson = "{\"d\":\"" + std::string(130, 'x') + "\"}";
  s = wand.cast(cfg.spell);
  gw.poll(); wand.poll();
  EXPECT(listener.results == 5 && !listener.last, "oversized extra must not resolve");

  printf(failures ? "\nE2E FAILED (%d)\n" : "\nE2E OK\n", failures);
  return failures ? 1 : 0;
}
