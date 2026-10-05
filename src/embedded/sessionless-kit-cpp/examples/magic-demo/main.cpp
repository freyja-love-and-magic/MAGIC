// Builds and signs a complete cast (as a wand would) and a gateway hop, entirely on the device, then
// prints the exact JSON body a gateway would POST to the resolver. No radio, WiFi, or network.
// Shows the size limits in action. Serial monitor at 115200.
#include <Arduino.h>
#include <WiFi.h>

#include <MagicCore.h>
#include <MagicNvs.h>
#include <SessionlessKit.h>
#include <SessionlessNvs.h>

SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);

Preferences prefs;

void halt(const char *what, const char *why) {
  while (true) {
    Serial.printf("%s failed: %s\n", what, why);
    delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  WiFi.mode(WIFI_STA);  // radio on => esp_random() is a true random source

  prefs.begin("magicdemo", false);
  sessionless::NvsKeyStore store(prefs);
  sessionless::Identity me;
  sessionless::Status ks = me.loadOrCreate(store);
  if (ks != sessionless::Status::Ok) halt("keys", sessionless::statusName(ks));

  magic::NvsOrdinalStore ordinalStore(prefs);
  magic::OrdinalCounter counter(ordinalStore);
  uint32_t ordinal;
  magic::Status s = counter.next(&ordinal);
  if (s != magic::Status::Ok) halt("ordinal", magic::statusName(s));

  // A real device gets epoch time from the gateway's TimeSync (or NTP). This demo has neither,
  // so it uses an obviously fixed timestamp rather than inventing "now".
  magic::EpochClock clock;
  clock.sync(1700000000000ULL, esp_timer_get_time() / 1000);
  uint64_t now;
  s = clock.now(esp_timer_get_time() / 1000, &now);
  if (s != magic::Status::Ok) halt("clock", magic::statusName(s));

  const char *casterUUID = "123e4567-e89b-42d3-a456-426614174000";  // would come from registration
  const char *gatewayUUID = "9b2f6c1e-3d4a-4f8b-9c7e-0a1b2c3d4e5f";
  char casterSig[SESSIONLESS_SIGNATURE_HEX_LEN], gatewaySig[SESSIONLESS_SIGNATURE_HEX_LEN];
  s = magic::signCaster(me, now, "wandCast", casterUUID, 50, true, ordinal, casterSig);
  if (s != magic::Status::Ok) halt("signCaster", magic::statusName(s));
  s = magic::signGateway(me, now, gatewayUUID, 0, ordinal, gatewaySig);  // same key for the demo; real gateways have their own
  if (s != magic::Status::Ok) halt("signGateway", magic::statusName(s));

  magic::SpellFields spell = {now, "wandCast", casterUUID, 50, true, ordinal, casterSig};
  magic::GatewayFields gateway = {now, gatewayUUID, 0, ordinal, gatewaySig, "{\"effect\":\"sparkle\"}"};

  static char body[MAGIC_MAX_SPELL_BODY_BYTES + 1];
  size_t len = 0;
  s = magic::writeSpellJson(body, sizeof(body), &len, spell, &gateway, 1);
  if (s != magic::Status::Ok) halt("writeSpellJson", magic::statusName(s));
  Serial.printf("ordinal %u, body %u of %d bytes:\n%s\n", ordinal, (unsigned)len, MAGIC_MAX_SPELL_BODY_BYTES, body);

  // The limits are enforced, not advisory:
  gateway.extraJson = "{\"d\":\"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789\"}";
  Serial.printf("oversized extra  -> %s\n", magic::statusName(magic::writeSpellJson(body, sizeof(body), &len, spell, &gateway, 1)));
  gateway.extraJson = "{not json}";
  Serial.printf("malformed extra  -> %s\n", magic::statusName(magic::writeSpellJson(body, sizeof(body), &len, spell, &gateway, 1)));
}

void loop() {}
