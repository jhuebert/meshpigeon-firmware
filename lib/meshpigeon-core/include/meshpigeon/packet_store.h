#ifndef MESHPGEON_PACKET_STORE_H
#define MESHPGEON_PACKET_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

namespace meshpigeon {

/**
 * One retained packet. Raw on-air bytes only — the firmware cannot read
 * them (no protocol, no keys), which is exactly what maximizes capacity.
 *
 * Serialized layout (little-endian):
 *   [seq:u32][uptime_ms:u32][rssi:i8][snr:i8][len:u8][flags:u8][raw:len]
 */
struct StoredPacket {
  uint32_t seq;
  uint32_t uptime_ms;
  int8_t rssi;
  int8_t snr;
  uint8_t flags;  // 0x01 = sent by us (TX), 0x02 = received
  uint8_t len;
  uint8_t raw[MESHPGEON_MAX_RAW_PACKET];
};

// seq + uptime + rssi + snr + len + flags
static const size_t kStoredPacketOverhead = 12;

/**
 * Fixed-capacity ring buffer of retained packets. Overflow drops the
 * oldest entries. Sequence numbers are monotonic (across store wraps and
 * purges) and are the cursors the app uses for resumable `since_seq`
 * fetches — a fresh app can replay exact history by seq.
 */
class PacketStore {
 public:
  explicit PacketStore(uint32_t capacity);
  ~PacketStore();

  PacketStore(const PacketStore&) = delete;
  PacketStore& operator=(const PacketStore&) = delete;

  /** Append a packet; drops the oldest on overflow. Returns assigned seq. */
  uint32_t append(uint32_t uptime_ms, int8_t rssi, int8_t snr, uint8_t flags,
                  const uint8_t* raw, uint8_t len);

  /** Copy the entry with the given seq into `out`. False if absent. */
  bool get(uint32_t seq, StoredPacket* out) const;

  /** First seq currently retained (cursor baseline for a fresh app). */
  uint32_t oldest_seq() const { return count_ == 0 ? next_seq_ : head_seq_; }

  uint32_t count() const { return count_; }
  uint32_t capacity() const { return capacity_; }
  uint32_t next_seq() const { return next_seq_; }
  uint32_t dropped() const { return dropped_; }

  void clear() {
    head_ = 0;
    count_ = 0;
    head_seq_ = next_seq_;
  }

  /** Number of retained packets with seq > since_seq. */
  uint32_t count_since(uint32_t since_seq) const {
    return count_since_impl(since_seq);
  }

  /**
   * Deliver up to max_count retained packets with seq > since_seq, oldest
   * first, via cb(entry). cb returning false stops early. Returns count
   * delivered.
   */
  template <typename F>
  uint32_t fetch_since(uint32_t since_seq, uint32_t max_count, F cb) const {
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < count_ && delivered < max_count; i++) {
      const StoredPacket& e = slots_[(head_ + i) % capacity_];
      if (e.seq <= since_seq) continue;
      if (!cb(e)) break;
      delivered++;
    }
    return delivered;
  }

 private:
  uint32_t count_since_impl(uint32_t since_seq) const;

  uint32_t capacity_;
  StoredPacket* slots_;
  uint32_t head_ = 0;      // ring index of the oldest entry
  uint32_t count_ = 0;     // entries retained
  uint32_t next_seq_ = 1;  // seq of the next packet appended
  uint32_t head_seq_ = 1;  // seq of the oldest retained packet
  uint32_t dropped_ = 0;   // entries evicted by overflow
};

}  // namespace meshpigeon

#endif  // MESHPGEON_PACKET_STORE_H
