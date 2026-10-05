// magic::Transport over ESP-NOW (Arduino-ESP32 core). One instance per device: ESP-NOW has a single
// global receive callback.
//
// What the original wand/target demos learned the hard way, now built in:
//  * esp_wifi_set_channel() on an unassociated radio only takes effect with promiscuous mode toggled
//    around it (setChannel does this).
//  * A wand doesn't know the gateway's channel: scanForGateway() sweeps channels 1..13 until time sync
//    arrives, and resumes sweeping if sync goes stale.
//  * A gateway that is joined to WiFi must stay on its router's channel: just call begin() after it
//    connects and don't touch the channel.
//  * The broadcast peer is added with channel 0 ("whatever the radio is on now").
//
// Frames are sent as ESP-NOW BROADCASTS by default, the path the demos proved on hardware. Every device
// in range then hears every frame, so with several wands casting at once a wand can see another wand's
// CastResult (results are advisory, the resolver is what counts). Set unicast = true to address replies
// to the specific peer (link-layer ACK + retry, no cross-talk). That mode has NOT been run on hardware.
#ifndef MAGIC_ESP_NOW_H
#define MAGIC_ESP_NOW_H

#if !defined(ARDUINO_ARCH_ESP32)
#error "MagicEspNow.h needs the ESP32 Arduino core."
#endif

#include "MagicCore.h"

namespace magic {

class EspNowTransport : public Transport {
 public:
  // Initialise ESP-NOW. Call after WiFi.mode(...) (and after connecting, for a gateway).
  // Busy if another EspNowTransport already owns the callback; Timeout if the radio refused.
  Status begin(bool unicast = false);

  // Move the radio to `channel` (1..13). For wands and other unassociated radios.
  Status setChannel(uint8_t channel);
  uint8_t channel() const { return channel_; }

  // Call from loop(): while `synced` is false, hop to the next channel every `dwellMs`.
  // Once synced, stays put. (A wand that has not yet been paired with a gateway calls this.)
  void scanForGateway(bool synced, uint64_t nowMs, uint32_t dwellMs = 700);

  size_t maxFrameSize() const override { return 250; }
  Status send(const PeerId *peer, const uint8_t *frame, size_t len) override;
  bool receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) override;

  // Frames dropped because the receive queue was full (the radio outran the loop).
  unsigned dropped() const { return dropped_; }

 public:
  // Internal: called by the ESP-NOW receive callback. Not for application use.
  static void onRecvBridge(const uint8_t *mac, const uint8_t *data, int len);

 private:
  void push(const uint8_t *mac, const uint8_t *data, int len);
  Status ensurePeer(const uint8_t mac[6]);

  static const int kQueue = 8;
  uint8_t frames_[kQueue][MAGIC_FRAME_MAX_SIZE];
  uint8_t lens_[kQueue] = {0};
  uint8_t macs_[kQueue][6];
  int head_ = 0, count_ = 0;
  volatile unsigned dropped_ = 0;
  uint8_t channel_ = 1;
  uint64_t lastHop_ = 0;
  bool unicast_ = false;
  bool started_ = false;
};

}  // namespace magic

#endif
