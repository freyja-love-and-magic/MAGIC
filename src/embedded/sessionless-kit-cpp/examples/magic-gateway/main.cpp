// A complete gateway: joins WiFi, gets real time over NTP, registers with fount, serves wands over ESP-NOW,
// and reacts when a spell lands. Edit the CONFIG block, then watch the serial monitor.
//   Libraries: SessionlessKit, MagicCore, MagicGateway, MagicEsp32.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <sys/time.h>

#include <MagicEspNow.h>
#include <MagicEspStores.h>
#include <MagicFountHttp.h>
#include <MagicGateway.h>
#include <MagicNvs.h>
#include <SessionlessNvs.h>

SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);  // secp256k1 and TLS both want a big stack

// ---------------------------- CONFIG ---------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-name"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-wifi-password"
#endif
#ifndef FOUNT_URL
#define FOUNT_URL "https://dev.fount.allyabase.com"  // your resolver; https unless you opt out below
#endif
// TLS is VERIFIED: paste the PEM of the root certificate your fount's certificate chains to.
// (Get it with: openssl s_client -showcerts -connect HOST:443 and take the last certificate, or from your CA.)
// Leaving this placeholder makes the first request fail with a TLS error. That is the safe failure.
#ifndef FOUNT_ROOT_CA_PEM
#warning "FOUNT_ROOT_CA_PEM is a placeholder: set it to your fount's root CA certificate (PEM) or TLS will fail"
#define FOUNT_ROOT_CA_PEM "-----BEGIN CERTIFICATE-----\nREPLACE-ME\n-----END CERTIFICATE-----\n"
#endif
static const magic::SpellDef SPELL = {"wandCast", 50, true};  // must match the wands'
static const int LED_PIN = 2;                                  // any output pin you want to flash
// ---------------------------------------------------------------------------

static uint64_t monotonicMs() { return esp_timer_get_time() / 1000; }

static uint64_t epochMsFromNtp() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// What the gateway DOES when a spell arrives. Swap this for a servo, a display, a relay...
struct Reactions : magic::GatewayHandler {
  void onCast(const magic::CastInfo &c) override { Serial.printf("cast from %s ordinal %u\n", c.casterUUID, c.ordinal); }
  // Optional: attach up to 128 bytes of JSON to the spell. Must be a JSON object.
  size_t extra(const magic::CastInfo &, char *out, size_t cap) override {
    const char *json = "{\"lit\":true}";
    size_t n = strlen(json);
    if (n > cap) return 0;
    memcpy(out, json, n);
    return n;
  }
  void onResolved(const magic::CastInfo &, bool ok, magic::Status s) override {
    Serial.printf("resolved: %s (%s)\n", ok ? "success" : "no", magic::statusName(s));
    if (ok) { digitalWrite(LED_PIN, HIGH); delay(400); digitalWrite(LED_PIN, LOW); }
  }
};

Preferences prefs;
sessionless::NvsKeyStore keyStore(prefs);
sessionless::Identity identity;
magic::NvsOrdinalStore ordinals(prefs);   // NOTE: one flash write per cast; fine for demos, mind wear on a busy gateway
magic::NvsUuidStore uuids(prefs);
magic::EspNowTransport radio;
magic::FountHttpConfig fountConfig;
magic::FountHttpResolver resolver(fountConfig);  // configured in setup(); it reads the config by copy
Reactions reactions;
magic::GatewayConfig gatewayConfig;
magic::Gateway *gateway = nullptr;

static void halt(const char *what, const char *why) {
  while (true) { Serial.printf("%s failed: %s\n", what, why); delay(2000); }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(LED_PIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
  Serial.printf("\nWiFi up on channel %d\n", WiFi.channel());

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (epochMsFromNtp() < 1700000000000ULL) { delay(250); }  // NTP not set yet: the clock still says 1970
  Serial.println("time synced over NTP");

  prefs.begin("gateway", false);
  sessionless::Status ks = identity.loadOrCreate(keyStore);
  if (ks != sessionless::Status::Ok) halt("keys", sessionless::statusName(ks));

  magic::Status rs = radio.begin();  // after WiFi connects, so ESP-NOW follows the router's channel
  if (rs != magic::Status::Ok) halt("radio", magic::statusName(rs));

  fountConfig.baseUrl = FOUNT_URL;
  fountConfig.rootCaPem = FOUNT_ROOT_CA_PEM;
  resolver = magic::FountHttpResolver(fountConfig);
  gatewayConfig.spell = SPELL;
  gateway = new magic::Gateway(identity, radio, resolver, ordinals, uuids, monotonicMs, gatewayConfig, &reactions);

  gateway->setEpoch(epochMsFromNtp());
  magic::Status bs;
  while ((bs = gateway->begin()) != magic::Status::Ok) {  // registers with fount once, remembers the UUID in flash
    Serial.printf("gateway begin: %s, retrying\n", magic::statusName(bs));
    delay(3000);
    gateway->setEpoch(epochMsFromNtp());
  }
  Serial.printf("gateway ready, uuid %s\n", gateway->uuid());
}

void loop() {
  static uint64_t lastResync = 0;
  uint64_t now = monotonicMs();
  if (now - lastResync > 600000) {  // re-read NTP every 10 minutes to correct drift
    lastResync = now;
    gateway->setEpoch(epochMsFromNtp());
  }
  gateway->poll();
  delay(2);
}
