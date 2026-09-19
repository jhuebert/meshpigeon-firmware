#include "meshpigeon/packet_store.h"

#include <stdlib.h>
#include <string.h>

namespace meshpigeon {

static const size_t kRecordOverhead = kStoredPacketOverhead;  // 12-byte header

PacketStore::PacketStore(uint32_t capacity_bytes)
    : capacity_(capacity_bytes), wrap_(capacity_bytes) {
  // A store that cannot hold even one minimum entry is disabled outright
  // (appends drop, nothing crashes).
  if (capacity_ < kRecordOverhead + 1) {
    capacity_ = 0;
    wrap_ = 0;
  }
  buf_ = capacity_ ? (uint8_t*)calloc(capacity_, 1) : nullptr;
  if (!buf_) {
    capacity_ = 0;  // allocation failed: store stays empty
    wrap_ = 0;
  }
}

PacketStore::~PacketStore() { free(buf_); }

uint32_t PacketStore::free_bytes() const {
  if (count_ == 0) return capacity_;
  if (head_off_ == tail_off_) return 0;  // full
  // Wrapped layout ([0..tail) + [head..wrap)): the free hole sits between
  // tail and head. Linear layout: everything outside [head..tail).
  return head_off_ > tail_off_ ? head_off_ - tail_off_
                               : (wrap_ - tail_off_) + head_off_;
}

void PacketStore::read_header(uint32_t off, Header* h) const {
  memcpy(h, buf_ + off, kRecordOverhead);
}

void PacketStore::write_header(uint32_t off, const Header& h) {
  memcpy(buf_ + off, &h, kRecordOverhead);
}

void PacketStore::pop_oldest() {
  Header h;
  read_header(head_off_, &h);
  uint32_t next = head_off_ + kRecordOverhead + h.len;
  if (next >= wrap_) {
    head_off_ = 0;
    wrap_ = capacity_;  // tail segment exhausted: linear again
  } else {
    head_off_ = next;
  }
  count_--;
  head_seq_++;
  dropped_++;
}

uint32_t PacketStore::append(uint32_t uptime_ms, int8_t rssi, int8_t snr,
                             uint8_t flags, const uint8_t* raw, uint8_t len) {
  if (len > MESHPGEON_MAX_RAW_PACKET) len = MESHPGEON_MAX_RAW_PACKET;
  uint32_t need = (uint32_t)(kRecordOverhead + len);
  if (capacity_ == 0 || need > capacity_) {
    dropped_++;  // can never fit this pool — no seq consumed
    return 0;
  }

  // Evict oldest entries until there is room for the new one.
  while (count_ > 0 && free_bytes() < need) pop_oldest();

  if (count_ == 0) {
    head_off_ = tail_off_ = 0;
    wrap_ = capacity_;
  } else if (head_off_ > tail_off_) {
    // Wrapped: room at the tail is bounded by head; free_bytes() >= need
    // already guarantees it fits contiguously.
  } else if ((wrap_ - tail_off_) < need) {
    // Linear but the entry does not fit before the end of the pool: wrap
    // the tail to the front. The front must be clear of live data first.
    while (count_ > 0 && head_off_ < need) pop_oldest();
    if (count_ == 0) {
      head_off_ = tail_off_ = 0;
      wrap_ = capacity_;
    } else {
      wrap_ = tail_off_;  // entries live in [head..wrap) + [0..tail) now
      tail_off_ = 0;
    }
  }

  Header h = {next_seq_, uptime_ms, rssi, snr, flags, len};
  write_header(tail_off_, h);
  memcpy(buf_ + tail_off_ + kRecordOverhead, raw, len);
  tail_off_ += need;
  if (tail_off_ >= wrap_) tail_off_ = 0;
  count_++;
  return next_seq_++;
}

bool PacketStore::get(uint32_t seq, StoredPacket* out) const {
  if (count_ == 0 || seq < head_seq_ || seq >= next_seq_) return false;
  uint32_t off = head_off_;
  for (uint32_t i = 0; i < seq - head_seq_; i++) {
    Header h;
    read_header(off, &h);
    off += kRecordOverhead + h.len;
    if (off >= wrap_) off = 0;
  }
  Header h;
  read_header(off, &h);
  if (h.seq != seq) return false;  // defensive: ring corruption
  out->seq = h.seq;
  out->uptime_ms = h.uptime_ms;
  out->rssi = h.rssi;
  out->snr = h.snr;
  out->flags = h.flags;
  out->len = h.len;
  memcpy(out->raw, buf_ + off + kRecordOverhead, h.len);
  return true;
}

uint32_t PacketStore::count_since_impl(uint32_t since_seq) const {
  if (count_ == 0) return 0;
  uint32_t first = head_seq_ > since_seq ? head_seq_ : since_seq + 1;
  if (first >= next_seq_) return 0;
  return next_seq_ - first;
}

}  // namespace meshpigeon
