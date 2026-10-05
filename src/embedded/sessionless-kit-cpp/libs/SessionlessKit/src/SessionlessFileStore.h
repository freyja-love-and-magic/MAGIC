// File-backed KeyStore for hosts (macOS/Linux): tests, simulators, gateways on a Pi.
// One file containing the private key as 64 hex chars and the public key as 66, one per line, mode 0600.
#ifndef SESSIONLESS_FILE_STORE_H
#define SESSIONLESS_FILE_STORE_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "SessionlessKit.h"

namespace sessionless {

class FileKeyStore : public KeyStore {
 public:
  explicit FileKeyStore(std::string path) : path_(std::move(path)) {}

  Status load(uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE],
              bool &hasPublicKey) override {
    FILE *f = fopen(path_.c_str(), "r");
    if (!f) return errno == ENOENT ? Status::NoKeys : Status::StoreError;
    char priv[SESSIONLESS_PRIVATE_KEY_HEX_LEN + 2] = {0};
    char pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN + 2] = {0};
    bool gotPriv = fgets(priv, sizeof(priv), f) != nullptr;
    bool gotPub = fgets(pub, sizeof(pub), f) != nullptr;
    fclose(f);
    trim(priv);
    trim(pub);
    Status s = Status::Ok;
    if (!gotPriv || !fromHex(priv, privateKey, SESSIONLESS_PRIVATE_KEY_SIZE)) s = Status::CorruptKeys;
    hasPublicKey = false;
    if (s == Status::Ok && gotPub) {
      if (!fromHex(pub, publicKey, SESSIONLESS_PUBLIC_KEY_SIZE)) s = Status::CorruptKeys;
      else hasPublicKey = true;
    }
    memset(priv, 0, sizeof(priv));
    return s;
  }

  Status save(const uint8_t privateKey[SESSIONLESS_PRIVATE_KEY_SIZE], const uint8_t publicKey[SESSIONLESS_PUBLIC_KEY_SIZE]) override {
    std::string tmp = path_ + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return Status::StoreError;
    char priv[SESSIONLESS_PRIVATE_KEY_HEX_LEN], pub[SESSIONLESS_PUBLIC_KEY_HEX_LEN];
    toHex(privateKey, SESSIONLESS_PRIVATE_KEY_SIZE, priv);
    toHex(publicKey, SESSIONLESS_PUBLIC_KEY_SIZE, pub);
    std::string body = std::string(priv) + "\n" + pub + "\n";
    memset(priv, 0, sizeof(priv));
    bool ok = write(fd, body.data(), body.size()) == (ssize_t)body.size() && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(tmp.c_str(), path_.c_str()) != 0) {
      unlink(tmp.c_str());
      return Status::StoreError;
    }
    return Status::Ok;
  }

 private:
  static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
  }
  std::string path_;
};

}  // namespace sessionless

#endif
