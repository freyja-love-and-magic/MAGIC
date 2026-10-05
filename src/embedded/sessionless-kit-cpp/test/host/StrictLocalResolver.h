// A real, in-process magic::Resolver for tests. It is not a stub: it enforces what a correct resolver
// must (and what fount's resolve() currently does NOT, see MagicCore README): it verifies the caster's
// and every gateway's signature, rejects replayed ordinals and stale timestamps, enforces the 128/2048
// limits, and spends MP. It reads the structured request fields, so it needs no JSON parser.
#ifndef STRICT_LOCAL_RESOLVER_H
#define STRICT_LOCAL_RESOLVER_H

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "MagicCore.h"

class StrictLocalResolver : public magic::Resolver {
 public:
  enum Mode { Normal, Unreachable };

  struct User { std::string uuid, pubKey; uint64_t lastOrdinal = 0; int64_t mp = 1000; };

  explicit StrictLocalResolver(uint64_t (*now)()) : now_(now) {}

  magic::Status registerUser(uint64_t timestamp, const char *pubKeyHex, const char *signatureHex,
                             char uuidOut[MAGIC_FRAME_UUID_BYTES]) override {
    registerCalls++;
    if (mode == Unreachable) return magic::Status::ResolverUnreachable;
    if (!fresh(timestamp)) return magic::Status::ResolverRejected;
    std::string message = std::to_string(timestamp) + pubKeyHex;
    if (!sessionless::verifyHex(signatureHex, message.c_str(), pubKeyHex)) return magic::Status::ResolverRejected;
    for (const User &u : users) {
      if (u.pubKey == pubKeyHex) { strcpy(uuidOut, u.uuid.c_str()); return magic::Status::Ok; }  // idempotent, like fount
    }
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "00000000-0000-4000-8000-%012zu", users.size() + 1);
    User u; u.uuid = uuid; u.pubKey = pubKeyHex;
    users.push_back(u);
    strcpy(uuidOut, uuid);
    return magic::Status::Ok;
  }

  magic::Status resolve(const magic::ResolveRequest &req, magic::ResolveResult *out) override {
    resolveCalls++;
    out->success = false;
    if (mode == Unreachable) return magic::Status::ResolverUnreachable;
    lastBody.assign(req.body, req.bodyLen);

    // The body and the structured fields must describe the same spell.
    magic::JsonField f;
    char tmp[80];
    if (magic::jsonGetTopLevel(req.body, req.bodyLen, "spell", &f) != magic::Status::Ok || !magic::jsonCopyString(f, tmp, sizeof(tmp)) || strcmp(tmp, req.spell) != 0) return magic::Status::BadResponse;
    if (req.bodyLen > MAGIC_MAX_SPELL_BODY_BYTES) return magic::Status::Ok;  // refuse
    for (size_t i = 0; i < req.gatewayCount; i++)
      if (req.gateways[i].extraJson && strlen(req.gateways[i].extraJson) > MAGIC_MAX_GATEWAY_EXTRA_BYTES) return magic::Status::Ok;

    const magic::SpellFields &s = *req.spellFields;
    User *caster = find(s.casterUUID);
    if (!caster || !fresh(s.timestamp)) return magic::Status::Ok;
    char msg[MAGIC_MESSAGE_BUFFER_SIZE];
    size_t n = magic::casterMessage(msg, sizeof(msg), s.timestamp, s.spell, s.casterUUID, s.totalCost, s.mp, s.ordinal);
    if (!n || !sessionless::verifyHex(s.casterSignatureHex, msg, caster->pubKey.c_str())) return magic::Status::Ok;
    for (size_t i = 0; i < req.gatewayCount; i++) {
      const magic::GatewayFields &g = req.gateways[i];
      User *gw = find(g.uuid);
      n = magic::gatewayMessage(msg, sizeof(msg), g.timestamp, g.uuid, g.minimumCost, g.ordinal);
      if (!gw || !n || !fresh(g.timestamp) || !sessionless::verifyHex(g.signatureHex, msg, gw->pubKey.c_str())) return magic::Status::Ok;
    }
    if (s.ordinal <= caster->lastOrdinal) return magic::Status::Ok;  // replay
    if (!s.mp || caster->mp < (int64_t)s.totalCost) return magic::Status::Ok;
    caster->mp -= (int64_t)s.totalCost;
    caster->lastOrdinal = s.ordinal;
    out->success = true;
    return magic::Status::Ok;
  }

  User *find(const char *uuid) {
    for (User &u : users) if (u.uuid == uuid) return &u;
    return nullptr;
  }

  Mode mode = Normal;
  int registerCalls = 0, resolveCalls = 0;
  std::string lastBody;
  std::vector<User> users;

 private:
  bool fresh(uint64_t ts) const {
    uint64_t now = now_();
    return (ts > now ? ts - now : now - ts) <= 300000;
  }
  uint64_t (*now_)();
};

#endif
