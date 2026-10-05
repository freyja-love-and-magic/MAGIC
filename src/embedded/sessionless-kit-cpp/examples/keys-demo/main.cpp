// The whole workflow in one sketch: keys persist in flash across resets, then sign and verify.
// Open the serial monitor at 115200. Reset the board: the public key stays the same.
#include <Arduino.h>
#include <WiFi.h>

#include <SessionlessKit.h>
#include <SessionlessNvs.h>

SET_LOOP_TASK_STACK_SIZE(SESSIONLESS_STACK_BYTES);  // libsecp256k1 needs the room; see SessionlessKit.h

sessionless::NvsKeyStore store("keysdemo");
sessionless::Identity me;

void fail(const char *what, sessionless::Status s) {
  while (true) {
    Serial.printf("%s failed: %s\n", what, sessionless::statusName(s));
    delay(2000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  // esp_random() is only a true random source while the radio is on (see SessionlessKit.h).
  WiFi.mode(WIFI_STA);

  sessionless::Status s = me.loadOrCreate(store);
  if (s != sessionless::Status::Ok) fail("loadOrCreate", s);

  char pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
  me.publicKeyHex(pub);
  Serial.printf("public key: %s\n", pub);

  const char *message = "hello from the wand";
  char sig[SESSIONLESS_SIGNATURE_HEX_LEN];
  s = me.signHex(message, sig);
  if (s != sessionless::Status::Ok) fail("sign", s);
  Serial.printf("signature:  %s\n", sig);

  Serial.printf("verifies:   %s\n", sessionless::verifyHex(sig, message, pub) ? "yes" : "NO");
  Serial.printf("tampered:   %s\n", sessionless::verifyHex(sig, "hello from the wanD", pub) ? "YES (bad!)" : "rejected");
}

void loop() {}
