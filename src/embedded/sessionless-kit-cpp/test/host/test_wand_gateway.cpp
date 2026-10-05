// Wand <-> Gateway over a real LoopbackLink transport, against a strict resolver that actually verifies
// every signature. The checks that matter are on the OUTCOME of the whole stack, not on internals.
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "MagicGateway.h"
#include "MagicWand.h"
#include "StrictLocalResolver.h"

using namespace magic;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STATUS(expr, want) do { Status g_ = (expr); checks++; if (g_ != (want)) { failures++; printf("FAIL %s:%d  %s => %s, wanted %s\n", __FILE__, __LINE__, #expr, statusName(g_), statusName(want)); } } while (0)

static const uint64_t BASE = 1700000000000ULL;
static uint64_t g_mono = 1000;
static uint64_t monoFn() { return g_mono; }
static uint64_t epochNow() { return BASE + g_mono; }

struct Listener : WandListener {
  int synced = 0, registered = 0, regFailed = 0, results = 0, timeouts = 0;
  bool lastSuccess = false;
  char uuid[40] = {0};
  void onTimeSynced(uint64_t) override { synced++; }
  void onRegistered(const char *u) override { registered++; strcpy(uuid, u); }
  void onRegistrationFailed() override { regFailed++; }
  void onResult(bool s) override { results++; lastSuccess = s; }
  void onTimeout() override { timeouts++; }
};

struct Handler : GatewayHandler {
  int casts = 0, resolved = 0;
  bool lastSuccess = false;
  Status lastStatus = Status::Ok;
  std::string extraJson;  // what extra() should return
  int castsAtFirstResolverCall = -1;
  StrictLocalResolver *watch = nullptr;
  void onCast(const CastInfo &) override { casts++; }
  size_t extra(const CastInfo &, char *out, size_t cap) override {
    if (extraJson.size() > cap) { memcpy(out, extraJson.data(), cap); return extraJson.size(); }  // lie about length to test the guard
    memcpy(out, extraJson.data(), extraJson.size());
    return extraJson.size();
  }
  void onResolved(const CastInfo &, bool s, Status st) override { resolved++; lastSuccess = s; lastStatus = st; }
};

struct World {
  LoopbackLink link;
  sessionless::Identity wandId, gwId;
  MemoryOrdinalStore wandOrd, gwOrd;
  MemoryUuidStore wandUuids, gwUuids;
  StrictLocalResolver resolver{epochNow};
  Handler handler;
  Listener listener;
  SpellDef spell{"wandCast", 50, true};
  GatewayConfig cfg;
  Gateway *gw;
  Wand *wand;

  World() {
    g_mono = 1000;
    wandId.ephemeral();
    gwId.ephemeral();
    cfg.spell = spell;
    gw = new Gateway(gwId, link.b(), resolver, gwOrd, gwUuids, monoFn, cfg, &handler);
    wand = new Wand(wandId, link.a(), wandOrd, wandUuids, monoFn, &listener);
  }
  ~World() { delete gw; delete wand; }

  void pump(int rounds = 3) { for (int i = 0; i < rounds; i++) { gw->poll(); wand->poll(); } }

  // Bring both up to "wand can cast".
  void bringUp() {
    gw->setEpoch(epochNow());
    CHECK_STATUS(gw->begin(), Status::Ok);
    pump();                                   // TimeSync -> wand
    CHECK(wand->timeSynced());
    CHECK_STATUS(wand->registerWithGateway(), Status::Ok);
    pump();                                   // request -> gateway -> resolver -> response -> wand
    CHECK(wand->registered());
  }
};

static void testHappyPath() {
  World w;
  CHECK_STATUS(w.gw->begin(), Status::NotSynced);   // no time yet: refuses, doesn't guess
  CHECK(!w.gw->ready());
  w.gw->setEpoch(epochNow());
  CHECK_STATUS(w.gw->begin(), Status::Ok);
  CHECK(w.gw->ready() && strlen(w.gw->uuid()) == 36);
  CHECK(w.resolver.registerCalls == 1);
  CHECK_STATUS(w.gw->begin(), Status::Ok);          // already registered: no second round trip
  CHECK(w.resolver.registerCalls == 1);

  CHECK(!w.wand->timeSynced());
  w.pump();
  CHECK(w.wand->timeSynced() && w.listener.synced >= 1);
  CHECK(w.wand->gateway() == LoopbackLink::idOfB());

  CHECK_STATUS(w.wand->registerWithGateway(), Status::Ok);
  w.pump();
  CHECK(w.wand->registered() && w.listener.registered == 1);
  CHECK(strlen(w.wand->casterUUID()) == 36 && strcmp(w.wand->casterUUID(), w.listener.uuid) == 0);
  CHECK(w.resolver.users.size() == 2);              // gateway + wand

  w.handler.extraJson = "{\"effect\":\"sparkle\"}";
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  CHECK(w.wand->castPending());
  w.pump();
  // The strict resolver verified the wand's signature AND the gateway's, through the real frames and JSON.
  CHECK(w.resolver.resolveCalls == 1);
  CHECK(w.handler.casts == 1 && w.handler.resolved == 1 && w.handler.lastSuccess);
  CHECK(w.listener.results == 1 && w.listener.lastSuccess);
  CHECK(!w.wand->castPending());
  CHECK(w.resolver.lastBody.find("\"extra\":{\"effect\":\"sparkle\"}") != std::string::npos);
  CHECK(w.resolver.lastBody.size() <= MAGIC_MAX_SPELL_BODY_BYTES);
  CHECK(w.resolver.find(w.wand->casterUUID())->mp == 950);

  // a second cast works and uses a higher ordinal (the resolver rejects repeats)
  g_mono += 50;
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  w.pump();
  CHECK(w.listener.results == 2 && w.listener.lastSuccess);
  CHECK(w.resolver.find(w.wand->casterUUID())->mp == 900);
  CHECK(w.link.dropped() == 0);
}

static void testPreconditionsBurnNoOrdinals() {
  World w;
  uint32_t o = 0;
  CHECK_STATUS(w.wand->cast(w.spell), Status::NotSynced);
  CHECK_STATUS(w.wand->registerWithGateway(), Status::NotSynced);
  w.gw->setEpoch(epochNow()); w.gw->begin(); w.pump();
  CHECK_STATUS(w.wand->cast(w.spell), Status::NotRegistered);
  w.wand->registerWithGateway(); w.pump();
  SpellDef bad = {"bad spell", 1, true};
  CHECK_STATUS(w.wand->cast(bad), Status::BadArgument);
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  CHECK_STATUS(w.wand->cast(w.spell), Status::Busy);   // one at a time
  OrdinalCounter peek(w.wandOrd);
  CHECK_STATUS(peek.next(&o), Status::Ok);
  CHECK(o == 2);  // exactly one ordinal was used by the single successful cast; failures burned none
}

static void testTimeout() {
  World w;
  w.bringUp();
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  g_mono += 9999; w.wand->poll();
  CHECK(w.listener.timeouts == 0 && w.wand->castPending());
  g_mono += 2;    w.wand->poll();                       // gateway never polled: no result ever arrives
  CHECK(w.listener.timeouts == 1 && !w.wand->castPending() && w.listener.results == 0);
  // registration timeout
  World r;
  r.gw->setEpoch(epochNow()); r.gw->begin(); r.pump();
  CHECK_STATUS(r.wand->registerWithGateway(), Status::Ok);
  g_mono += 10001; r.wand->poll();
  CHECK(r.listener.regFailed == 1 && !r.wand->registered());
}

static void testUnsolicitedAndSpoofedFrames() {
  World w;
  w.gw->setEpoch(epochNow()); w.gw->begin(); w.pump();
  // a RegisterResponse nobody asked for must not give the wand an identity
  RegisterResponse rr; memset(&rr, 0, sizeof(rr)); rr.success = true; strcpy(rr.casterUUID, "00000000-0000-4000-8000-0000000000ff");
  uint8_t buf[MAGIC_FRAME_MAX_SIZE];
  size_t n = encode(rr, buf, sizeof(buf));
  w.link.b().send(nullptr, buf, n);
  w.wand->poll();
  CHECK(!w.wand->registered() && w.listener.registered == 0);
  // an unsolicited CastResult with nothing pending is ignored
  CastResult cr = {true}; n = encode(cr, buf, sizeof(buf));
  w.link.b().send(nullptr, buf, n);
  w.wand->poll();
  CHECK(w.listener.results == 0);
  // garbage and wrong-size frames are ignored without incident
  uint8_t junk[7] = {3, 1, 2, 3, 4, 5, 6};
  w.link.b().send(nullptr, junk, sizeof(junk)); w.link.a().send(nullptr, junk, sizeof(junk));
  w.pump();
  CHECK(w.handler.casts == 0 && w.resolver.resolveCalls == 0);
}

static void testStaleAndTampered() {
  // wand clock 10 minutes behind the gateway: gateway refuses cheaply, no LED, no network round trip
  World w;
  w.bringUp();
  g_mono += 1;
  w.gw->setEpoch(epochNow() + 600000);              // gateway jumps ahead; wand still believes the old time
  int calls = w.resolver.resolveCalls;
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  w.gw->poll(); w.wand->poll();
  CHECK(w.handler.casts == 0);
  CHECK(w.resolver.resolveCalls == calls);
  CHECK(w.handler.resolved == 1 && !w.handler.lastSuccess && w.handler.lastStatus == Status::ClockSkew);
  CHECK(w.listener.results == 1 && !w.listener.lastSuccess);

  // a frame altered in transit: the strict resolver rejects it (the gateway can't verify casters itself)
  World t;
  t.bringUp();
  CHECK_STATUS(t.wand->cast(t.spell), Status::Ok);
  uint8_t frame[MAGIC_FRAME_MAX_SIZE]; size_t len; PeerId from;
  CHECK(t.link.b().receive(frame, sizeof(frame), &len, &from));
  frame[60] ^= 0x01;                                // flip a bit in the caster signature
  t.link.a().send(nullptr, frame, len);             // re-inject toward the gateway (loopback: a's frames go to b)
  t.gw->poll(); t.wand->poll();
  CHECK(t.handler.casts == 1);                      // onCast fires before resolution, by design
  CHECK(t.resolver.resolveCalls == 1 && t.handler.resolved == 1 && !t.handler.lastSuccess && t.handler.lastStatus == Status::Ok);
  CHECK(t.listener.results == 1 && !t.listener.lastSuccess);
  CHECK(t.resolver.find(t.wand->casterUUID())->mp == 1000);  // nothing was spent
}

static void testSpellMismatchRejected() {
  World w;
  w.bringUp();
  SpellDef expensive = {"wandCast", 500, true};     // wand signs a different cost than the gateway believes
  CHECK_STATUS(w.wand->cast(expensive), Status::Ok);
  w.pump();
  CHECK(w.listener.results == 1 && !w.listener.lastSuccess);
  CHECK(w.resolver.find(w.wand->casterUUID())->mp == 1000);
}

static void testExtraLimitsAbortTheCast() {
  struct Case { const char *name; std::string extra; Status want; };
  std::vector<Case> cases = {
    {"oversized", "{\"d\":\"" + std::string(130, 'x') + "\"}", Status::ExtraTooLarge},
    {"malformed", "{not json}", Status::BadExtra},
    {"not an object", "[1,2]", Status::BadExtra},
  };
  for (const Case &c : cases) {
    World w;
    w.bringUp();
    w.handler.extraJson = c.extra;
    CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
    w.pump();
    CHECK(w.resolver.resolveCalls == 0);            // never sent
    CHECK(w.handler.resolved == 1 && !w.handler.lastSuccess && w.handler.lastStatus == c.want);
    CHECK(w.listener.results == 1 && !w.listener.lastSuccess);
  }
  World ok;  // exactly 128 is fine
  ok.bringUp();
  ok.handler.extraJson = "{\"d\":\"" + std::string(128 - 8, 'x') + "\"}";
  CHECK(ok.handler.extraJson.size() == 128);
  ok.wand->cast(ok.spell); ok.pump();
  CHECK(ok.listener.lastSuccess);
}

static void testResolverDown() {
  World w;
  w.bringUp();
  w.resolver.mode = StrictLocalResolver::Unreachable;
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  w.pump();
  CHECK(w.handler.lastStatus == Status::ResolverUnreachable && !w.handler.lastSuccess);
  CHECK(w.listener.results == 1 && !w.listener.lastSuccess);  // the wand is told it fizzled, not left hanging
  w.resolver.mode = StrictLocalResolver::Normal;
  g_mono += 10;
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok);
  w.pump();
  CHECK(w.listener.lastSuccess);                              // recovers on its own

  World g;  // gateway can't register while the resolver is down, and does not pretend to be ready
  g.resolver.mode = StrictLocalResolver::Unreachable;
  g.gw->setEpoch(epochNow());
  CHECK_STATUS(g.gw->begin(), Status::ResolverUnreachable);
  CHECK(!g.gw->ready());
  g.pump();
  CHECK_STATUS(g.wand->registerWithGateway(), Status::Ok);
  g.pump();
  CHECK(!g.wand->registered());                               // wand registration refused too: nothing resolves
}

static void testRegistrationAbuse() {
  World w;
  w.gw->setEpoch(epochNow()); w.gw->begin(); w.pump();
  CHECK_STATUS(w.wand->registerWithGateway(), Status::Ok);
  uint8_t frame[MAGIC_FRAME_MAX_SIZE]; size_t len; PeerId from;
  CHECK(w.link.b().receive(frame, sizeof(frame), &len, &from));
  frame[50] ^= 0x01;                                          // corrupt the wand's registration signature
  w.link.a().send(nullptr, frame, len);
  int before = w.resolver.registerCalls;
  w.gw->poll(); w.wand->poll();
  CHECK(w.resolver.registerCalls == before);                  // gateway checked the signature itself, never asked the resolver
  CHECK(w.listener.regFailed == 1 && !w.wand->registered());
}

static void testWandResumesAfterReboot() {
  World w;
  w.bringUp();
  std::string uuid = w.wand->casterUUID();
  CHECK_STATUS(w.wand->cast(w.spell), Status::Ok); w.pump();
  CHECK(w.listener.lastSuccess);

  // "reboot": a new Wand over the same persisted stores
  Listener l2;
  Wand rebooted(w.wandId, w.link.a(), w.wandOrd, w.wandUuids, monoFn, &l2);
  CHECK(!rebooted.registered() && !rebooted.timeSynced());
  rebooted.pinGateway(LoopbackLink::idOfB());
  CHECK(rebooted.registered() && uuid == rebooted.casterUUID());   // identity restored from storage, no re-registration
  CHECK_STATUS(rebooted.cast(w.spell), Status::NotSynced);          // still needs real time
  g_mono += 30000;                                                  // gateway's next periodic TimeSync (interval is 30s)
  w.gw->poll(); rebooted.poll();
  CHECK(rebooted.timeSynced());
  CHECK_STATUS(rebooted.cast(w.spell), Status::Ok);
  w.gw->poll(); rebooted.poll(); rebooted.poll();
  CHECK(l2.results == 1 && l2.lastSuccess);                         // ordinal continued, so the resolver accepted it
  CHECK(w.resolver.find(uuid.c_str())->mp == 900);
}

// A Transport you script by hand, for frames the loopback pair can't produce (a third party in range).
class ScriptedTransport : public Transport {
 public:
  size_t maxFrameSize() const override { return 250; }
  Status send(const PeerId *peer, const uint8_t *frame, size_t len) override {
    sent.push_back(std::vector<uint8_t>(frame, frame + len));
    sentTo.push_back(peer ? *peer : PeerId());
    return Status::Ok;
  }
  bool receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) override {
    if (in.empty()) return false;
    auto f = in.front(); in.pop_front();
    if (f.second.size() > cap) return false;
    memcpy(buf, f.second.data(), f.second.size()); *len = f.second.size(); *from = f.first;
    return true;
  }
  void inject(const PeerId &from, const uint8_t *frame, size_t len) { in.push_back({from, std::vector<uint8_t>(frame, frame + len)}); }
  std::deque<std::pair<PeerId, std::vector<uint8_t>>> in;
  std::vector<std::vector<uint8_t>> sent;
  std::vector<PeerId> sentTo;
};

static void testPinningAndRogueGateway() {
  g_mono = 1000;
  sessionless::Identity id; id.ephemeral();
  MemoryOrdinalStore ord; MemoryUuidStore uuids; ScriptedTransport link; Listener l;
  Wand wand(id, link, ord, uuids, monoFn, &l);
  const uint8_t a[] = {1}, b[] = {2};
  PeerId good(a, 1), rogue(b, 1);
  uint8_t buf[MAGIC_FRAME_MAX_SIZE];
  TimeSync ts = {BASE}; size_t n = encode(ts, buf, sizeof(buf));

  wand.pinGateway(good);
  link.inject(rogue, buf, n);
  wand.poll();
  CHECK(!wand.timeSynced() && l.synced == 0);       // pinned: a stranger's TimeSync can't move our clock
  link.inject(good, buf, n);
  wand.poll();
  CHECK(wand.timeSynced() && wand.gateway() == good);

  // unpinned wand follows the latest TimeSync sender (documented, unauthenticated)
  ScriptedTransport link2; Listener l2; MemoryUuidStore u2;
  Wand follower(id, link2, ord, u2, monoFn, &l2);
  link2.inject(good, buf, n); follower.poll();
  CHECK(follower.gateway() == good);
  link2.inject(rogue, buf, n); follower.poll();
  CHECK(follower.gateway() == rogue);

  // registration goes to the learned gateway, unicast
  CHECK_STATUS(follower.registerWithGateway(), Status::Ok);
  CHECK(link2.sent.size() == 1 && link2.sentTo[0] == rogue && link2.sent[0][0] == MSG_REGISTER_REQUEST);
}

static void testTimeSyncCadence() {
  World w;
  w.gw->setEpoch(epochNow());
  w.gw->poll();
  uint8_t buf[MAGIC_FRAME_MAX_SIZE]; size_t len; PeerId from;
  int syncs = 0;
  auto drain = [&] { while (w.link.a().receive(buf, sizeof(buf), &len, &from)) if (peekType(buf, len) == MSG_TIME_SYNC) syncs++; };
  drain(); CHECK(syncs == 1);
  g_mono += 29999; w.gw->poll(); drain(); CHECK(syncs == 1);   // not yet
  g_mono += 1;     w.gw->poll(); drain(); CHECK(syncs == 2);   // interval elapsed
  // the epoch it announces tracks real time
  g_mono += 30000; w.gw->poll();
  CHECK(w.link.a().receive(buf, sizeof(buf), &len, &from));
  TimeSync ts; CHECK_STATUS(decode(buf, len, ts), Status::Ok);
  CHECK(ts.epochMs == epochNow());
}

static void testStaleTimeSync() {
  World w;
  CHECK(w.wand->timeSyncStale(120000));            // never synced counts as stale
  w.bringUp();
  CHECK(!w.wand->timeSyncStale(120000));
  g_mono += 119999; CHECK(!w.wand->timeSyncStale(120000));
  g_mono += 2;      CHECK(w.wand->timeSyncStale(120000));   // gateway went quiet: time to rescan for a new one
  w.pump();                                                  // gateway's periodic TimeSync arrives
  CHECK(!w.wand->timeSyncStale(120000));
}

static void testStoreFailuresAreLoud() {
  struct FailingUuids : UuidStore {
    Status load(const PeerId &, char *) override { return Status::NotFound; }
    Status save(const PeerId &, const char *) override { return Status::StoreError; }
  };
  World w;
  FailingUuids bad;
  Listener l;
  Wand wand(w.wandId, w.link.a(), w.wandOrd, bad, monoFn, &l);
  w.gw->setEpoch(epochNow()); w.gw->begin(); w.gw->poll(); wand.poll();
  CHECK_STATUS(wand.registerWithGateway(), Status::Ok);
  w.gw->poll(); wand.poll();
  CHECK(!wand.registered() && l.registered == 0 && l.regFailed == 1);   // an identity we can't persist is not "registered"
  Gateway gw2(w.gwId, w.link.b(), w.resolver, w.gwOrd, bad, monoFn, w.cfg);
  gw2.setEpoch(epochNow());
  CHECK_STATUS(gw2.begin(), Status::StoreError);
  CHECK(!gw2.ready());
}

int main() {
  printf("happy path\n");           testHappyPath();
  printf("preconditions\n");        testPreconditionsBurnNoOrdinals();
  printf("timeouts\n");             testTimeout();
  printf("unsolicited frames\n");   testUnsolicitedAndSpoofedFrames();
  printf("stale/tampered\n");       testStaleAndTampered();
  printf("spell mismatch\n");       testSpellMismatchRejected();
  printf("extra limits\n");         testExtraLimitsAbortTheCast();
  printf("resolver down\n");        testResolverDown();
  printf("registration abuse\n");   testRegistrationAbuse();
  printf("reboot resume\n");        testWandResumesAfterReboot();
  printf("pinning\n");              testPinningAndRogueGateway();
  printf("timesync cadence\n");     testTimeSyncCadence();
  printf("stale time sync\n");      testStaleTimeSync();
  printf("store failures\n");       testStoreFailuresAreLoud();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
