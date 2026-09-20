#ifndef MESHPIGEON_PACKET_STORE_H
#define MESHPIGEON_PACKET_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
  uint8_t raw[MESHPIGEON_MAX_RAW_PACKET];
};

// seq + uptime + rssi + snr + len + flags
static const size_t kStoredPacketOverhead = 12;

/**
 * Byte-budgeted ring of retained packets. The store holds a fixed pool of
 * `capacity()` BYTES; each entry costs 12 bytes of header plus its exact
 * payload length, so a 30-byte MeshCore packet costs ~42 bytes instead of
 * a max-size slot. Real packets vary 5-10x in size — budgeting bytes
 * instead of packet slots is what makes the retention targets fit in RAM.
 *
 * Entries are appended contiguously; when one does not fit before the end
 * of the pool, the tail wraps to the front (evicting oldest entries until
 * the front has room). A walk therefore crosses the wrap at most once per
 * pass. Overflow drops the oldest entries; a packet larger than the whole
 * pool is dropped outright (still counted in `dropped()`).
 *
 * Sequence numbers are monotonic (across store wraps and purges) and the
 * retained range is always contiguous ([oldest_seq, next_seq)) because
 * eviction only removes from the oldest end — resumable `since_seq`
 * cursors keep working exactly as before.
 */
class PacketStore {
 public:
  explicit PacketStore(uint32_t capacity_bytes);
  ~PacketStore();

  PacketStore(const PacketStore&) = delete;
  PacketStore& operator=(const PacketStore&) = delete;

  /** Append a packet; drops the oldest on overflow. Returns assigned seq
   *  (0 if the packet was dropped without ever entering the store). */
  uint32_t append(uint32_t uptime_ms, int8_t rssi, int8_t snr, uint8_t flags,
                  const uint8_t* raw, uint8_t len);

  /** Copy the entry with the given seq into `out`. False if absent. */
  bool get(uint32_t seq, StoredPacket* out) const;

  /** First seq currently retained (cursor baseline for a fresh app). */
  uint32_t oldest_seq() const { return count_ == 0 ? next_seq_ : head_seq_; }

  uint32_t count() const { return count_; }
  uint32_t capacity() const { return capacity_; }  // byte budget
  uint32_t bytes_used() const { return capacity_ - free_bytes(); }
  uint32_t next_seq() const { return next_seq_; }
  uint32_t dropped() const { return dropped_; }

  void clear() {
    head_off_ = 0;
    tail_off_ = 0;
    wrap_ = capacity_;
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
    uint32_t off = head_off_;
    for (uint32_t i = 0; i < count_ && delivered < max_count; i++) {
      Header h;
      read_header(off, &h);
      uint32_t next = off + kStoredPacketOverhead + h.len;
      if (next >= wrap_) next = 0;
      if (h.seq > since_seq) {
        StoredPacket e;
        e.seq = h.seq;
        e.uptime_ms = h.uptime_ms;
        e.rssi = h.rssi;
        e.snr = h.snr;
        e.flags = h.flags;
        e.len = h.len;
        memcpy(e.raw, buf_ + off + kStoredPacketOverhead, h.len);
        if (!cb(e)) break;
        delivered++;
      }
      off = next;
    }
    return delivered;
  }

 private:
  // On-ring record header, little-endian, followed by exactly `len` bytes.
  struct Header {
    uint32_t seq;
    uint32_t uptime_ms;
    int8_t rssi;
    int8_t snr;
    uint8_t flags;
    uint8_t len;
  };
  static_assert(sizeof(Header) == kStoredPacketOverhead,
                "on-ring header must be packed");

  uint32_t free_bytes() const;
  void read_header(uint32_t off, Header* h) const;
  void write_header(uint32_t off, const Header& h);
  void pop_oldest();
  uint32_t count_since_impl(uint32_t since_seq) const;

  uint32_t capacity_;      // pool size in bytes
  uint8_t* buf_;           // the pool
  uint32_t head_off_ = 0;  // byte offset of the oldest entry
  uint32_t tail_off_ = 0;  // byte offset where the next entry is written
  uint32_t wrap_ = 0;      // entries never cross this offset; 0 if empty
  uint32_t count_ = 0;     // entries retained
  uint32_t next_seq_ = 1;  // seq of the next packet appended
  uint32_t head_seq_ = 1;  // seq of the oldest retained packet
  uint32_t dropped_ = 0;   // entries evicted / dropped by overflow
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_PACKET_STORE_H
