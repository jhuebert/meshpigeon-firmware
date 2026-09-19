#ifndef MESHPGEON_SIM_RADIO_H
#define MESHPGEON_SIM_RADIO_H

#include <cstdlib>
#include <cstring>
#include <deque>

#include "meshpigeon/command_processor.h"

namespace meshpigeon {

/**
 * Desktop stand-in for a real LoRa radio. Scriptable: loss %, dup %, and a
 * synthetic traffic generator so app pipelines can be exercised in CI with
 * zero hardware (09-testing §2).
 */
class SimRadio : public ILoRaRadio {
 public:
  explicit SimRadio(unsigned seed = 1234) : rng_(seed) {}

  bool apply(const RadioSettings& s) override {
    applied_ = s;
    return s.freq_hz >= 150000000 && s.sf >= 5 && s.sf <= 12;
  }

  int transmit(const uint8_t* raw, uint8_t len) override {
    if (loss(rng_) < loss_pct_) return -1;  // RF loss model kills the send
    memcpy(last_tx_, raw, len);
    last_tx_len_ = len;
    tx_count_++;
    tx_active_ = true;
    return 0;
  }

  bool tx_done() override {
    if (!tx_active_) return true;
    if (--tx_ticks_left_ <= 0) {
      tx_active_ = false;
      // deliver to "the air": maybe duplicate, maybe drop
      if (loss(rng_) >= loss_pct_) {
        air_.push_back(AirPacket(last_tx_, last_tx_len_));
        if (loss(rng_) < dup_pct_) air_.push_back(AirPacket(last_tx_, last_tx_len_));
      }
      tx_ticks_left_ = kTxTicks;
      return true;
    }
    return false;
  }

  bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi, int8_t* snr) override {
    if (air_.empty()) return false;
    const AirPacket& p = air_.front();
    *len = p.len;
    memcpy(raw, p.data, p.len);
    *rssi = -60 - (int8_t)(rng() % 40);
    *snr = (int8_t)(rng() % 12);
    air_.pop_front();
    return true;
  }

  /** Inject a packet into the sim's "air" as if another node had sent it. */
  void inject(const uint8_t* raw, uint8_t len) {
    air_.push_back(AirPacket(raw, len));
  }

  int tx_count() const { return tx_count_; }
  void set_loss(uint8_t pct) { loss_pct_ = pct; }
  void set_dup(uint8_t pct) { dup_pct_ = pct; }

 private:
  struct AirPacket {
    uint8_t len;
    uint8_t data[MESHPGEON_MAX_RAW_PACKET];
    AirPacket(const uint8_t* d, uint8_t l) : len(l) { memcpy(data, d, l); }
  };

  static const int kTxTicks = 3;  // sim TX takes 3 poll() calls to finish

  int loss(unsigned) { return (int)((rng() % 100)); }

  unsigned rng() {
    rng_ = rng_ * 1103515245u + 12345u;
    return (rng_ >> 16) & 0x7FFF;
  }

  unsigned rng_;
  uint8_t loss_pct_ = 0;
  uint8_t dup_pct_ = 0;
  RadioSettings applied_{};
  uint8_t last_tx_[MESHPGEON_MAX_RAW_PACKET];
  uint8_t last_tx_len_ = 0;
  int tx_count_ = 0;
  bool tx_active_ = false;
  int tx_ticks_left_ = kTxTicks;
  std::deque<AirPacket> air_;
};

}  // namespace meshpigeon

#endif  // MESHPGEON_SIM_RADIO_H
