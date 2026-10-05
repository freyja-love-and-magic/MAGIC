// A complete wand: press the BOOT button to cast. About 60 lines of application code; everything else is the packages.
//   Libraries: SessionlessKit (keys), MagicCore, MagicWand, MagicEsp32 (ESP-NOW + flash storage).
// Needs a gateway in range (see ../magic-gateway). The spell must match the gateway's configuration.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>

#include <MagicEspNow.h>
#include <MagicEspStores.h>
#include <MagicNvs.h>
#include <MagicWand.h>
#include <SessionlessNvs.h>

SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);  // libsecp256k1 needs the room (see SessionlessKit.h)

static const magic::SpellDef SPELL = {"wandCast", 50, true};  // name, MP cost, spends MP
static const int BUTTON_PIN = 0;                              // BOOT button on most ESP32-S3 boards

static uint64_t monotonicMs() { return esp_timer_get_time() / 1000; }  // 64-bit, unlike millis()

struct Reactions : magic::WandListener {
  void onTimeSynced(uint64_t) override { Serial.println("gateway found, time synced"); }
  void onRegistered(const char *uuid) override { Serial.printf("registered as %s\n", uuid); }
  void onRegistrationFailed() override { Serial.println("registration failed, will retry"); }
  void onResult(bool ok) override { Serial.println(ok ? "spell resolved" : "spell fizzled"); }
  void onTimeout() override { Serial.println("no answer from the gateway"); }
};

Preferences prefs;
sessionless::NvsKeyStore keyStore(prefs);
sessionless::Identity identity;
magic::NvsOrdinalStore ordinals(prefs);
magic::NvsUuidStore uuids(prefs);
magic::EspNowTransport radio;
Reactions reactions;
magic::Wand wand(identity, radio, ordinals, uuids, monotonicMs, &reactions);

static void halt(const char *what, const char *why) {
  while (true) { Serial.printf("%s failed: %s\n", what, why); delay(2000); }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);   // radio on (also makes esp_random() a true random source)
  WiFi.disconnect();     // a wand never joins a network; it only talks ESP-NOW

  prefs.begin("wand", false);
  sessionless::Status ks = identity.loadOrCreate(keyStore);
  if (ks != sessionless::Status::Ok) halt("keys", sessionless::statusName(ks));
  magic::Status rs = radio.begin();
  if (rs != magic::Status::Ok) halt("radio", magic::statusName(rs));
  Serial.println("wand ready, scanning for a gateway...");
}

void loop() {
  static uint64_t lastRegisterTry = 0, lastPress = 0;
  uint64_t now = monotonicMs();

  radio.scanForGateway(!wand.timeSyncStale(120000), now);  // hunt for a gateway until one speaks; resume if it goes quiet
  wand.poll();

  if (wand.timeSynced() && !wand.registered() && now - lastRegisterTry > 5000) {
    lastRegisterTry = now;
    wand.registerWithGateway();
  }

  if (digitalRead(BUTTON_PIN) == LOW && now - lastPress > 300) {
    lastPress = now;
    magic::Status s = wand.cast(SPELL);
    if (s != magic::Status::Ok) Serial.printf("can't cast yet: %s\n", magic::statusName(s));
  }
  delay(5);
}
