#include "meshpigeon/packet_store.h"

#include <stdlib.h>
#include <string.h>

namespace meshpigeon {

PacketStore::PacketStore(uint32_t capacity) : capacity_(capacity) {
  slots_ = (StoredPacket*)calloc(capacity_, sizeof(StoredPacket));
}

PacketStore::~PacketStore() { free(slots_); }

uint32_t PacketStore::append(uint32_t uptime_ms, int8_t rssi, int8_t snr,
                             uint8_t flags, const uint8_t* raw, uint8_t len) {
  if (len > MESHPGEON_MAX_RAW_PACKET) len = MESHPGEON_MAX_RAW_PACKET;
  if (count_ == capacity_) {  // drop oldest
    head_ = (head_ + 1) % capacity_;
    count_--;
    head_seq_++;
    dropped_++;
  }
  uint32_t idx = (head_ + count_) % capacity_;
  StoredPacket& e = slots_[idx];
  e.seq = next_seq_;
  e.uptime_ms = uptime_ms;
  e.rssi = rssi;
  e.snr = snr;
  e.flags = flags;
  e.len = len;
  memcpy(e.raw, raw, len);
  count_++;
  return next_seq_++;
}

bool PacketStore::get(uint32_t seq, StoredPacket* out) const {
  if (count_ == 0 || seq < head_seq_ || seq >= next_seq_) return false;
  uint32_t idx = head_ + (seq - head_seq_);
  if (idx >= capacity_) idx %= capacity_;  // seq survives store wrap
  *out = slots_[idx];
  return slots_[idx].seq == seq;
}

uint32_t PacketStore::count_since_impl(uint32_t since_seq) const {
  if (count_ == 0) return 0;
  uint32_t first = head_seq_ > since_seq ? head_seq_ : since_seq + 1;
  if (first >= next_seq_) return 0;
  return next_seq_ - first;
}

}  // namespace meshpigeon
