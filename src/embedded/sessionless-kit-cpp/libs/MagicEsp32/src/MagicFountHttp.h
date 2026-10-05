// magic::Resolver over HTTPS to fount, using the Arduino-ESP32 HTTPClient. Call only while WiFi is connected.
//
// TLS is verified by default. An ESP32 has no system trust store, so you must supply the PEM of the
// root (or pinned server) certificate: config.rootCaPem. With none, every call fails with BadArgument
// instead of quietly skipping verification. Skipping verification takes
// tlsVerification = TlsVerification::InsecureForDevelopment. The original demos did this (setInsecure()
// "for the weekend"); a package people install must not.
#ifndef MAGIC_FOUNT_HTTP_H
#define MAGIC_FOUNT_HTTP_H

#if !defined(ARDUINO_ARCH_ESP32)
#error "MagicFountHttp.h needs the ESP32 Arduino core."
#endif

#include "MagicCore.h"

namespace magic {

struct FountHttpConfig {
  const char *baseUrl = nullptr;    // e.g. "https://dev.fount.allyabase.com" (no trailing slash)
  const char *rootCaPem = nullptr;  // PEM text of the trusted root/server certificate; must outlive the resolver
  TlsVerification tlsVerification = TlsVerification::Verify;
  bool allowPlainHttp = false;      // permit non-loopback http:// (a device is never loopback, so this is what http:// needs)
  uint32_t timeoutMs = 10000;
};

class FountHttpResolver : public Resolver {
 public:
  explicit FountHttpResolver(const FountHttpConfig &config) : cfg_(config) {}
  Status check() const;
  Status registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                      char uuidOut[MAGIC_FRAME_UUID_BYTES]) override;
  Status resolve(const ResolveRequest &request, ResolveResult *out) override;
  int lastHttpStatus() const { return lastHttpStatus_; }

 private:
  Status request(const char *method, const char *path, const char *body, size_t bodyLen, char *response, size_t cap, size_t *responseLen);
  FountHttpConfig cfg_;
  int lastHttpStatus_ = 0;
};

}  // namespace magic

#endif
