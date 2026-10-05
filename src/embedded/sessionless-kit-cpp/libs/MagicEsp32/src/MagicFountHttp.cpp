#if defined(ARDUINO_ARCH_ESP32)

#include "MagicFountHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <string.h>

#include <string>

namespace magic {

namespace {
const size_t kMaxResponseBytes = 4096;

// Collects a response up to a fixed size and flags overflow instead of truncating.
class BoundedSink : public Stream {
 public:
  BoundedSink(char *buf, size_t cap) : buf_(buf), cap_(cap) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t n) override {
    if (len_ + n >= cap_) { overflow_ = true; return n; }  // swallow the rest; the caller sees overflow
    memcpy(buf_ + len_, data, n);
    len_ += n;
    return n;
  }
  int available() override { return 0; }  // write-only; HTTPClient::writeToStream needs a Stream
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t length() const { return len_; }
  bool overflow() const { return overflow_; }
 private:
  char *buf_;
  size_t cap_, len_ = 0;
  bool overflow_ = false;
};
}  // namespace

Status FountHttpResolver::check() const {
  bool https = false;
  Status s = checkResolverUrl(cfg_.baseUrl, cfg_.allowPlainHttp, &https);
  if (s != Status::Ok) return s;
  // Verifying TLS needs a trust anchor, and a device has none built in. Refuse rather than skip verification.
  if (https && cfg_.tlsVerification == TlsVerification::Verify && (!cfg_.rootCaPem || !cfg_.rootCaPem[0])) return Status::BadArgument;
  return Status::Ok;
}

Status FountHttpResolver::request(const char *method, const char *path, const char *body, size_t bodyLen, char *response,
                                  size_t cap, size_t *responseLen) {
  lastHttpStatus_ = 0;
  *responseLen = 0;
  bool https = false;
  if (check() != Status::Ok) return Status::BadArgument;
  checkResolverUrl(cfg_.baseUrl, cfg_.allowPlainHttp, &https);

  std::string url = cfg_.baseUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();  // tolerate a trailing slash on baseUrl
  url += path;

  HTTPClient http;
  http.setTimeout(cfg_.timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // a redirect to http:// would silently downgrade TLS
  http.addHeader("Content-Type", "application/json");

  WiFiClientSecure secure;
  WiFiClient plain;
  bool began;
  if (https) {
    if (cfg_.tlsVerification == TlsVerification::InsecureForDevelopment) secure.setInsecure();
    else secure.setCACert(cfg_.rootCaPem);
    began = http.begin(secure, url.c_str());
  } else {
    began = http.begin(plain, url.c_str());
  }
  if (!began) return Status::ResolverUnreachable;

  int code = http.sendRequest(method, (uint8_t *)body, bodyLen);
  if (code <= 0) {  // negative = client-side failure: DNS, connect, TLS verification failure, timeout
    http.end();
    return Status::ResolverUnreachable;
  }
  BoundedSink sink(response, cap);
  http.writeToStream(&sink);  // handles Content-Length and chunked encoding
  http.end();
  if (sink.overflow()) return Status::BadResponse;
  response[sink.length()] = '\0';
  *responseLen = sink.length();
  lastHttpStatus_ = code;
  return Status::Ok;
}

Status FountHttpResolver::registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                                       char uuidOut[MAGIC_FRAME_UUID_BYTES]) {
  char body[320];
  size_t n = 0;
  Status s = buildRegisterBody(body, sizeof(body), &n, timestamp, pubKeyHex, signatureHex);
  if (s != Status::Ok) return s;
  char response[kMaxResponseBytes + 1];
  size_t rn = 0;
  s = request("PUT", "/user/create", body, n, response, sizeof(response), &rn);  // fount registers this as PUT, not POST
  if (s != Status::Ok) return s;
  return parseRegisterResponse(lastHttpStatus_, response, rn, uuidOut);
}

Status FountHttpResolver::resolve(const ResolveRequest &req, ResolveResult *out) {
  out->success = false;
  if (!isSafeId(req.spell, MAGIC_SPELL_NAME_MAX_CHARS)) return Status::BadArgument;
  std::string path = std::string("/resolve/") + req.spell;
  char response[kMaxResponseBytes + 1];
  size_t rn = 0;
  Status s = request("POST", path.c_str(), req.body, req.bodyLen, response, sizeof(response), &rn);
  if (s != Status::Ok) return s;
  return parseResolveResponse(lastHttpStatus_, response, rn, out);
}

}  // namespace magic

#endif
