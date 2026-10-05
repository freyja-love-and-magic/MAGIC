#if defined(ARDUINO_ARCH_ESP32)

#include "MagicEspNow.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

namespace magic {

static EspNowTransport *g_instance = nullptr;
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The recv callback signature changed with the IDF 5 based Arduino core 3.x.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void rawRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (g_instance && info) EspNowTransport::onRecvBridge(info->src_addr, data, len);
}
#else
static void rawRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (g_instance) EspNowTransport::onRecvBridge(mac, data, len);
}
#endif

Status EspNowTransport::begin(bool unicast) {
  if (g_instance && g_instance != this) return Status::Busy;
  if (esp_now_init() != ESP_OK) return Status::Timeout;
  g_instance = this;
  unicast_ = unicast;
  if (esp_now_register_recv_cb(rawRecv) != ESP_OK) return Status::Timeout;
  uint8_t ch;
  wifi_second_chan_t second;
  if (esp_wifi_get_channel(&ch, &second) == ESP_OK && ch >= 1) channel_ = ch;
  Status s = ensurePeer(kBroadcast);
  if (s != Status::Ok) return s;
  started_ = true;
  return Status::Ok;
}

Status EspNowTransport::setChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) return Status::BadArgument;
  // Without the promiscuous toggle, set_channel doesn't reliably take effect on a disconnected STA.
  esp_wifi_set_promiscuous(true);
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (err != ESP_OK) return Status::Timeout;
  channel_ = channel;
  return Status::Ok;
}

void EspNowTransport::scanForGateway(bool synced, uint64_t nowMs, uint32_t dwellMs) {
  if (synced || nowMs - lastHop_ < dwellMs) return;
  lastHop_ = nowMs;
  setChannel(channel_ >= 13 ? 1 : channel_ + 1);
}

Status EspNowTransport::ensurePeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return Status::Ok;
  esp_now_peer_info_t info = {};
  memcpy(info.peer_addr, mac, 6);
  info.channel = 0;  // "the channel the radio is on right now", so it can't drift from the real one
  info.encrypt = false;
  return esp_now_add_peer(&info) == ESP_OK ? Status::Ok : Status::Timeout;
}

Status EspNowTransport::send(const PeerId *peer, const uint8_t *frame, size_t len) {
  if (!started_) return Status::NotRegistered;  // begin() not called
  if (!frame || len == 0) return Status::BadArgument;
  if (len > maxFrameSize()) return Status::BufferTooSmall;
  const uint8_t *dest = kBroadcast;
  if (unicast_ && peer && peer->len == 6) {
    dest = peer->bytes;
    Status s = ensurePeer(dest);
    if (s != Status::Ok) return s;
  }
  return esp_now_send(dest, frame, len) == ESP_OK ? Status::Ok : Status::Timeout;  // Ok = handed to the radio, not delivered
}

void EspNowTransport::onRecvBridge(const uint8_t *mac, const uint8_t *data, int len) {
  if (g_instance) g_instance->push(mac, data, len);
}

// Runs on the WiFi task, not loop(): keep it to a short critical section.
void EspNowTransport::push(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 1 || len > MAGIC_FRAME_MAX_SIZE) return;  // not a MAGIC frame
  portENTER_CRITICAL(&g_lock);
  if (count_ == kQueue) {
    dropped_++;
  } else {
    int slot = (head_ + count_) % kQueue;
    memcpy(frames_[slot], data, len);
    lens_[slot] = (uint8_t)len;
    memcpy(macs_[slot], mac, 6);
    count_++;
  }
  portEXIT_CRITICAL(&g_lock);
}

bool EspNowTransport::receive(uint8_t *buf, size_t cap, size_t *len, PeerId *from) {
  bool got = false;
  portENTER_CRITICAL(&g_lock);
  while (count_ > 0 && !got) {
    int slot = head_;
    head_ = (head_ + 1) % kQueue;
    count_--;
    if (lens_[slot] > cap) continue;  // too big for the caller: discard, never truncate
    memcpy(buf, frames_[slot], lens_[slot]);
    *len = lens_[slot];
    if (from) *from = PeerId(macs_[slot], 6);
    got = true;
  }
  portEXIT_CRITICAL(&g_lock);
  return got;
}

}  // namespace magic

#endif
