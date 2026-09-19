#include <string.h>
#include <vector>

#include <unity.h>

#include "meshpigeon/command_processor.h"
#include "meshpigeon/framing.h"
#include "meshpigeon/packet_store.h"
#include "meshpigeon/settings.h"
#include "meshpigeon/uptime_clock.h"

using namespace meshpigeon;

// ---- fakes -----------------------------------------------------------------

class FakeRadio : public ILoRaRadio {
 public:
  bool apply(const RadioSettings& s) override {
    applied_ = s;
    apply_calls_++;
    return apply_ok_;
  }
  int transmit(const uint8_t* raw, uint8_t len) override {
    last_tx_len_ = len;
    memcpy(last_tx_, raw, len);
    tx_calls_++;
    tx_active_ = tx_async_;  // async radios hold TX until complete_tx()
    return tx_ok_ ? 0 : -1;
  }
  bool tx_done() override {
    if (!tx_active_) return true;
    if (complete_after_calls_ > 0 && --complete_after_calls_ == 0) {
      tx_active_ = false;
      return true;
    }
    return !tx_active_;
  }
  void complete_tx() { tx_active_ = false; }

  bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi, int8_t* snr) override {
    if (rx_queue_len_ == 0) return false;
    *len = rx_len_[rx_head_];
    memcpy(raw, rx_queue_[rx_head_], *len);
    *rssi = rx_rssi_[rx_head_];
    *snr = rx_snr_[rx_head_];
    rx_head_ = (rx_head_ + 1) % 16;
    rx_queue_len_--;
    return true;
  }

  void push_rx(const uint8_t* data, uint8_t len, int8_t rssi = -80,
               int8_t snr = 10) {
    int idx = (rx_head_ + rx_queue_len_) % 16;
    memcpy(rx_queue_[idx], data, len);
    rx_len_[idx] = len;
    rx_rssi_[idx] = rssi;
    rx_snr_[idx] = snr;
    rx_queue_len_++;
  }

  RadioSettings applied_{};
  int apply_calls_ = 0;
  int tx_calls_ = 0;
  uint8_t last_tx_[256] = {0};
  uint8_t last_tx_len_ = 0;
  bool apply_ok_ = true;
  bool tx_ok_ = true;
  bool tx_async_ = false;      // model an in-flight TX when true
  uint32_t complete_after_calls_ = 0;
  bool tx_active_ = false;
  uint8_t rx_queue_[16][200];
  uint8_t rx_len_[16];
  int8_t rx_rssi_[16];
  int8_t rx_snr_[16];
  int rx_head_ = 0;
  int rx_queue_len_ = 0;
};

class RecordingSink : public IFrameSink {
 public:
  void send_frame(const uint8_t* decoded, size_t len) override {
    frames_[count_ % 64] = std::vector<uint8_t>(decoded, decoded + len);
    lens_[count_ % 64] = len;
    count_++;
  }
  // Chronological access: frame(0) is the FIRST frame received.
  const uint8_t* frame(int i) const { return frames_[i % 64].data(); }
  size_t len(int i) const { return lens_[i % 64]; }
  uint8_t cmd(int i) const { return frame(i)[0]; }
  uint8_t nonce(int i) const { return frame(i)[1]; }
  uint8_t status(int i) const { return frame(i)[2]; }
  const uint8_t* payload(int i) const { return frame(i) + 3; }
  size_t payload_len(int i) const { return len(i) - 5; }

  std::vector<uint8_t> frames_[64];
  size_t lens_[64];
  size_t count_ = 0;
};

// ---- helpers ----------------------------------------------------------------

static RadioSettings make_settings(uint8_t region, uint32_t epoch) {
  RadioSettings s = RadioSettings::unset();
  s.region = region;
  s.freq_hz = 906875000;
  s.bw_x100khz = 12500;
  s.sf = 9;
  s.cr = 5;
  s.power_dbm = 22;
  s.config_epoch = epoch;
  return s;
}

static PacketStore* store;
static ManualClock* raw_clock;
static UptimeClock* clock_;
static MemorySettingsStore* sstore;
static FakeRadio* radio;
static CommandProcessor* proc;
static RecordingSink* sink;

void setUp(void) {
  store = new PacketStore(1024);  // byte budget: plenty for these tests
  raw_clock = new ManualClock(1000);
  clock_ = new UptimeClock(*raw_clock);
  sstore = new MemorySettingsStore();
  radio = new FakeRadio();
  proc = new CommandProcessor(*store, *clock_, *sstore, *radio, "TEST", "0.1");
  sink = new RecordingSink();
  proc->add_sink(sink);
  proc->boot();
}
void tearDown(void) {
  delete sink;
  delete proc;
  delete radio;
  delete sstore;
  delete clock_;
  delete raw_clock;
  delete store;
}

// ---- tests: framing -----------------------------------------------------------

void test_cobs_roundtrip() {
  const uint8_t src[] = {0x01, 0x00, 0x02, 0x03, 0x00, 0xFF, 0x05};
  uint8_t enc[64];
  uint8_t dec[64];
  size_t n = cobs_encode(enc, src, sizeof(src));
  size_t m = cobs_decode(dec, enc, n);
  TEST_ASSERT_EQUAL(sizeof(src), m);
  TEST_ASSERT_EQUAL_MEMORY(src, dec, sizeof(src));
}

void test_cobs_empty() {
  uint8_t enc[4];
  uint8_t dec[4];
  size_t n = cobs_encode(enc, nullptr, 0);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(0, cobs_decode(dec, enc, n));  // empty is not a frame here
}

void test_frame_roundtrip_and_crc() {
  uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t built[16];
  size_t n = frame_build(built, CMD_PING, 0x42, STATUS_OK, payload, 4);
  TEST_ASSERT_EQUAL(9, n);
  // Corrupt one byte -> frame_crc differs
  uint8_t copy[16];
  memcpy(copy, built, n);
  copy[3] ^= 0xFF;
  TEST_ASSERT_NOT_EQUAL(frame_crc(copy, n), (uint16_t)(copy[n - 2] | (copy[n - 1] << 8)));
}

void test_frame_reader_stream() {
  uint8_t payload[] = {1, 2, 3};
  uint8_t built[16];
  size_t n = frame_build(built, CMD_GET_INFO, 0x07, STATUS_OK, payload, 3);
  uint8_t wire[FRAME_MAX_WIRE];
  size_t wl = frame_encode_wire(wire, built, n);

  FrameReader r;
  uint8_t out[FRAME_MAX_DECODED];
  size_t got = 0;
  for (size_t i = 0; i < wl; i++) {
    size_t res = r.feed(wire[i], out);
    if (res != 0 && res != (size_t)-1) got = res;
  }
  TEST_ASSERT_EQUAL(n, got);
  TEST_ASSERT_EQUAL(CMD_GET_INFO, out[0]);
  TEST_ASSERT_EQUAL(0x07, out[1]);
  TEST_ASSERT_EQUAL(STATUS_OK, out[2]);
  TEST_ASSERT_EQUAL_MEMORY(payload, out + 3, 3);
}

void test_frame_reader_bad_crc_dropped() {
  uint8_t built[16];
  size_t n = frame_build(built, CMD_PING, 1, STATUS_OK, nullptr, 0);
  built[0] ^= 0xFF;  // corrupt
  uint8_t wire[FRAME_MAX_WIRE];
  size_t wl = frame_encode_wire(wire, built, n);
  FrameReader r;
  uint8_t out[FRAME_MAX_DECODED];
  size_t res = 0;
  for (size_t i = 0; i < wl; i++) res = r.feed(wire[i], out);
  TEST_ASSERT_EQUAL((size_t)-1, res);
}

void test_frame_reader_ignores_garbage_between_frames() {
  uint8_t built[16];
  size_t n = frame_build(built, CMD_PING, 9, STATUS_OK, nullptr, 0);
  uint8_t wire[FRAME_MAX_WIRE];
  size_t wl = frame_encode_wire(wire, built, n);

  // A corrupted frame (garbage prepended to the body) is dropped, and the
  // next good frame still decodes.
  uint8_t stream[128];
  size_t sl = 0;
  stream[sl++] = 0x55;  // garbage that breaks the first frame
  stream[sl++] = 0xAA;
  memcpy(&stream[sl], wire, wl);
  sl += wl;
  memcpy(&stream[sl], wire, wl);  // intact frame follows
  sl += wl;

  FrameReader r;
  uint8_t out[FRAME_MAX_DECODED];
  size_t first = (size_t)-2, second = 0;
  for (size_t i = 0; i < sl; i++) {
    size_t res = r.feed(stream[i], out);
    if (res == (size_t)-1) {
      if (first == (size_t)-2) first = (size_t)-1;
    } else if (res != 0 && second == 0) {
      if (first == (size_t)-2) first = res;
      second = res;
    }
  }
  TEST_ASSERT_EQUAL((size_t)-1, first);  // garbage frame dropped
  TEST_ASSERT_EQUAL(n, second);          // good frame decoded
}

// ---- tests: packet store ------------------------------------------------------

// Byte cost of one retained packet of the given payload length.
static uint32_t rec_b(uint8_t len) { return 12 + len; }

void test_store_append_and_fetch_order() {
  PacketStore s(rec_b(1) * 4 + 1);  // room for 4 one-byte packets
  uint8_t data[4];
  for (int i = 1; i <= 6; i++) {
    data[0] = (uint8_t)i;
    s.append(100 + i, -70, 9, 0x02, data, 1);
  }
  TEST_ASSERT_EQUAL(4, s.count());
  TEST_ASSERT_EQUAL(2, s.dropped());        // oldest two evicted
  TEST_ASSERT_EQUAL(3, s.oldest_seq());     // seq 3,4,5,6 retained
  TEST_ASSERT_EQUAL(7, s.next_seq());

  // fetch since 0 delivers exactly the retained ones in ascending order
  uint32_t seen[8];
  uint32_t n = s.fetch_since(0, 10, [&](const StoredPacket& e) {
    seen[e.seq - 1] = e.raw[0];
    return true;
  });
  TEST_ASSERT_EQUAL(4, n);
  TEST_ASSERT_EQUAL(3, seen[2]);
  TEST_ASSERT_EQUAL(4, seen[3]);
  TEST_ASSERT_EQUAL(5, seen[4]);
  TEST_ASSERT_EQUAL(6, seen[5]);
}

void test_store_packs_variable_lengths() {
  PacketStore s(120);
  uint8_t big[30], one[1], mid[40], small[20];
  memset(big, 0xAA, sizeof(big));
  one[0] = 0x55;
  memset(mid, 0xBB, sizeof(mid));
  small[0] = 0x11;
  uint32_t s1 = s.append(0, -70, 9, 0x02, big, 30);    // 42 bytes
  s.append(0, -70, 9, 0x02, one, 1);                   // 13 bytes
  uint32_t s3 = s.append(0, -70, 9, 0x02, mid, 40);    // 52 bytes
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(rec_b(30) + rec_b(1) + rec_b(40), s.bytes_used());

  // a 4th packet that does not fit evicts exactly the oldest one
  uint32_t s4 = s.append(0, -70, 9, 0x02, small, 20);  // 32 bytes
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(1, s.dropped());
  TEST_ASSERT_EQUAL(2, s.oldest_seq());

  // contents survive the eviction
  StoredPacket e;
  TEST_ASSERT_FALSE(s.get(s1, &e));  // s1 was evicted
  TEST_ASSERT_TRUE(s.get(s3, &e));
  TEST_ASSERT_EQUAL(0xBB, e.raw[0]);
  TEST_ASSERT_EQUAL(40, e.len);
  TEST_ASSERT_TRUE(s.get(s4, &e));
  TEST_ASSERT_EQUAL(0x11, e.raw[0]);
}

void test_store_wraps_tail_to_front() {
  PacketStore s(64);
  uint8_t one[1] = {0x01}, long_[20];
  memset(long_, 0x22, sizeof(long_));
  s.append(0, -70, 9, 0x02, one, 1);    // seq 1: [0..13)
  s.append(0, -70, 9, 0x02, one, 1);    // seq 2: [13..26)
  s.append(0, -70, 9, 0x02, long_, 20); // seq 3: [26..58), 6 bytes left
  TEST_ASSERT_EQUAL(3, s.count());

  // 13 bytes do not fit at the tail: evict seq 1, wrap tail to the front
  uint32_t s4 = s.append(0, -70, 9, 0x02, one, 1);
  TEST_ASSERT_EQUAL(4, s4);
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(1, s.dropped());
  TEST_ASSERT_EQUAL(2, s.oldest_seq());

  // every entry still readable, fetch order intact across the wrap
  StoredPacket e;
  TEST_ASSERT_TRUE(s.get(4, &e));
  TEST_ASSERT_EQUAL(0x01, e.raw[0]);
  TEST_ASSERT_TRUE(s.get(3, &e));
  TEST_ASSERT_EQUAL(0x22, e.raw[0]);
  TEST_ASSERT_EQUAL(20, e.len);
  uint32_t seqs[8], n = 0;
  s.fetch_since(0, 10, [&](const StoredPacket& x) { seqs[n++] = x.seq; return true; });
  TEST_ASSERT_EQUAL(3, n);
  TEST_ASSERT_EQUAL(2, seqs[0]);
  TEST_ASSERT_EQUAL(3, seqs[1]);
  TEST_ASSERT_EQUAL(4, seqs[2]);

  // fill the wrapped hole, then overflow-evict cleanly
  s.append(0, -70, 9, 0x02, one, 1);    // seq 5: fills the hole at [13..26)
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(6, s.next_seq());
  TEST_ASSERT_EQUAL(2, s.dropped());
  TEST_ASSERT_EQUAL(3, s.oldest_seq());
  s.append(0, -70, 9, 0x02, one, 1);    // seq 6: evicts seq 3, ring goes linear again
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(3, s.dropped());
  TEST_ASSERT_EQUAL(4, s.oldest_seq());
  TEST_ASSERT_TRUE(s.get(6, &e));
}

void test_store_oversize_packet_never_fits() {
  PacketStore s(64);  // smaller than 12 + 200
  uint8_t big[MESHPGEON_MAX_RAW_PACKET];
  memset(big, 0x77, sizeof(big));
  TEST_ASSERT_EQUAL(0, s.append(0, -70, 9, 0x02, big, MESHPGEON_MAX_RAW_PACKET));
  TEST_ASSERT_EQUAL(0, s.count());
  TEST_ASSERT_EQUAL(1, s.dropped());
  TEST_ASSERT_EQUAL(1, s.next_seq());  // no seq consumed by the drop
  TEST_ASSERT_EQUAL(0, s.count_since(0));
}

void test_store_max_size_packet_roundtrip() {
  PacketStore s(12 + MESHPGEON_MAX_RAW_PACKET);
  uint8_t big[MESHPGEON_MAX_RAW_PACKET];
  for (int i = 0; i < MESHPGEON_MAX_RAW_PACKET; i++) big[i] = (uint8_t)i;
  uint32_t sq = s.append(1234, -100, -5, 0x01, big, MESHPGEON_MAX_RAW_PACKET);
  TEST_ASSERT_EQUAL(1, sq);
  StoredPacket e;
  TEST_ASSERT_TRUE(s.get(sq, &e));
  TEST_ASSERT_EQUAL(MESHPGEON_MAX_RAW_PACKET, e.len);
  TEST_ASSERT_EQUAL_MEMORY(big, e.raw, MESHPGEON_MAX_RAW_PACKET);
  TEST_ASSERT_EQUAL(-100, e.rssi);
  TEST_ASSERT_EQUAL(1234, e.uptime_ms);
}

void test_store_since_cursor_is_resumable() {
  PacketStore s(rec_b(4) * 10);
  uint8_t data[4] = {1, 2, 3, 4};
  for (int i = 0; i < 5; i++) s.append(0, -70, 9, 0x02, data, 1);
  // simulate a partial fetch that got seqs 1..2
  uint32_t n = s.fetch_since(2, 10, [](const StoredPacket&) { return true; });
  TEST_ASSERT_EQUAL(3, n);  // 3,4,5 remain
}

void test_store_get_across_wrap() {
  PacketStore s(rec_b(4) * 3);
  uint8_t data[4] = {9, 9, 9, 9};
  for (int i = 0; i < 10; i++) s.append(0, -70, 9, 0x02, data, 1);
  StoredPacket e;
  TEST_ASSERT_TRUE(s.get(9, &e));    // retained
  TEST_ASSERT_EQUAL(9, e.seq);
  TEST_ASSERT_FALSE(s.get(5, &e));   // evicted
  TEST_ASSERT_FALSE(s.get(11, &e));  // not yet assigned
}

void test_store_purge_resets_history() {
  PacketStore s(rec_b(1) * 3);
  uint8_t data[4] = {7};
  s.append(0, -70, 9, 0x02, data, 1);
  s.clear();
  TEST_ASSERT_EQUAL(0, s.count());
  TEST_ASSERT_EQUAL(2, s.next_seq());  // seq stays monotonic after purge
  uint32_t n = s.fetch_since(0, 10, [](const StoredPacket&) { return true; });
  TEST_ASSERT_EQUAL(0, n);
}

void test_store_randomized_matches_model() {
  // Deterministic PRNG stress: compare against a simple reference model.
  PacketStore s(100);
  std::vector<uint32_t> seqs;   // model: retained seqs, oldest first
  uint8_t payload[MESHPGEON_MAX_RAW_PACKET];
  for (int i = 0; i < (int)sizeof(payload); i++) payload[i] = (uint8_t)(i * 7);
  uint64_t rng = 0x12345678;

  for (int op = 0; op < 5000; op++) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    uint8_t len = (uint8_t)(1 + (rng >> 33) % MESHPGEON_MAX_RAW_PACKET);
    uint32_t sq = s.append(0, -70, 9, 0x02, payload, len);
    if (sq != 0) seqs.push_back(sq);
    // evict from the model while it exceeds what the store can hold
    while (seqs.size() > s.count()) seqs.erase(seqs.begin());

    TEST_ASSERT_EQUAL(seqs.size(), s.count());
    TEST_ASSERT_EQUAL(seqs.empty() ? 1 : seqs.front(), s.oldest_seq());

    // every retained packet must round-trip intact
    for (size_t i = 0; i < seqs.size(); i++) {
      StoredPacket e;
      TEST_ASSERT_TRUE_MESSAGE(s.get(seqs[i], &e), "get failed");
      TEST_ASSERT_EQUAL(seqs[i], e.seq);
      TEST_ASSERT_EQUAL(payload[(i * 3) % len], e.raw[(i * 3) % len]);
    }
    // fetch_since walks the whole retained range in order
    uint32_t n = s.fetch_since(0, 100000, [](const StoredPacket&) { return true; });
    TEST_ASSERT_EQUAL(seqs.size(), n);
  }
}

// ---- tests: settings -----------------------------------------------------------

void test_settings_serialize_roundtrip() {
  RadioSettings s = make_settings(3, 42);
  uint8_t buf[RadioSettings::kSerializedSize];
  s.serialize(buf);
  RadioSettings t;
  TEST_ASSERT_TRUE(t.deserialize(buf, sizeof(buf)));
  TEST_ASSERT_TRUE(s == t);
}

void test_settings_corrupt_crc_rejected() {
  RadioSettings s = make_settings(3, 42);
  uint8_t buf[RadioSettings::kSerializedSize];
  s.serialize(buf);
  buf[2] ^= 0xFF;
  RadioSettings t;
  TEST_ASSERT_FALSE(t.deserialize(buf, sizeof(buf)));
}

void test_settings_bad_size_rejected() {
  RadioSettings t;
  uint8_t buf[8] = {0};
  TEST_ASSERT_FALSE(t.deserialize(buf, sizeof(buf)));
}

// ---- tests: command processor ---------------------------------------------------

void test_ping_echoes() {
  uint8_t payload[] = {0xAB, 0xCD};
  proc->on_frame(CMD_PING, 0x11, 0, payload, 2, sink);
  TEST_ASSERT_EQUAL(1, sink->count_);
  TEST_ASSERT_EQUAL(CMD_PING, sink->cmd(0));
  TEST_ASSERT_EQUAL(0x11, sink->nonce(0));
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  TEST_ASSERT_EQUAL_MEMORY(payload, sink->payload(0), 2);
}

void test_unknown_command_errors() {
  proc->on_frame(0x77, 1, 0, nullptr, 0, sink);
  TEST_ASSERT_EQUAL(STATUS_ERR_BAD_CMD, sink->status(0));
}

void test_boot_uses_persisted_settings() {
  RadioSettings s = make_settings(7, 5);
  sstore->save(s);
  // fresh processor (setUp made one) — boot again with settings present
  proc->boot();
  TEST_ASSERT_EQUAL(2, radio->apply_calls_);  // setUp boot + this boot
  TEST_ASSERT_TRUE(radio->applied_ == s);
}

void test_boot_first_time_uses_safe_default() {
  TEST_ASSERT_TRUE(radio->applied_ == RadioSettings::unset());
}

void test_set_radio_persists_and_applies() {
  RadioSettings s = make_settings(2, 99);  // epoch ignored: radio bumps it
  uint8_t buf[RadioSettings::kSerializedSize];
  s.serialize(buf);
  proc->on_frame(CMD_SET_RADIO, 0x22, 0, buf, sizeof(buf), sink);
  TEST_ASSERT_EQUAL(1, sink->count_);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  RadioSettings echoed;
  TEST_ASSERT_TRUE(echoed.deserialize(sink->payload(0), sink->payload_len(0)));
  TEST_ASSERT_EQUAL(2, echoed.region);
  TEST_ASSERT_EQUAL(1, echoed.config_epoch);  // bumped by radio
  TEST_ASSERT_TRUE(proc->settings() == echoed);
  // persisted for next boot
  RadioSettings loaded;
  TEST_ASSERT_TRUE(sstore->load(&loaded));
  TEST_ASSERT_TRUE(loaded == echoed);
}

void test_set_radio_bad_payload() {
  uint8_t buf[4] = {1, 2, 3, 4};
  proc->on_frame(CMD_SET_RADIO, 1, 0, buf, sizeof(buf), sink);
  TEST_ASSERT_EQUAL(STATUS_ERR_BAD_PAYLOAD, sink->status(0));
}

void test_first_owner_lock_honors_only_first_set() {
  RadioSettings s = make_settings(1, 0);
  uint8_t buf[RadioSettings::kSerializedSize];
  s.serialize(buf);
  proc->on_frame(CMD_SET_RADIO, 1, 0, buf, sizeof(buf), sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  // second client tries within the 5-minute grace window
  RecordingSink other;
  proc->add_sink(&other);
  proc->on_frame(CMD_SET_RADIO, 2, 0, buf, sizeof(buf), &other);
  TEST_ASSERT_EQUAL(STATUS_ERR_BUSY, other.status(0));
  // after grace window, re-tune allowed
  raw_clock->advance(6 * 60 * 1000);
  proc->on_frame(CMD_SET_RADIO, 3, 0, buf, sizeof(buf), &other);
  TEST_ASSERT_EQUAL(STATUS_OK, other.status(1));  // (0) was the earlier BUSY
  proc->remove_sink(&other);
}

void test_send_packet_roundtrip() {
  radio->tx_async_ = true;  // TX in flight until poll()
  uint8_t pkt[] = {0x45, 0x01, 0x02, 0x03, 0x04};
  uint8_t payload[1 + sizeof(pkt)];
  payload[0] = sizeof(pkt);
  memcpy(payload + 1, pkt, sizeof(pkt));
  proc->on_frame(CMD_SEND_PACKET, 0x33, 0, payload, sizeof(payload), sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  uint32_t seq;
  memcpy(&seq, sink->payload(0), 4);
  TEST_ASSERT_EQUAL(1, seq);
  TEST_ASSERT_EQUAL(1, radio->tx_calls_);
  TEST_ASSERT_EQUAL_MEMORY(pkt, radio->last_tx_, sizeof(pkt));
  TEST_ASSERT_EQUAL(1, sink->count_);  // no TX_RESULT yet
  proc->poll();                        // radio still busy
  TEST_ASSERT_EQUAL(1, sink->count_);
  radio->complete_tx();
  proc->poll();                        // now TX completes
  TEST_ASSERT_EQUAL(2, sink->count_);
  TEST_ASSERT_EQUAL(CMD_TX_RESULT, sink->cmd(1));
  uint32_t rseq;
  memcpy(&rseq, sink->payload(1), 4);
  TEST_ASSERT_EQUAL(seq, rseq);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->payload(1)[4]);
  // stored as sent
  StoredPacket e;
  TEST_ASSERT_TRUE(store->get(seq, &e));
  TEST_ASSERT_EQUAL(0x01, e.flags);
}

void test_send_packet_tx_failure_reports_result() {
  radio->tx_ok_ = false;  // radio refuses to start
  uint8_t payload[] = {1, 0x42};
  proc->on_frame(CMD_SEND_PACKET, 1, 0, payload, 2, sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));  // accepted, tx failed async
  TEST_ASSERT_EQUAL(2, sink->count_);             // immediate TX_RESULT
  TEST_ASSERT_EQUAL(CMD_TX_RESULT, sink->cmd(1));
  TEST_ASSERT_EQUAL(STATUS_ERR_TX_FAILED, sink->payload(1)[4]);
}

void test_send_packet_busy_when_second_in_flight() {
  radio->tx_async_ = true;
  uint8_t payload[] = {1, 0x42};
  proc->on_frame(CMD_SEND_PACKET, 1, 0, payload, 2, sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  proc->on_frame(CMD_SEND_PACKET, 2, 0, payload, 2, sink);
  TEST_ASSERT_EQUAL(STATUS_ERR_BUSY, sink->status(1));
}

void test_send_packet_bad_length_prefix() {
  uint8_t payload[] = {5, 1, 2};  // claims 5 bytes, carries 2
  proc->on_frame(CMD_SEND_PACKET, 1, 0, payload, 3, sink);
  TEST_ASSERT_EQUAL(STATUS_ERR_BAD_PAYLOAD, sink->status(0));
}

void test_fetch_packets_streams_entries_and_end() {
  uint8_t pkt[] = {0x45, 0x06};
  for (int i = 0; i < 3; i++) {
    radio->push_rx(pkt, sizeof(pkt), -72, 8);
    proc->on_packet_received(-72, 8, pkt, sizeof(pkt));
  }
  RecordingSink fetcher;
  proc->add_sink(&fetcher);
  uint8_t req[6] = {0, 0, 0, 0, 10, 0};  // since 0, max 10
  proc->on_frame(CMD_FETCH_PACKETS, 0x55, 0, req, 6, &fetcher);
  // 3 entry frames (RX_PACKET w/ request nonce) + FETCH_END, in order
  TEST_ASSERT_EQUAL(4, fetcher.count_);
  TEST_ASSERT_EQUAL(CMD_RX_PACKET, fetcher.cmd(0));
  TEST_ASSERT_EQUAL(0x55, fetcher.nonce(0));
  TEST_ASSERT_EQUAL(CMD_FETCH_END, fetcher.cmd(3));
  TEST_ASSERT_EQUAL(0x55, fetcher.nonce(3));
  uint16_t endcount =
      (uint16_t)(fetcher.payload(3)[0] | (fetcher.payload(3)[1] << 8));
  TEST_ASSERT_EQUAL(3, endcount);
  proc->remove_sink(&fetcher);
}

void test_fetch_packets_since_cursor_skips_old() {
  uint8_t pkt[] = {0x45, 0x06};
  for (int i = 0; i < 3; i++) proc->on_packet_received(-72, 8, pkt, sizeof(pkt));
  RecordingSink fetcher;
  proc->add_sink(&fetcher);
  uint8_t req[6] = {1, 0, 0, 0, 10, 0};  // since seq 1 -> seqs 2,3
  proc->on_frame(CMD_FETCH_PACKETS, 0x56, 0, req, 6, &fetcher);
  TEST_ASSERT_EQUAL(3, fetcher.count_);  // 2 entries + END
  uint16_t endcount =
      (uint16_t)(fetcher.payload(2)[0] | (fetcher.payload(2)[1] << 8));
  TEST_ASSERT_EQUAL(2, endcount);
  proc->remove_sink(&fetcher);
}

void test_rx_packet_pushes_to_all_sinks_except_none() {
  uint8_t pkt[] = {0x45, 0x07, 0x08};
  RecordingSink a, b;
  proc->add_sink(&a);
  proc->add_sink(&b);
  proc->on_packet_received(-80, 10, pkt, sizeof(pkt));
  TEST_ASSERT_EQUAL(1, a.count_);
  TEST_ASSERT_EQUAL(1, b.count_);
  TEST_ASSERT_EQUAL(CMD_RX_PACKET, a.cmd(0));
  TEST_ASSERT_EQUAL(0, a.nonce(0));  // async: nonce 0
  uint32_t seq;
  memcpy(&seq, a.payload(0), 4);
  TEST_ASSERT_EQUAL(1, seq);
  TEST_ASSERT_EQUAL(0x02, a.payload(0)[10]);  // flags = received
  TEST_ASSERT_EQUAL(3, a.payload(0)[11]);     // len
  TEST_ASSERT_EQUAL_MEMORY(pkt, a.payload(0) + 12, 3);
  proc->remove_sink(&a);
  proc->remove_sink(&b);
}

void test_radio_changed_broadcast_to_others() {
  RecordingSink other;
  proc->add_sink(&other);
  RadioSettings s = make_settings(4, 0);
  uint8_t buf[RadioSettings::kSerializedSize];
  s.serialize(buf);
  proc->on_frame(CMD_SET_RADIO, 1, 0, buf, sizeof(buf), sink);
  // originator gets OK only; other client gets RADIO_CHANGED
  TEST_ASSERT_EQUAL(1, sink->count_);
  TEST_ASSERT_EQUAL(1, other.count_);
  TEST_ASSERT_EQUAL(CMD_RADIO_CHANGED, other.cmd(0));
  TEST_ASSERT_EQUAL(0, other.nonce(0));
  RadioSettings changed;
  TEST_ASSERT_TRUE(changed.deserialize(other.payload(0) + 4, other.payload_len(0) - 4));
  TEST_ASSERT_EQUAL(4, changed.region);
  proc->remove_sink(&other);
}

void test_purge_store() {
  uint8_t pkt[] = {0x45};
  proc->on_packet_received(-80, 10, pkt, 1);
  TEST_ASSERT_EQUAL(1, store->count());
  proc->on_frame(CMD_PURGE_STORE, 1, 0, nullptr, 0, sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  TEST_ASSERT_EQUAL(0, store->count());
}

void test_get_info_shape() {
  proc->on_frame(CMD_GET_INFO, 0x66, 0, nullptr, 0, sink);
  TEST_ASSERT_EQUAL(STATUS_OK, sink->status(0));
  TEST_ASSERT_EQUAL(49, sink->payload_len(0));
  TEST_ASSERT_EQUAL(MESHPGEON_PROTOCOL_VERSION, sink->payload(0)[0]);
  TEST_ASSERT_EQUAL_MEMORY("TEST", sink->payload(0) + 3, 4);
}

// ---- main -----------------------------------------------------------------------

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cobs_roundtrip);
  RUN_TEST(test_cobs_empty);
  RUN_TEST(test_frame_roundtrip_and_crc);
  RUN_TEST(test_frame_reader_stream);
  RUN_TEST(test_frame_reader_bad_crc_dropped);
  RUN_TEST(test_frame_reader_ignores_garbage_between_frames);
  RUN_TEST(test_store_append_and_fetch_order);
  RUN_TEST(test_store_packs_variable_lengths);
  RUN_TEST(test_store_wraps_tail_to_front);
  RUN_TEST(test_store_oversize_packet_never_fits);
  RUN_TEST(test_store_max_size_packet_roundtrip);
  RUN_TEST(test_store_randomized_matches_model);
  RUN_TEST(test_store_since_cursor_is_resumable);
  RUN_TEST(test_store_get_across_wrap);
  RUN_TEST(test_store_purge_resets_history);
  RUN_TEST(test_settings_serialize_roundtrip);
  RUN_TEST(test_settings_corrupt_crc_rejected);
  RUN_TEST(test_settings_bad_size_rejected);
  RUN_TEST(test_ping_echoes);
  RUN_TEST(test_unknown_command_errors);
  RUN_TEST(test_boot_uses_persisted_settings);
  RUN_TEST(test_boot_first_time_uses_safe_default);
  RUN_TEST(test_set_radio_persists_and_applies);
  RUN_TEST(test_set_radio_bad_payload);
  RUN_TEST(test_first_owner_lock_honors_only_first_set);
  RUN_TEST(test_send_packet_roundtrip);
  RUN_TEST(test_send_packet_tx_failure_reports_result);
  RUN_TEST(test_send_packet_busy_when_second_in_flight);
  RUN_TEST(test_send_packet_bad_length_prefix);
  RUN_TEST(test_fetch_packets_streams_entries_and_end);
  RUN_TEST(test_fetch_packets_since_cursor_skips_old);
  RUN_TEST(test_rx_packet_pushes_to_all_sinks_except_none);
  RUN_TEST(test_radio_changed_broadcast_to_others);
  RUN_TEST(test_purge_store);
  RUN_TEST(test_get_info_shape);
  return UNITY_END();
}
