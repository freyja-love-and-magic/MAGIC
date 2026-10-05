#include "MagicCurlResolver.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

namespace magic {

namespace {
const size_t kMaxResponseBytes = 8192;

struct Sink {
  std::string *data;
  bool overflow;
};

size_t onBody(char *chunk, size_t size, size_t n, void *user) {
  Sink *sink = static_cast<Sink *>(user);
  size_t bytes = size * n;
  if (sink->data->size() + bytes > kMaxResponseBytes) {
    sink->overflow = true;
    return 0;  // abort the transfer: never truncate a reply and then parse half of it
  }
  sink->data->append(chunk, bytes);
  return bytes;
}

void ensureCurlInit() {
  static bool done = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
  (void)done;
}

}  // namespace

CurlResolver::CurlResolver(const CurlResolverConfig &config) : cfg_(config) {
  while (!cfg_.baseUrl.empty() && cfg_.baseUrl.back() == '/') cfg_.baseUrl.pop_back();
}

Status CurlResolver::check() const {
  return checkResolverUrl(cfg_.baseUrl.c_str(), cfg_.allowPlainHttp);
}

Status CurlResolver::request(const char *method, const std::string &path, const char *body, size_t bodyLen, std::string *response) {
  lastHttpStatus_ = 0;
  if (check() != Status::Ok) return Status::BadArgument;
  ensureCurlInit();
  CURL *curl = curl_easy_init();
  if (!curl) return Status::ResolverUnreachable;

  std::string url = cfg_.baseUrl + path;
  Sink sink = {response, false};
  struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyLen);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, cfg_.timeoutMs);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (cfg_.tlsVerification == TlsVerification::InsecureForDevelopment) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  } else {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cfg_.caBundlePath.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, cfg_.caBundlePath.c_str());
  }

  CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (sink.overflow) return Status::BadResponse;
  if (rc != CURLE_OK) return Status::ResolverUnreachable;  // DNS, connect, TLS verification failure, timeout...
  lastHttpStatus_ = http;
  return Status::Ok;
}

Status CurlResolver::registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                                  char uuidOut[MAGIC_FRAME_UUID_BYTES]) {
  char body[320];
  size_t n = 0;
  Status s = buildRegisterBody(body, sizeof(body), &n, timestamp, pubKeyHex, signatureHex);
  if (s != Status::Ok) return s;
  std::string response;
  s = request("PUT", "/user/create", body, n, &response);  // fount registers this as PUT, not POST
  if (s != Status::Ok) return s;
  return parseRegisterResponse((int)lastHttpStatus_, response.c_str(), response.size(), uuidOut);
}

Status CurlResolver::resolve(const ResolveRequest &req, ResolveResult *out) {
  out->success = false;
  if (!isSafeId(req.spell, MAGIC_SPELL_NAME_MAX_CHARS)) return Status::BadArgument;
  std::string response;
  Status s = request("POST", std::string("/resolve/") + req.spell, req.body, req.bodyLen, &response);
  if (s != Status::Ok) return s;
  return parseResolveResponse((int)lastHttpStatus_, response.c_str(), response.size(), out);
}

}  // namespace magic
