// A magic::Resolver that talks to fount (or anything with the same HTTP contract) using libcurl.
// For desktop/Linux/Raspberry Pi gateways and for tests. Link with -lcurl.
//
// Security defaults: the server certificate and host name are VERIFIED. Plain http:// is refused
// unless the host is loopback (localhost, 127.0.0.1, [::1]) or you set allowPlainHttp. Redirects are
// never followed (a redirect to http:// would silently downgrade TLS). Skipping certificate
// verification takes an explicit tlsVerification = TlsVerification::InsecureForDevelopment.
#ifndef MAGIC_CURL_RESOLVER_H
#define MAGIC_CURL_RESOLVER_H

#include <string>

#include "MagicCore.h"

namespace magic {

struct CurlResolverConfig {
  std::string baseUrl;                                   // e.g. "https://dev.fount.allyabase.com" (no trailing slash)
  TlsVerification tlsVerification = TlsVerification::Verify;
  std::string caBundlePath;                              // optional PEM bundle; empty = system trust store
  bool allowPlainHttp = false;                           // permit non-loopback http://
  long timeoutMs = 10000;
};

class CurlResolver : public Resolver {
 public:
  explicit CurlResolver(const CurlResolverConfig &config);

  // Ok if the configuration is usable (valid URL, https or permitted http). The calls below return
  // BadArgument for a configuration that fails this, before any network traffic.
  Status check() const;

  Status registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                      char uuidOut[MAGIC_FRAME_UUID_BYTES]) override;
  Status resolve(const ResolveRequest &request, ResolveResult *out) override;

  // For diagnostics: the HTTP status of the last request (0 if none got a response).
  long lastHttpStatus() const { return lastHttpStatus_; }

 private:
  Status request(const char *method, const std::string &path, const char *body, size_t bodyLen, std::string *response);

  CurlResolverConfig cfg_;
  long lastHttpStatus_ = 0;
};

}  // namespace magic

#endif
