// Certificate checking is on by default, and the opt-out has to be explicit. Run via ../e2e/tls-test.sh.
#include <stdio.h>
#include <string>

#include "MagicCurlResolver.h"

using namespace magic;
static int failures = 0;
#define EXPECT(cond) do { if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static Status reg(CurlResolver &r) {
  char uuid[MAGIC_FRAME_UUID_BYTES];
  return r.registerUser(1700000000000ULL, std::string(66, 'a').c_str(), std::string(128, 'b').c_str(), uuid);
}

int main(int argc, char **argv) {
  if (argc < 3) { printf("usage: tls_test <port> <ca.pem>\n"); return 2; }
  std::string port = argv[1], ca = argv[2];

  // 1. DEFAULT config against a self-signed server: refused, no request ever reaches the server.
  { CurlResolverConfig c; c.baseUrl = "https://localhost:" + port;
    CurlResolver r(c);
    EXPECT(r.check() == Status::Ok);
    EXPECT(reg(r) == Status::ResolverUnreachable); }

  // 2. Trusting that exact certificate (a CA bundle) works, so (1) failed because of verification.
  { CurlResolverConfig c; c.baseUrl = "https://localhost:" + port; c.caBundlePath = ca;
    CurlResolver r(c);
    EXPECT(reg(r) == Status::Ok); }

  // 3. Right certificate, wrong host name: refused (host name verification is on).
  { CurlResolverConfig c; c.baseUrl = "https://127.0.0.1:" + port; c.caBundlePath = ca;
    CurlResolver r(c);
    EXPECT(reg(r) == Status::ResolverUnreachable); }

  // 4. The explicit opt-out works, and it is the ONLY way to skip verification.
  { CurlResolverConfig c; c.baseUrl = "https://localhost:" + port; c.tlsVerification = TlsVerification::InsecureForDevelopment;
    CurlResolver r(c);
    EXPECT(reg(r) == Status::Ok); }

  // 5. URL policy, checked before any traffic.
  struct { const char *url; bool allowHttp; Status want; } urls[] = {
    {"https://dev.fount.allyabase.com", false, Status::Ok},
    {"http://localhost:3006", false, Status::Ok},
    {"http://127.0.0.1:3006", false, Status::Ok},
    {"http://[::1]:3006", false, Status::Ok},
    {"http://fount.example.com", false, Status::BadArgument},        // plaintext to a real host
    {"http://fount.example.com", true, Status::Ok},                  // ...unless explicitly allowed
    {"http://localhost@evil.example.com", false, Status::BadArgument},  // userinfo trick
    {"http://localhost.evil.example.com", false, Status::BadArgument},  // look-alike host
    {"ftp://localhost", false, Status::BadArgument},
    {"localhost:3006", false, Status::BadArgument},
    {"", false, Status::BadArgument},
  };
  for (auto &u : urls) {
    CurlResolverConfig c; c.baseUrl = u.url; c.allowPlainHttp = u.allowHttp;
    CurlResolver r(c);
    if (r.check() != u.want) { failures++; printf("FAIL url policy: '%s' allowHttp=%d\n", u.url, u.allowHttp); }
  }
  { CurlResolverConfig c; c.baseUrl = "http://fount.example.com";  // refused before any network traffic
    CurlResolver r(c);
    EXPECT(reg(r) == Status::BadArgument); }

  printf(failures ? "TLS FAILED (%d)\n" : "TLS OK\n", failures);
  return failures ? 1 : 0;
}
