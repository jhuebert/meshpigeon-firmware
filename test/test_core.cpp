#include <string.h>
#include <vector>

#include <pb_decode.h>
#include <pb_encode.h>
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
  bool begin() override {
    begin_calls_++;
    return begin_ok_;
  }
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
  int begin_calls_ = 0;
  int apply_calls_ = 0;
  int tx_calls_ = 0;
  uint8_t last_tx_[MESHPIGEON_MAX_RAW_PACKET] = {0};
  uint8_t last_tx_len_ = 0;
  bool begin_ok_ = true;
  bool apply_ok_ = true;
  bool tx_ok_ = true;
  bool tx_async_ = false;      // model an in-flight TX when true
  uint32_t complete_after_calls_ = 0;
  bool tx_active_ = false;
  uint8_t rx_queue_[16][MESHPIGEON_MAX_RAW_PACKET];
  uint8_t rx_len_[16];
  int8_t rx_rssi_[16];
  int8_t rx_snr_[16];
  int rx_head_ = 0;
  int rx_queue_len_ = 0;
};

/** Board facilities as the core sees them, with call counters. */
class FakeHooks : public IBoardHooks {
 public:
  uint16_t battery_mv() override { return battery_mv_; }
  void reboot_to_bootloader() override { bootloader_calls_++; }
  void reboot() override { reboot_calls_++; }
  void factory_reset() override { factory_reset_calls_++; }
  void mac_suffix(char out[5]) override {
    memcpy(out, "A3F2", 4);
    out[4] = 0;
  }
  void set_device_name(const char* name) override {
    name_calls_++;
    strncpy(name_, name, sizeof(name_) - 1);
    name_[sizeof(name_) - 1] = 0;
  }
  uint8_t ble_clients() override { return ble_clients_; }
  uint8_t usb_cdc_clients() override { return usb_cdc_clients_; }
  uint8_t wifi_tcp_clients() override { return wifi_tcp_clients_; }
  void fill_status(StatusMessage* status) override {
    // A sparse hook: exactly the shape a board with no Wi-Fi has, and the
    // one that would surface a stale union in Status.
    if (status_sparse_) {
      status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_OFF;
      return;
    }
    status->wifi_state = status_wifi_state_;
    if (status_wifi_state_ ==
        meshpigeon_Status_WifiState_WIFI_STATE_CONNECTED) {
      strncpy(status->wifi_ssid, "testnet", sizeof(status->wifi_ssid) - 1);
      status->wifi_ssid[sizeof(status->wifi_ssid) - 1] = 0;
    }
    const uint8_t ip[4] = {192, 168, 7, 9};
    memcpy(status->wifi_ipv4.bytes, ip, 4);
    status->wifi_ipv4.size = 4;
    status->wifi_port = 5000;
    status->wifi_rssi = -61;
  }
  void apply_wifi(const DeviceSettings& settings) override {
    apply_wifi_calls_++;
    wifi_ = settings;
  }
  bool wifi_supported() const override { return wifi_supported_; }
  pb_size_t fill_capabilities(meshpigeon_Capability* out,
                              pb_size_t max) override {
    // Honour `max` exactly as a board hook must: a value that does not fit
    // is neither written nor counted. Ignoring it hid a real bug where the
    // core passed a zero capacity and every board advertised nothing.
    const meshpigeon_Capability caps[] = {
        meshpigeon_Capability_CAPABILITY_BLE,
        meshpigeon_Capability_CAPABILITY_USB_CDC};
    pb_size_t n = 0;
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
      if (n < max) out[n] = caps[i];
      n++;
    }
    return n;
  }

  meshpigeon_Status_WifiState status_wifi_state_ =
      meshpigeon_Status_WifiState_WIFI_STATE_CONNECTED;
  uint16_t battery_mv_ = 3900;
  uint8_t ble_clients_ = 3;
  uint8_t usb_cdc_clients_ = 1;
  uint8_t wifi_tcp_clients_ = 2;
  bool wifi_supported_ = true;
  bool status_sparse_ = false;
  DeviceSettings wifi_{};
  char name_[MESHPIGEON_NAME_MAX + 1] = {0};
  int reboot_calls_ = 0;
  int bootloader_calls_ = 0;
  int factory_reset_calls_ = 0;
  int name_calls_ = 0;
  int apply_wifi_calls_ = 0;
};

/** One decoded RadioToClient, as the client side would see it. */
struct Decoded {
  RadioToClientMessage msg = RadioToClientMessage_init_zero;

  bool decode(const uint8_t* data, size_t len) {
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    msg = RadioToClientMessage_init_zero;
    return pb_decode(&stream, RadioToClientMessage_fields, &msg);
  }
  uint32_t id() const { return msg.id; }
  pb_size_t which() const { return msg.which_body; }
  ErrorCode error() const { return msg.body.error.code; }
  const RadioToClientMessage& m() const { return msg; }
};

class RecordingSink : public IFrameSink {
 public:
  void send_frame(const uint8_t* decoded, size_t len) override {
    Decoded d;
    bool ok = d.decode(decoded, len);
    frames_.push_back(d);
    undecoded_ += ok ? 0 : 1;
  }
  // Chronological access: at(0) is the FIRST frame received.
  const Decoded& at(int i) const { return frames_[i]; }
  size_t count() const { return frames_.size(); }
  int undecoded() const { return undecoded_; }
  void clear() { frames_.clear(); }

 private:
  std::vector<Decoded> frames_;
  int undecoded_ = 0;
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
static FakeHooks* hooks;
static CommandProcessor* proc;
static RecordingSink* sink;

void setUp(void) {
  store = new PacketStore(4096);  // byte budget: plenty for these tests
  raw_clock = new ManualClock(1000);
  clock_ = new UptimeClock(*raw_clock);
  sstore = new MemorySettingsStore();
  radio = new FakeRadio();
  hooks = new FakeHooks();
  proc = new CommandProcessor(*store, *clock_, *sstore, *radio, "TEST", "0.1");
  sink = new RecordingSink();
  proc->set_hooks(hooks);
  proc->add_sink(sink);
  proc->boot();
}
void tearDown(void) {
  delete sink;
  delete proc;
  delete hooks;
  delete radio;
  delete sstore;
  delete clock_;
  delete raw_clock;
  delete store;
}

/** Board loop tick: advance the millis source and let the core observe it. */
static void tick(uint32_t ms) {
  raw_clock->advance(ms);
  proc->poll();
}

/** Serialize a request envelope and hand it to the core. */
static void send(const ClientToRadioMessage& req, IFrameSink* from) {
  uint8_t buf[1024];
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  TEST_ASSERT_TRUE_MESSAGE(pb_encode(&stream, ClientToRadioMessage_fields, &req),
                           "request encode failed");
  proc->on_envelope(buf, stream.bytes_written, from);
}

/** Zero-initialized request with the id set, filled by the caller. */
static ClientToRadioMessage request(uint32_t id) {
  ClientToRadioMessage req = ClientToRadioMessage_init_zero;
  req.id = id;
  return req;
}

static void set_str(char* dst, size_t cap, const char* src) {
  memset(dst, 0, cap);
  strncpy(dst, src, cap - 1);
}

// ---- tests: framing -----------------------------------------------------------

void test_cobs_roundtrip() {
  const uint8_t src[] = {0x01, 0x00, 0x02, 0x03, 0x00, 0xFF, 0x05};
  uint8_t enc[64];
  uint8_t dec[64];
  size_t n = cobs_encode(enc, src, sizeof(src));
  size_t m = cobs_decode(dec, enc, n, sizeof(dec));
  TEST_ASSERT_EQUAL(sizeof(src), m);
  TEST_ASSERT_EQUAL_MEMORY(src, dec, sizeof(src));
}

void test_cobs_empty() {
  uint8_t enc[4];
  uint8_t dec[4];
  size_t n = cobs_encode(enc, nullptr, 0);
  TEST_ASSERT_EQUAL(1, n);
  TEST_ASSERT_EQUAL(0, cobs_decode(dec, enc, n, sizeof(dec)));  // empty is not a frame here
}

void test_cobs_decode_respects_the_destination_cap() {
  // A COBS body expands by up to 3 bytes over the decoded length, so an
  // unbounded decode writes past the caller's frame buffer. The cap is what
  // makes an attacker-sized frame a drop instead of memory corruption.
  uint8_t src[600];
  memset(src, 0, sizeof(src));  // all zeros: COBS's worst-case expansion
  uint8_t enc[640];
  size_t n = cobs_encode(enc, src, sizeof(src));
  TEST_ASSERT_TRUE(n > sizeof(src));  // the body really is longer than the data

  struct {
    uint8_t buf[256];
    uint8_t canary[8];
  } guarded;
  memset(guarded.canary, 0x5A, sizeof(guarded.canary));
  TEST_ASSERT_EQUAL(0, cobs_decode(guarded.buf, enc, n, sizeof(guarded.buf)));
  for (size_t i = 0; i < sizeof(guarded.canary); i++) {
    TEST_ASSERT_EQUAL(0x5A, guarded.canary[i]);
  }
  // One byte more of room and the whole thing decodes.
  uint8_t big[600];
  TEST_ASSERT_EQUAL(sizeof(src), cobs_decode(big, enc, n, sizeof(big)));
}

void test_frame_reader_drops_a_body_longer_than_any_frame() {
  // The reader buffers up to FRAME_MAX_WIRE bytes, which is more than
  // FrameReader can hand on: a decoded frame is capped at
  // FRAME_MAX_DECODED. Such a frame must be dropped, not decoded past the
  // caller's buffer.
  uint8_t dec[FRAME_MAX_DECODED + 8];
  size_t dlen = frame_build(dec, nullptr, FRAME_MAX_DECODED - 1);  // 515 bytes
  TEST_ASSERT_EQUAL(FRAME_MAX_DECODED + 1, dlen);
  uint8_t wire[FRAME_MAX_WIRE];
  size_t wl = frame_encode_wire(wire, dec, dlen);
  TEST_ASSERT_TRUE(wl <= FRAME_MAX_WIRE);  // it does fit the wire buffer

  struct {
    uint8_t buf[FRAME_MAX_DECODED];
    uint8_t canary[8];
  } guarded;
  memset(guarded.canary, 0x5A, sizeof(guarded.canary));
  FrameReader r;
  size_t res = 0;
  for (size_t i = 0; i < wl; i++) res = r.feed(wire[i], guarded.buf);
  TEST_ASSERT_EQUAL((size_t)-1, res);
  for (size_t i = 0; i < sizeof(guarded.canary); i++) {
    TEST_ASSERT_EQUAL(0x5A, guarded.canary[i]);
  }
  // ...and the reader recovers: the next good frame still decodes.
  uint8_t good[16];
  size_t glen = frame_build(good, nullptr, 0);
  uint8_t gwire[FRAME_MAX_WIRE];
  size_t gwl = frame_encode_wire(gwire, good, glen);
  for (size_t i = 0; i < gwl; i++) res = r.feed(gwire[i], guarded.buf);
  TEST_ASSERT_EQUAL(0, res);
}

void test_frame_roundtrip_and_crc() {
  uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  uint8_t built[16];
  size_t n = frame_build(built, payload, 4);  // envelope + crc
  TEST_ASSERT_EQUAL(6, n);
  // frame_encode_envelope must produce byte-identical output: it is the same
  // function the transports use, so fixtures cannot drift from the wire.
  uint8_t a[FRAME_MAX_WIRE], b[FRAME_MAX_WIRE];
  size_t wa = frame_encode_envelope(a, payload, 4);
  size_t wb = frame_encode_wire(b, built, n);
  TEST_ASSERT_EQUAL(wa, wb);
  TEST_ASSERT_EQUAL_MEMORY(a, b, wa);
  // Corrupt one byte -> frame_crc differs
  uint8_t copy[16];
  memcpy(copy, built, n);
  copy[0] ^= 0xFF;
  TEST_ASSERT_NOT_EQUAL(frame_crc(copy, n),
                        (uint16_t)(copy[n - 2] | (copy[n - 1] << 8)));
}

void test_frame_reader_stream() {
  uint8_t payload[] = {1, 2, 3};
  uint8_t built[16];
  size_t n = frame_build(built, payload, 3);
  uint8_t wire[FRAME_MAX_WIRE];
  size_t wl = frame_encode_wire(wire, built, n);

  FrameReader r;
  uint8_t out[FRAME_MAX_DECODED];
  size_t got = 0;
  for (size_t i = 0; i < wl; i++) {
    size_t res = r.feed(wire[i], out);
    if (res != 0 && res != (size_t)-1) got = res;
  }
  // the CRC is verified and stripped: what comes out is the payload
  TEST_ASSERT_EQUAL(n - 2, got);
  TEST_ASSERT_EQUAL_MEMORY(payload, out, 3);
}

void test_frame_reader_bad_crc_dropped() {
  uint8_t built[16];
  size_t n = frame_build(built, nullptr, 0);
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
  size_t n = frame_build(built, nullptr, 0);
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
  TEST_ASSERT_EQUAL(n - 2, second);      // good frame decoded
}

// ---- tests: packet store ------------------------------------------------------

// Byte cost of one retained packet of the given payload length.
static uint32_t rec_b(uint8_t len) { return 16 + len; }

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
  uint32_t s1 = s.append(0, -70, 9, 0x02, big, 30);  // 46 bytes
  s.append(0, -70, 9, 0x02, one, 1);                 // 17 bytes
  uint32_t s3 = s.append(0, -70, 9, 0x02, mid, 40);  // 56 bytes
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(rec_b(30) + rec_b(1) + rec_b(40), s.bytes_used());

  // a 4th packet that does not fit evicts exactly the oldest one
  uint32_t s4 = s.append(0, -70, 9, 0x02, small, 20);  // 36 bytes
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
  PacketStore s(80);
  uint8_t one[1] = {0x01}, long_[20];
  memset(long_, 0x22, sizeof(long_));
  s.append(0, -70, 9, 0x02, one, 1);    // seq 1: [0..17)
  s.append(0, -70, 9, 0x02, one, 1);    // seq 2: [17..34)
  s.append(0, -70, 9, 0x02, long_, 20); // seq 3: [34..70), 10 bytes left
  TEST_ASSERT_EQUAL(3, s.count());

  // 17 bytes do not fit at the tail: evict seq 1, wrap tail to the front
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
  s.append(0, -70, 9, 0x02, one, 1);  // seq 5: fills the hole at [17..34)
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(6, s.next_seq());
  TEST_ASSERT_EQUAL(2, s.dropped());
  TEST_ASSERT_EQUAL(3, s.oldest_seq());
  s.append(0, -70, 9, 0x02, one, 1);  // seq 6: evicts seq 3, ring goes linear
  TEST_ASSERT_EQUAL(3, s.count());
  TEST_ASSERT_EQUAL(3, s.dropped());
  TEST_ASSERT_EQUAL(4, s.oldest_seq());
  TEST_ASSERT_TRUE(s.get(6, &e));
}

void test_store_oversize_packet_never_fits() {
  PacketStore s(64);  // smaller than 16 + 255
  uint8_t big[MESHPIGEON_MAX_RAW_PACKET];
  memset(big, 0x77, sizeof(big));
  TEST_ASSERT_EQUAL(0, s.append(0, -70, 9, 0x02, big, MESHPIGEON_MAX_RAW_PACKET));
  TEST_ASSERT_EQUAL(0, s.count());
  TEST_ASSERT_EQUAL(1, s.dropped());
  TEST_ASSERT_EQUAL(1, s.next_seq());  // no seq consumed by the drop
  TEST_ASSERT_EQUAL(0, s.count_since(0));
}

void test_store_max_size_packet_roundtrip() {
  PacketStore s(rec_b(MESHPIGEON_MAX_RAW_PACKET));
  uint8_t big[MESHPIGEON_MAX_RAW_PACKET];
  for (int i = 0; i < MESHPIGEON_MAX_RAW_PACKET; i++) big[i] = (uint8_t)i;
  uint32_t sq = s.append(0x1FFFFFFFFULL, -100, -5, 0x01, big,
                         MESHPIGEON_MAX_RAW_PACKET);
  TEST_ASSERT_EQUAL(1, sq);
  StoredPacket e;
  TEST_ASSERT_TRUE(s.get(sq, &e));
  TEST_ASSERT_EQUAL(MESHPIGEON_MAX_RAW_PACKET, e.len);
  TEST_ASSERT_EQUAL_MEMORY(big, e.raw, MESHPIGEON_MAX_RAW_PACKET);
  TEST_ASSERT_EQUAL(-100, e.rssi);
  TEST_ASSERT_EQUAL(0x1FFFFFFFFULL, e.uptime_ms);  // 64-bit survives the ring
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
  std::vector<uint32_t> seqs;  // model: retained seqs, oldest first
  uint8_t payload[MESHPIGEON_MAX_RAW_PACKET];
  for (int i = 0; i < (int)sizeof(payload); i++) payload[i] = (uint8_t)(i * 7);
  uint64_t rng = 0x12345678;

  for (int op = 0; op < 5000; op++) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    uint8_t len = (uint8_t)(1 + (rng >> 33) % MESHPIGEON_MAX_RAW_PACKET);
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

void test_device_settings_defaults_and_validation() {
  DeviceSettings d = DeviceSettings::defaults();
  TEST_ASSERT_TRUE(d.pin_is_default());
  TEST_ASSERT_EQUAL(MESHPIGEON_WIFI_PORT_DEFAULT, d.wifi_port);
  TEST_ASSERT_FALSE(d.wifi_enabled);
  TEST_ASSERT_TRUE(DeviceSettings::valid_pin("1234", 4));
  TEST_ASSERT_TRUE(DeviceSettings::valid_pin("12345678", 8));
  TEST_ASSERT_FALSE(DeviceSettings::valid_pin("123", 3));    // too short
  TEST_ASSERT_FALSE(DeviceSettings::valid_pin("123456789", 9));  // too long
  TEST_ASSERT_FALSE(DeviceSettings::valid_pin("12a4", 4));   // not digits
  TEST_ASSERT_FALSE(DeviceSettings::valid_port(0));
  d.clear();
  TEST_ASSERT_TRUE(d.pin_is_default());
}

// ---- tests: envelopes ----------------------------------------------------------

void test_ping_echoes() {
  ClientToRadioMessage req = request(0x11);
  req.which_body = kOpPing;
  const uint8_t payload[] = {0xAB, 0xCD};
  req.body.ping.payload.size = sizeof(payload);
  memcpy(req.body.ping.payload.bytes, payload, sizeof(payload));
  send(req, sink);

  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(0, sink->undecoded());
  TEST_ASSERT_EQUAL(0x11, sink->at(0).id());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_pong_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL_MEMORY(payload, sink->at(0).m().body.pong.payload.bytes, 2);
}

void test_ping_skips_unknown_fields() {
  // A newer client appends a field this firmware predates: unknown fields
  // are skipped, the PING still answers (evolution policy).
  ClientToRadioMessage req = request(7);
  req.which_body = kOpPing;
  send(req, sink);
  TEST_ASSERT_EQUAL(1, sink->count());

  uint8_t buf[64];
  pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
  TEST_ASSERT_TRUE(pb_encode(&stream, ClientToRadioMessage_fields, &req));
  size_t n = stream.bytes_written;
  const uint8_t junk[] = {0x98, 0x06, 0x01};  // field 99, varint 1
  memcpy(buf + n, junk, sizeof(junk));
  proc->on_envelope(buf, n + sizeof(junk), sink);
  TEST_ASSERT_EQUAL(2, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_pong_tag, sink->at(1).which());
}

void test_unknown_operation_errors() {
  // Field 20 with an empty message: an operation this firmware predates.
  const uint8_t frame[] = {0x08, 0x01, 0xA2, 0x01, 0x00};
  proc->on_envelope(frame, sizeof(frame), sink);
  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_error_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_COMMAND,
                    sink->at(0).error());
}

void test_sink_registry_is_bounded_and_shared() {
  // The registry is one pool for every transport (docs/radio-protocol.md §3),
  // so add_sink has to be able to say no: a transport that ignored the
  // result would broadcast into the void.
  RecordingSink extra[8];
  size_t accepted = 0;
  for (size_t i = 0; i < 8; i++) {
    if (proc->add_sink(&extra[i])) accepted++;
  }
  TEST_ASSERT_EQUAL(5, accepted);  // one was taken by setUp()'s sink
  TEST_ASSERT_EQUAL(0, proc->sink_free());
  for (size_t i = 0; i < 5; i++) {
    proc->remove_sink(&extra[i]);
  }
  TEST_ASSERT_EQUAL(1, proc->sink_count());
}

void test_malformed_or_unidentified_envelope_dropped() {
  uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF};
  proc->on_envelope(garbage, sizeof(garbage), sink);
  // A decodable envelope with no id cannot be answered: silent drop.
  ClientToRadioMessage req = request(0);
  req.which_body = kOpPing;
  send(req, sink);
  TEST_ASSERT_EQUAL(0, sink->count());
}

// ---- tests: radio settings ------------------------------------------------------

void test_boot_uses_persisted_settings() {
  RadioSettings s = make_settings(7, 5);
  sstore->save(s);
  proc->boot();  // setUp already booted once
  TEST_ASSERT_EQUAL(2, radio->apply_calls_);
  TEST_ASSERT_TRUE(radio->applied_ == s);
}

void test_boot_first_time_uses_safe_default() {
  TEST_ASSERT_TRUE(radio->applied_ == RadioSettings::unset());
}

void test_get_radio_settings() {
  ClientToRadioMessage req = request(3);
  req.which_body = kOpGetRadioSettings;
  send(req, sink);
  TEST_ASSERT_EQUAL(1, sink->count());
  const RadioSettingsMessage& m = sink->at(0).m().body.radio_settings;
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_radio_settings_tag,
                    sink->at(0).which());
  TEST_ASSERT_EQUAL(3, sink->at(0).id());
  TEST_ASSERT_EQUAL(RadioSettings::unset().freq_hz, m.freq_hz);
  TEST_ASSERT_EQUAL(125000u, m.bandwidth_hz);  // 0.01 kHz units -> Hz
}

void test_set_radio_persists_and_applies() {
  ClientToRadioMessage req = request(0x22);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 906875000;
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  m.power_dbm = 22;
  send(req, sink);

  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_radio_settings_tag,
                    sink->at(0).which());
  const RadioSettingsMessage& echo = sink->at(0).m().body.radio_settings;
  TEST_ASSERT_EQUAL(906875000u, echo.freq_hz);
  TEST_ASSERT_EQUAL(1, echo.config_epoch);  // bumped by the firmware
  // the radio saw the tuning as requested; the firmware then bumped the epoch
  TEST_ASSERT_EQUAL(906875000u, radio->applied_.freq_hz);
  TEST_ASSERT_EQUAL(9, radio->applied_.sf);
  TEST_ASSERT_EQUAL(1, proc->settings().config_epoch);
  // applied to the radio, with the internal units
  TEST_ASSERT_EQUAL(12500, radio->applied_.bw_x100khz);
  TEST_ASSERT_EQUAL(0, radio->applied_.region);  // regions are gone in v2
  // persisted for next boot
  RadioSettings loaded;
  TEST_ASSERT_TRUE(sstore->load(&loaded));
  TEST_ASSERT_TRUE(loaded == proc->settings());  // including the bumped epoch
}

void test_set_radio_bad_payload() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 0;  // no frequency
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
  // SF out of range is rejected the same way
  sink->clear();
  req.id = 2;
  m.freq_hz = 906875000;
  m.sf = 13;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
  // So is a bandwidth that is not a whole 10 Hz step: the internal field is
  // in 0.01 kHz, so converting first wrapped 655370 Hz into 10 Hz and
  // "accepted" it.
  sink->clear();
  req.id = 3;
  m.sf = 9;
  m.bandwidth_hz = 125005;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
  sink->clear();
  req.id = 4;
  m.bandwidth_hz = 655370;  // /10 == 65537, which wraps the uint16 field
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
  TEST_ASSERT_EQUAL(12500, proc->settings().bw_x100khz);  // default kept
}

void test_set_radio_apply_failure_is_tx_failed() {
  radio->apply_ok_ = false;
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 906875000;
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_TX_FAILED,
                    sink->at(0).error());
  TEST_ASSERT_EQUAL(0, proc->settings().config_epoch);  // nothing applied
}

void test_rejected_retune_leaves_the_radio_usable() {
  // A radio that refuses one tuning still holds the previous one, which is
  // in force and persisted — so the rejection must not cost the device its
  // air until the next reboot. Only a failure at boot() clears radio_ok.
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 906875000;
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  radio->apply_ok_ = false;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_TX_FAILED,
                    sink->at(0).error());
  TEST_ASSERT_TRUE(proc->radio_ok());

  sink->clear();
  ClientToRadioMessage tx = request(2);
  tx.which_body = kOpSendPacket;
  tx.body.send_packet.raw.size = 1;
  tx.body.send_packet.raw.bytes[0] = 0x45;
  send(tx, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_accepted_tag,
                    sink->at(0).which());
  TEST_ASSERT_EQUAL(1, radio->tx_calls_);

  // The successful retune is what makes a radio that failed at boot usable.
  radio->apply_ok_ = true;
  sink->clear();
  proc->boot();
  radio->apply_ok_ = false;
  sink->clear();
  req.id = 3;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_TX_FAILED,
                    sink->at(0).error());
  TEST_ASSERT_TRUE(proc->radio_ok());
}

void test_set_radio_without_settings_is_bad_payload() {
  // nanopb decodes an absent submessage as an empty one: without an explicit
  // has_settings check there is no way to tell it from "all defaults".
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
}

void test_first_owner_lock_honors_only_first_set() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 906875000;
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_radio_settings_tag,
                    sink->at(0).which());

  // second client tries within the 5-minute grace window
  RecordingSink other;
  proc->add_sink(&other);
  req.id = 2;
  send(req, &other);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, other.at(0).error());

  // after the grace window, re-tuning is allowed again
  tick(6 * 60 * 1000);
  req.id = 3;
  send(req, &other);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_radio_settings_tag, other.at(1).which());
  proc->remove_sink(&other);
}

void test_radio_changed_broadcast_to_others() {
  RecordingSink other;
  proc->add_sink(&other);
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetRadioSettings;
  req.body.set_radio_settings.has_settings = true;
  RadioSettingsMessage& m = req.body.set_radio_settings.settings;
  m.freq_hz = 906875000;
  m.bandwidth_hz = 125000;
  m.sf = 9;
  m.cr = 5;
  send(req, sink);

  // the originator gets its own echo only; the other client gets the push
  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(1, other.count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_radio_settings_tag, other.at(0).which());
  TEST_ASSERT_EQUAL(0, other.at(0).id());  // async push
  TEST_ASSERT_EQUAL(1, other.at(0).m().body.radio_settings.config_epoch);
  proc->remove_sink(&other);
}

// ---- tests: packet store over the wire ------------------------------------------

void test_store_append_reports_the_stored_entry() {
  // append() hands back exactly what landed in the ring, so the receive hot
  // path never has to walk the retained range to reach the newest entry.
  PacketStore s(rec_b(1) * 8);
  uint8_t data[3] = {0xDE, 0xAD, 0x01};
  StoredPacket e;
  for (int i = 0; i < 6; i++) {
    uint32_t sq = s.append(0x1122334455667788ULL + (uint64_t)i, -91, 7, 0x02,
                           data, sizeof(data), &e);
    TEST_ASSERT_EQUAL((uint32_t)(i + 1), sq);
    TEST_ASSERT_EQUAL(sq, e.seq);
    TEST_ASSERT_EQUAL(0x1122334455667788ULL + (uint64_t)i, e.uptime_ms);
    TEST_ASSERT_EQUAL(-91, e.rssi);
    TEST_ASSERT_EQUAL(7, e.snr);
    TEST_ASSERT_EQUAL(0x02, e.flags);
    TEST_ASSERT_EQUAL(sizeof(data), e.len);
    TEST_ASSERT_EQUAL_MEMORY(data, e.raw, sizeof(data));
  }
  // A drop consumes no seq and writes no entry.
  PacketStore tiny(8);  // cannot hold even one record
  TEST_ASSERT_EQUAL(0, tiny.append(0, -70, 9, 0x02, data, 3, &e));
}

void test_send_packet_refuses_a_store_that_cannot_take_it() {
  // A store with no room for the packet at all: seq 0 is the "nothing was
  // stored" sentinel, so accepting it would hand the app a seq it can never
  // fetch and then push a TxResult for it.
  delete store;
  store = new PacketStore(8);
  delete proc;
  proc = new CommandProcessor(*store, *clock_, *sstore, *radio, "TEST", "0.1");
  proc->set_hooks(hooks);
  proc->add_sink(sink);
  proc->boot();

  ClientToRadioMessage req = request(1);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = 8;
  memset(req.body.send_packet.raw.bytes, 0x45, 8);
  send(req, sink);

  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY,
                    sink->at(0).error());
  TEST_ASSERT_EQUAL(0, radio->tx_calls_);  // nothing keyed up
}

void test_send_packet_roundtrip() {
  radio->tx_async_ = true;  // TX in flight until poll()
  const uint8_t pkt[] = {0x45, 0x01, 0x02, 0x03, 0x04};
  ClientToRadioMessage req = request(0x33);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = sizeof(pkt);
  memcpy(req.body.send_packet.raw.bytes, pkt, sizeof(pkt));
  send(req, sink);

  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_accepted_tag, sink->at(0).which());
  uint32_t seq = sink->at(0).m().body.packet_accepted.seq;
  TEST_ASSERT_EQUAL(1, seq);
  TEST_ASSERT_EQUAL(1, radio->tx_calls_);
  TEST_ASSERT_EQUAL_MEMORY(pkt, radio->last_tx_, sizeof(pkt));
  TEST_ASSERT_EQUAL(1, sink->count());  // no TX_RESULT yet
  tick(0);                             // radio still busy
  TEST_ASSERT_EQUAL(1, sink->count());
  radio->complete_tx();
  tick(0);  // now TX completes
  TEST_ASSERT_EQUAL(2, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_tx_result_tag, sink->at(1).which());
  TEST_ASSERT_EQUAL(0, sink->at(1).id());  // async
  TEST_ASSERT_EQUAL(seq, sink->at(1).m().body.tx_result.seq);
  TEST_ASSERT_TRUE(sink->at(1).m().body.tx_result.success);
  // stored as sent
  StoredPacket e;
  TEST_ASSERT_TRUE(store->get(seq, &e));
  TEST_ASSERT_EQUAL(0x01, e.flags);
}

void test_send_packet_tx_failure_reports_result() {
  radio->tx_ok_ = false;  // radio refuses to start
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = 1;
  req.body.send_packet.raw.bytes[0] = 0x42;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_accepted_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(2, sink->count());  // immediate TX_RESULT
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_tx_result_tag, sink->at(1).which());
  TEST_ASSERT_FALSE(sink->at(1).m().body.tx_result.success);
}

void test_send_packet_busy_when_second_in_flight() {
  radio->tx_async_ = true;
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = 1;
  req.body.send_packet.raw.bytes[0] = 0x42;
  send(req, sink);
  req.id = 2;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, sink->at(1).error());
}

void test_send_packet_bad_length() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = 0;  // nothing to send
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
}

void test_send_packet_max_raw_size_accepted() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSendPacket;
  req.body.send_packet.raw.size = MESHPIGEON_MAX_RAW_PACKET;
  for (uint16_t i = 0; i < MESHPIGEON_MAX_RAW_PACKET; i++) {
    req.body.send_packet.raw.bytes[i] = (uint8_t)i;
  }
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_accepted_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(1, radio->tx_calls_);
}

void test_fetch_packets_streams_entries_and_end() {
  const uint8_t pkt[] = {0x45, 0x06};
  for (int i = 0; i < 3; i++) proc->on_packet_received(-72, 8, pkt, sizeof(pkt));
  sink->clear();
  ClientToRadioMessage req = request(0x55);
  req.which_body = kOpFetchPackets;
  req.body.fetch_packets.since_seq = 0;
  req.body.fetch_packets.max_count = 10;
  send(req, sink);

  // 3 entry frames then FETCH_END, in order, all echoing the request id
  TEST_ASSERT_EQUAL(4, sink->count());
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_entry_tag, sink->at(i).which());
    TEST_ASSERT_EQUAL(0x55, sink->at(i).id());
    TEST_ASSERT_EQUAL(i + 1, sink->at(i).m().body.packet_entry.seq);
    TEST_ASSERT_EQUAL(meshpigeon_PacketEntry_Origin_ORIGIN_RECEIVED,
                      sink->at(i).m().body.packet_entry.origin);
    TEST_ASSERT_EQUAL_MEMORY(pkt, sink->at(i).m().body.packet_entry.raw.bytes, 2);
  }
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_fetch_end_tag, sink->at(3).which());
  TEST_ASSERT_EQUAL(3, sink->at(3).m().body.fetch_end.count);
}

void test_fetch_packets_since_cursor_skips_old() {
  const uint8_t pkt[] = {0x45, 0x06};
  for (int i = 0; i < 3; i++) proc->on_packet_received(-72, 8, pkt, sizeof(pkt));
  sink->clear();
  ClientToRadioMessage req = request(0x56);
  req.which_body = kOpFetchPackets;
  req.body.fetch_packets.since_seq = 1;  // seqs 2,3
  req.body.fetch_packets.max_count = 10;
  send(req, sink);
  TEST_ASSERT_EQUAL(3, sink->count());  // 2 entries + END
  TEST_ASSERT_EQUAL(2, sink->at(2).m().body.fetch_end.count);
  TEST_ASSERT_EQUAL(2, sink->at(0).m().body.packet_entry.seq);
}

void test_fetch_packets_is_capped_per_request() {
  // One handler call streams straight out of the sink, so an unbounded
  // max_count would let a client hold the board loop (and a BLE queue) for
  // as long as the store is deep. The cap is visible as a short FetchEnd
  // and the client resumes from the last seq it got.
  const uint8_t pkt[] = {0x45};
  for (int i = 0; i < 200; i++) proc->on_packet_received(-80, 10, pkt, 1);

  ClientToRadioMessage req = request(1);
  req.which_body = kOpFetchPackets;
  req.body.fetch_packets.since_seq = 0;
  req.body.fetch_packets.max_count = 0xFFFFFFFF;
  sink->clear();
  send(req, sink);

  // 200 entries + one FetchEnd, and the stream stopped short of all 200.
  TEST_ASSERT_LESS_THAN(200, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_fetch_end_tag,
                    sink->at((int)sink->count() - 1).which());
  uint32_t delivered =
      sink->at((int)sink->count() - 1).m().body.fetch_end.count;
  TEST_ASSERT_LESS_THAN(200, delivered);
  TEST_ASSERT_EQUAL(0, sink->undecoded());

  // Resuming from the last seq continues where it stopped; the client keeps
  // re-issuing until FetchEnd delivers nothing.
  uint32_t last = 0;
  uint32_t total = 0;
  for (int round = 0; round < 10; round++) {
    sink->clear();
    req.id = (uint32_t)(2 + round);
    req.body.fetch_packets.since_seq = last;
    send(req, sink);
    uint32_t delivered =
        sink->at((int)sink->count() - 1).m().body.fetch_end.count;
    if (delivered == 0) break;
    // Contiguous: every round picks up at exactly the seq after the last.
    TEST_ASSERT_EQUAL(last + 1, sink->at(0).m().body.packet_entry.seq);
    last = sink->at((int)delivered - 1).m().body.packet_entry.seq;
    total += delivered;
  }
  TEST_ASSERT_EQUAL(200, total);
  TEST_ASSERT_EQUAL(0, sink->undecoded());
}

void test_rx_packet_pushes_to_all_sinks() {
  const uint8_t pkt[] = {0x45, 0x07, 0x08};
  RecordingSink a, b;
  proc->add_sink(&a);
  proc->add_sink(&b);
  proc->on_packet_received(-80, 10, pkt, sizeof(pkt));
  TEST_ASSERT_EQUAL(1, a.count());
  TEST_ASSERT_EQUAL(1, b.count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_packet_entry_tag, a.at(0).which());
  TEST_ASSERT_EQUAL(0, a.at(0).id());  // async
  TEST_ASSERT_EQUAL(1, a.at(0).m().body.packet_entry.seq);
  TEST_ASSERT_EQUAL(-80, a.at(0).m().body.packet_entry.rssi);
  TEST_ASSERT_EQUAL(10, a.at(0).m().body.packet_entry.snr);
  TEST_ASSERT_EQUAL(sizeof(pkt), a.at(0).m().body.packet_entry.raw.size);
  TEST_ASSERT_EQUAL_MEMORY(pkt, a.at(0).m().body.packet_entry.raw.bytes, 3);
  proc->remove_sink(&a);
  proc->remove_sink(&b);
}

void test_purge_store() {
  const uint8_t pkt[] = {0x45};
  proc->on_packet_received(-80, 10, pkt, 1);
  TEST_ASSERT_EQUAL(1, store->count());
  sink->clear();
  ClientToRadioMessage req = request(1);
  req.which_body = kOpPurgeStore;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(0, store->count());
}

// ---- tests: device identity, settings, auth -------------------------------------

void test_device_info() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetDeviceInfo;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_info_tag, sink->at(0).which());
  const DeviceInfoMessage& m = sink->at(0).m().body.device_info;
  TEST_ASSERT_EQUAL(MESHPIGEON_SPEC_VERSION, m.spec_version);
  TEST_ASSERT_EQUAL_STRING("TEST", m.board_name);
  TEST_ASSERT_EQUAL_STRING("0.1", m.fw_version);
  // The hook is asked for kMaxCapabilities slots, not zero: a board that
  // reported nothing here would take its Wi-Fi fields with it.
  TEST_ASSERT_EQUAL(2, m.capabilities_count);
  TEST_ASSERT_EQUAL(meshpigeon_Capability_CAPABILITY_BLE, m.capabilities[0]);
  TEST_ASSERT_EQUAL(meshpigeon_Capability_CAPABILITY_USB_CDC, m.capabilities[1]);
  TEST_ASSERT_EQUAL(0, store->count());
  TEST_ASSERT_EQUAL(store->capacity(), m.store.capacity_bytes);
  TEST_ASSERT_EQUAL(3900, m.battery_mv);
  TEST_ASSERT_TRUE(m.radio_ok);
  TEST_ASSERT_FALSE(m.auth_required);  // default PIN: no lock
  TEST_ASSERT_EQUAL(0, m.radio_config_epoch);
  // Nothing received yet => no noise-floor estimate.
  TEST_ASSERT_EQUAL(0, m.noise_floor_dbm);
  // store stats track reality
  const uint8_t pkt[] = {0x45};
  proc->on_packet_received(-80, 10, pkt, 1);
  sink->clear();
  send(req, sink);
  TEST_ASSERT_EQUAL(1, sink->at(0).m().body.device_info.store.count);
  TEST_ASSERT_EQUAL(1, sink->at(0).m().body.device_info.store.oldest_seq);
  // RSSI - SNR of the last packet is the noise floor estimate.
  TEST_ASSERT_EQUAL(-90, sink->at(0).m().body.device_info.noise_floor_dbm);
}

void test_device_info_radio_failure() {
  radio->apply_ok_ = false;
  proc->boot();
  sink->clear();
  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetDeviceInfo;
  send(req, sink);
  TEST_ASSERT_FALSE(sink->at(0).m().body.device_info.radio_ok);
  // A radio that never came up still loads its settings, so the app can
  // name the node and unlock it.
  TEST_ASSERT_TRUE(proc->device_settings().pin_is_default());
  TEST_ASSERT_EQUAL(MESHPIGEON_WIFI_PORT_DEFAULT,
                    proc->device_settings().wifi_port);
}

void test_boot_reports_a_dead_radio_and_still_loads_settings() {
  // The board port's begin() failing must not stop the core from booting:
  // radio_ok goes false, the stored settings are still in RAM, and sending
  // is refused instead of being silently "accepted".
  sstore->save_device([] {
    DeviceSettings d = DeviceSettings::defaults();
    strcpy(d.name, "shelf-pigeon");
    return d;
  }());
  radio->begin_ok_ = false;
  TEST_ASSERT_FALSE(proc->boot());
  TEST_ASSERT_FALSE(proc->radio_ok());
  TEST_ASSERT_EQUAL_STRING("shelf-pigeon", proc->device_settings().name);

  sink->clear();
  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetDeviceInfo;
  send(req, sink);
  TEST_ASSERT_FALSE(sink->at(0).m().body.device_info.radio_ok);

  sink->clear();
  ClientToRadioMessage tx = request(2);
  tx.which_body = kOpSendPacket;
  tx.body.send_packet.raw.size = 1;
  tx.body.send_packet.raw.bytes[0] = 0x45;
  send(tx, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_NO_RADIO,
                    sink->at(0).error());
  // Nothing was stored and nothing was keyed up: no TxResult lies either.
  TEST_ASSERT_EQUAL(0, store->count());
  TEST_ASSERT_EQUAL(0, radio->tx_calls_);
  TEST_ASSERT_EQUAL(0, sink->count() - 1);
}

void test_send_packet_survives_a_failed_apply() {
  // radio_ok is the gate on SendPacket, so a radio that could not come up at
  // boot must refuse sends rather than pretend to key up.
  radio->apply_ok_ = false;
  proc->boot();
  TEST_ASSERT_FALSE(proc->radio_ok());
  radio->apply_ok_ = true;

  sink->clear();
  ClientToRadioMessage tx = request(1);
  tx.which_body = kOpSendPacket;
  tx.body.send_packet.raw.size = 1;
  tx.body.send_packet.raw.bytes[0] = 0x45;
  send(tx, sink);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_NO_RADIO,
                    sink->at(0).error());
}

/** Set a PIN on the fixture and return a fresh, locked-down client sink. */
static RecordingSink* lock_with_pin(const char* pin) {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  req.body.set_device_settings.has_pin = true;
  set_str(req.body.set_device_settings.pin, sizeof(req.body.set_device_settings.pin),
          pin);
  send(req, sink);
  sink->clear();
  return sink;
}

void test_device_settings_defaults() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetDeviceSettings;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, sink->at(0).which());
  const DeviceSettingsMessage& m = sink->at(0).m().body.device_settings;
  // no stored name -> derived from the MAC suffix the hook provides
  TEST_ASSERT_EQUAL_STRING("MeshPigeon-A3F2", m.name);
  TEST_ASSERT_FALSE(m.wifi_enabled);
  TEST_ASSERT_EQUAL(MESHPIGEON_WIFI_PORT_DEFAULT, m.wifi_port);
  // the PIN is write-only: DeviceSettingsMessage has no field for it
  TEST_ASSERT_TRUE(DeviceSettings::defaults().pin_is_default());
}

void test_set_device_name_renames_and_broadcasts() {
  RecordingSink other;
  proc->add_sink(&other);
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  req.body.set_device_settings.has_name = true;
  set_str(req.body.set_device_settings.name,
          sizeof(req.body.set_device_settings.name), "attic-pigeon");
  send(req, sink);

  // the hook re-advertised, the name persisted...
  TEST_ASSERT_EQUAL(1, hooks->name_calls_);
  TEST_ASSERT_EQUAL_STRING("attic-pigeon", hooks->name_);
  TEST_ASSERT_EQUAL_STRING("attic-pigeon", proc->device_settings().name);
  DeviceSettings loaded;
  TEST_ASSERT_TRUE(sstore->load_device(&loaded));
  TEST_ASSERT_EQUAL_STRING("attic-pigeon", loaded.name);
  // ...the issuer got the full post-write read model...
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL_STRING("attic-pigeon",
                           sink->at(0).m().body.device_settings.name);
  // ...and the other client got the async change push
  TEST_ASSERT_EQUAL(1, other.count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, other.at(0).which());
  TEST_ASSERT_EQUAL(0, other.at(0).id());
  TEST_ASSERT_EQUAL_STRING("attic-pigeon",
                           other.at(0).m().body.device_settings.name);

  // an empty name resets to the derived default
  other.clear();
  req.id = 2;
  set_str(req.body.set_device_settings.name,
          sizeof(req.body.set_device_settings.name), "");
  send(req, sink);
  TEST_ASSERT_EQUAL(2, hooks->name_calls_);
  TEST_ASSERT_EQUAL_STRING("MeshPigeon-A3F2", hooks->name_);
  TEST_ASSERT_EQUAL_STRING("MeshPigeon-A3F2",
                           sink->at(1).m().body.device_settings.name);
  proc->remove_sink(&other);
}

void test_set_device_settings_wifi_applies_live() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  SetDeviceSettingsMessage& m = req.body.set_device_settings;
  m.has_wifi_enabled = true;
  m.wifi_enabled = true;
  m.has_wifi_ssid = true;
  set_str(m.wifi_ssid, sizeof(m.wifi_ssid), "home-net");
  m.has_wifi_password = true;
  set_str(m.wifi_password, sizeof(m.wifi_password), "hunter2");
  m.has_wifi_port = true;
  m.wifi_port = 5100;
  send(req, sink);

  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(1, hooks->apply_wifi_calls_);
  TEST_ASSERT_TRUE(hooks->wifi_.wifi_enabled);
  TEST_ASSERT_EQUAL_STRING("home-net", hooks->wifi_.wifi_ssid);
  const DeviceSettingsMessage& r = sink->at(0).m().body.device_settings;
  TEST_ASSERT_TRUE(r.wifi_enabled);
  TEST_ASSERT_EQUAL_STRING("home-net", r.wifi_ssid);
  TEST_ASSERT_EQUAL(5100, r.wifi_port);
}

void test_set_device_settings_is_atomic() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  SetDeviceSettingsMessage& m = req.body.set_device_settings;
  m.has_name = true;
  set_str(m.name, sizeof(m.name), "good-name");
  m.has_pin = true;
  set_str(m.pin, sizeof(m.pin), "12");  // too short
  send(req, sink);

  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                    sink->at(0).error());
  TEST_ASSERT_EQUAL(0, proc->device_settings().name[0]);  // nothing applied
  TEST_ASSERT_TRUE(proc->device_settings().pin_is_default());
  TEST_ASSERT_EQUAL(0, hooks->name_calls_);
}

void test_empty_pin_restores_the_factory_default() {
  // device.proto: "Empty string restores the default; absent = unchanged."
  lock_with_pin("1234");
  TEST_ASSERT_FALSE(proc->device_settings().pin_is_default());
  ClientToRadioMessage auth = request(90);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, sink);
  sink->clear();

  // absent => unchanged
  ClientToRadioMessage rename = request(1);
  rename.which_body = kOpSetDeviceSettings;
  rename.body.set_device_settings.has_name = true;
  set_str(rename.body.set_device_settings.name,
          sizeof(rename.body.set_device_settings.name), "kept");
  send(rename, sink);
  TEST_ASSERT_FALSE(proc->device_settings().pin_is_default());

  // empty => back to "0000", which also re-opens the device
  sink->clear();
  ClientToRadioMessage req = request(2);
  req.which_body = kOpSetDeviceSettings;
  req.body.set_device_settings.has_pin = true;
  set_str(req.body.set_device_settings.pin,
          sizeof(req.body.set_device_settings.pin), "");
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag,
                    sink->at(0).which());
  TEST_ASSERT_TRUE(proc->device_settings().pin_is_default());
  DeviceSettings loaded;
  TEST_ASSERT_TRUE(sstore->load_device(&loaded));
  TEST_ASSERT_TRUE(loaded.pin_is_default());
}

void test_device_settings_push_reaches_only_authorized_clients() {
  // The read model carries the Wi-Fi passphrase, so the async change push
  // must not be a free-for-all: a socket that merely attached would
  // otherwise collect the password from every rename.
  ClientToRadioMessage set = request(1);
  set.which_body = kOpSetDeviceSettings;
  set.body.set_device_settings.has_wifi_ssid = true;
  set_str(set.body.set_device_settings.wifi_ssid,
          sizeof(set.body.set_device_settings.wifi_ssid), "home-net");
  set.body.set_device_settings.has_wifi_password = true;
  set_str(set.body.set_device_settings.wifi_password,
          sizeof(set.body.set_device_settings.wifi_password), "hunter2");
  send(set, sink);
  proc->remove_sink(sink);  // the issuer goes away

  // A locked device: an unauthenticated socket hears nothing.
  lock_with_pin("1234");
  ClientToRadioMessage auth = request(90);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, sink);  // the writer is inside; the stranger is not
  sink->clear();

  RecordingSink stranger;
  proc->add_sink(&stranger);
  ClientToRadioMessage rename = request(2);
  rename.which_body = kOpSetDeviceSettings;
  rename.body.set_device_settings.has_name = true;
  set_str(rename.body.set_device_settings.name,
          sizeof(rename.body.set_device_settings.name), "still-here");
  send(rename, sink);
  TEST_ASSERT_EQUAL(0, stranger.count());

  // Once it authenticates, the same client is in the loop again.
  ClientToRadioMessage stranger_auth = request(3);
  stranger_auth.which_body = kOpAuth;
  set_str(stranger_auth.body.auth.pin, sizeof(stranger_auth.body.auth.pin),
          "1234");
  send(stranger_auth, &stranger);
  stranger.clear();
  rename.id = 4;
  send(rename, sink);
  TEST_ASSERT_EQUAL(1, stranger.count());
  TEST_ASSERT_EQUAL_STRING("hunter2",
                           stranger.at(0).m().body.device_settings.wifi_password);
  proc->remove_sink(&stranger);
  proc->add_sink(sink);
}

void test_changing_the_pin_re_gates_other_connections() {
  lock_with_pin("1234");
  RecordingSink peer;
  proc->add_sink(&peer);
  ClientToRadioMessage auth = request(1);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, sink);   // the holder
  send(auth, &peer);  // ...and a peer that knew the old PIN

  ClientToRadioMessage req = request(2);
  req.which_body = kOpSetDeviceSettings;
  req.body.set_device_settings.has_pin = true;
  set_str(req.body.set_device_settings.pin,
          sizeof(req.body.set_device_settings.pin), "5555");
  send(req, sink);

  // The peer knew the old PIN, so it is no longer entitled to the node; the
  // issuer keeps its session (it made the change).
  peer.clear();
  ClientToRadioMessage get = request(3);
  get.which_body = kOpGetDeviceSettings;
  send(get, &peer);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                    peer.at(0).error());
  sink->clear();
  send(get, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag,
                    sink->at(0).which());
  proc->remove_sink(&peer);
}

void test_max_length_strings_survive_the_wire() {
  // nanopb counts the NUL inside a string's buffer, so a cap of exactly the
  // documented length silently truncated every maximum-size value. This
  // round-trips the limits through real encode/decode, so a future edit to
  // the *.options caps fails here rather than on a device.
  TEST_ASSERT_TRUE(DeviceSettings::valid_name("01234567890123456789", 20));
  TEST_ASSERT_TRUE(DeviceSettings::valid_ssid("01234567890123456789012345678901",
                                             32));
  TEST_ASSERT_TRUE(DeviceSettings::valid_password(
      "012345678901234567890123456789012345678901234567890123456789012", 63));
  TEST_ASSERT_TRUE(DeviceSettings::valid_pin("12345678", 8));

  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  SetDeviceSettingsMessage& m = req.body.set_device_settings;
  m.has_name = true;
  set_str(m.name, sizeof(m.name), "01234567890123456789");
  m.has_wifi_ssid = true;
  set_str(m.wifi_ssid, sizeof(m.wifi_ssid),
          "01234567890123456789012345678901");
  m.has_wifi_password = true;
  set_str(m.wifi_password, sizeof(m.wifi_password),
          "012345678901234567890123456789012345678901234567890123456789012");
  m.has_pin = true;
  set_str(m.pin, sizeof(m.pin), "12345678");
  send(req, sink);

  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag,
                    sink->at(0).which());
  const DeviceSettingsMessage& r = sink->at(0).m().body.device_settings;
  TEST_ASSERT_EQUAL_STRING("01234567890123456789", r.name);
  TEST_ASSERT_EQUAL_STRING("01234567890123456789012345678901", r.wifi_ssid);
  TEST_ASSERT_EQUAL_STRING(
      "012345678901234567890123456789012345678901234567890123456789012",
      r.wifi_password);
  // ...and the 8-digit PIN round-tripped through Auth.
  DeviceSettings stored;
  TEST_ASSERT_TRUE(sstore->load_device(&stored));
  TEST_ASSERT_EQUAL_STRING("12345678", stored.pin);
  sink->clear();
  ClientToRadioMessage auth = request(2);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "12345678");
  send(auth, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
}

void test_set_device_settings_capability_gate() {
  hooks->wifi_supported_ = false;  // a board with no radio at all
  ClientToRadioMessage req = request(1);
  req.which_body = kOpSetDeviceSettings;
  SetDeviceSettingsMessage& m = req.body.set_device_settings;
  m.has_name = true;
  set_str(m.name, sizeof(m.name), "shed-pigeon");
  m.has_wifi_enabled = true;
  send(req, sink);

  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                    sink->at(0).error());
  TEST_ASSERT_EQUAL(0, proc->device_settings().name[0]);  // atomic: nothing
  TEST_ASSERT_FALSE(proc->device_settings().wifi_enabled);
  TEST_ASSERT_EQUAL(0, hooks->apply_wifi_calls_);
}

void test_auth_gate() {
  lock_with_pin("1234");

  // a second connection starts unauthenticated
  RecordingSink client;
  proc->add_sink(&client);

  // gated: the radio settings, the device settings, and every write
  struct {
    pb_size_t op;
    const char* what;
  } gated[] = {
      {kOpGetRadioSettings, "get_radio_settings"},
      {kOpGetDeviceSettings, "get_device_settings"},
      {kOpSetDeviceSettings, "set_device_settings"},
      {kOpSetRadioSettings, "set_radio_settings"},
      {kOpSendPacket, "send_packet"},
      {kOpFetchPackets, "fetch_packets"},
      {kOpPurgeStore, "purge_store"},
      {kOpReboot, "reboot"},
      {kOpFactoryReset, "factory_reset"},
  };
  for (size_t i = 0; i < sizeof(gated) / sizeof(gated[0]); i++) {
    client.clear();
    ClientToRadioMessage req = request(10 + (uint32_t)i);
    req.which_body = gated[i].op;
    send(req, &client);
    TEST_ASSERT_EQUAL_MESSAGE(
        meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED, client.at(0).error(),
        gated[i].what);
  }

  // exempt: ping, device info, status, bootloader, and auth itself
  client.clear();
  ClientToRadioMessage ping = request(50);
  ping.which_body = kOpPing;
  send(ping, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_pong_tag, client.at(0).which());
  client.clear();
  ClientToRadioMessage info = request(51);
  info.which_body = kOpGetDeviceInfo;
  send(info, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_info_tag, client.at(0).which());
  TEST_ASSERT_TRUE(client.at(0).m().body.device_info.auth_required);
  client.clear();
  ClientToRadioMessage status = request(52);
  status.which_body = kOpGetStatus;
  send(status, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_status_tag, client.at(0).which());
  client.clear();
  ClientToRadioMessage boot = request(53);
  boot.which_body = kOpBootloader;
  send(boot, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, client.at(0).which());

  // wrong PIN: rejected
  client.clear();
  ClientToRadioMessage auth = request(54);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "9999");
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                    client.at(0).error());

  // right PIN: unlocked, and the lock is gone from the info message
  client.clear();
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, client.at(0).which());
  client.clear();
  send(info, &client);
  TEST_ASSERT_FALSE(client.at(0).m().body.device_info.auth_required);
  // ...and the operations work now
  client.clear();
  ClientToRadioMessage get = request(55);
  get.which_body = kOpGetDeviceSettings;
  send(get, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, client.at(0).which());
  proc->remove_sink(&client);
}

void test_auth_is_per_connection() {
  lock_with_pin("1234");
  RecordingSink a, b;
  proc->add_sink(&a);
  proc->add_sink(&b);
  ClientToRadioMessage auth = request(1);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, &a);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, a.at(0).which());
  b.clear();
  ClientToRadioMessage get = request(2);
  get.which_body = kOpGetDeviceSettings;
  send(get, &b);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                    b.at(0).error());
  proc->remove_sink(&a);
  proc->remove_sink(&b);
}

void test_auth_rate_limit() {
  lock_with_pin("1234");
  RecordingSink client;
  proc->add_sink(&client);

  ClientToRadioMessage auth = request(1);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "0000");

  // three failures are free
  for (int i = 0; i < 3; i++) {
    client.clear();
    send(auth, &client);
    TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                      client.at(0).error());
  }
  // the fourth opens a 1 s window: even the right PIN is not evaluated
  client.clear();
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                    client.at(0).error());
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  client.clear();
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                    client.at(0).error());
  // once the window passes, the right PIN works
  tick(1001);
  client.clear();
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, client.at(0).which());
  proc->remove_sink(&client);
}

void test_setting_a_new_pin_clears_the_lock_and_budget() {
  lock_with_pin("1234");
  // the holder authenticates first...
  ClientToRadioMessage auth = request(1);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
  // ...while a stranger burns the brute-force budget
  RecordingSink client;
  proc->add_sink(&client);
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "0000");
  for (int i = 0; i < 4; i++) send(auth, &client);  // opens a penalty window

  // the holder authenticates, then rotates the PIN
  sink->clear();
  ClientToRadioMessage req = request(2);
  req.which_body = kOpSetDeviceSettings;
  req.body.set_device_settings.has_pin = true;
  set_str(req.body.set_device_settings.pin, sizeof(req.body.set_device_settings.pin),
          "5555");
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_settings_tag, sink->at(0).which());

  // the new PIN is accepted immediately: no penalty carries over
  client.clear();
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "5555");
  send(auth, &client);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, client.at(0).which());
  proc->remove_sink(&client);
}

void test_factory_reset() {
  const uint8_t pkt[] = {0x45};
  proc->on_packet_received(-80, 10, pkt, 1);
  ClientToRadioMessage set = request(1);
  set.which_body = kOpSetDeviceSettings;
  set.body.set_device_settings.has_name = true;
  set_str(set.body.set_device_settings.name,
          sizeof(set.body.set_device_settings.name), "shed-pigeon");
  set.body.set_device_settings.has_pin = true;
  set_str(set.body.set_device_settings.pin, sizeof(set.body.set_device_settings.pin),
          "1234");
  send(set, sink);
  TEST_ASSERT_EQUAL(1, store->count());

  // the PIN now locks the device: authenticate before the reset
  sink->clear();
  ClientToRadioMessage auth = request(2);
  auth.which_body = kOpAuth;
  set_str(auth.body.auth.pin, sizeof(auth.body.auth.pin), "1234");
  send(auth, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());

  sink->clear();
  ClientToRadioMessage req = request(3);
  req.which_body = kOpFactoryReset;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(1, hooks->factory_reset_calls_);
  TEST_ASSERT_TRUE(proc->device_settings().pin_is_default());
  TEST_ASSERT_EQUAL(0, proc->device_settings().name[0]);
  TEST_ASSERT_EQUAL(0, store->count());
  // The record is forgotten, not overwritten with defaults: the next boot
  // reads "never written" and falls back itself.
  DeviceSettings loaded;
  TEST_ASSERT_FALSE(sstore->load_device(&loaded));
  TEST_ASSERT_TRUE(loaded == DeviceSettings::defaults());
}

void test_reboot_and_bootloader() {
  ClientToRadioMessage boot = request(1);
  boot.which_body = kOpBootloader;
  send(boot, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(1, hooks->bootloader_calls_);

  sink->clear();
  ClientToRadioMessage req = request(2);
  req.which_body = kOpReboot;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_ok_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(1, hooks->reboot_calls_);
}

void test_status_reads_the_hooks() {
  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetStatus;
  send(req, sink);
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_status_tag, sink->at(0).which());
  const StatusMessage& m = sink->at(0).m().body.status;
  TEST_ASSERT_EQUAL(meshpigeon_Status_WifiState_WIFI_STATE_CONNECTED,
                    m.wifi_state);
  TEST_ASSERT_EQUAL_STRING("testnet", m.wifi_ssid);
  TEST_ASSERT_EQUAL(4, m.wifi_ipv4.size);
  TEST_ASSERT_EQUAL(192, m.wifi_ipv4.bytes[0]);
  TEST_ASSERT_EQUAL(9, m.wifi_ipv4.bytes[3]);
  TEST_ASSERT_EQUAL(5000, m.wifi_port);
  TEST_ASSERT_EQUAL(-61, m.wifi_rssi);
  TEST_ASSERT_EQUAL(3, m.ble_clients);
  TEST_ASSERT_EQUAL(1, m.usb_cdc_clients);
  TEST_ASSERT_EQUAL(2, m.wifi_tcp_clients);
}

void test_status_pushed_on_wifi_transition() {
  sink->clear();
  proc->on_wifi_state_changed();
  TEST_ASSERT_EQUAL(1, sink->count());
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_status_tag, sink->at(0).which());
  TEST_ASSERT_EQUAL(0, sink->at(0).id());  // async push
  TEST_ASSERT_EQUAL(3, sink->at(0).m().body.status.ble_clients);
}

void test_status_never_inherits_the_previous_response() {
  // IBoardHooks::fill_status only owes the core the fields the board knows
  // about; a board with no Wi-Fi fills the state and nothing else. The rest
  // of the Status must read as "unknown", whatever the previous response
  // left in response_.body — the builder zeroes the body it fills, so that
  // does not depend on every call site remembering to reset response_.
  ClientToRadioMessage info = request(1);
  info.which_body = kOpGetDeviceInfo;
  send(info, sink);  // the biggest response the core builds
  TEST_ASSERT_EQUAL(meshpigeon_RadioToClient_device_info_tag,
                    sink->at(0).which());

  // A hook that reports only the state, nothing else.
  hooks->status_sparse_ = true;
  sink->clear();
  ClientToRadioMessage status = request(2);
  status.which_body = kOpGetStatus;
  send(status, sink);
  const StatusMessage& m = sink->at(0).m().body.status;
  TEST_ASSERT_EQUAL(meshpigeon_Status_WifiState_WIFI_STATE_OFF, m.wifi_state);
  TEST_ASSERT_EQUAL_STRING("", m.wifi_ssid);
  TEST_ASSERT_EQUAL(0, m.wifi_ipv4.size);
  TEST_ASSERT_EQUAL(0, m.wifi_port);
  TEST_ASSERT_EQUAL(0, m.wifi_rssi);
  // The counts are the core's own and are always written.
  TEST_ASSERT_EQUAL(3, m.ble_clients);
  TEST_ASSERT_EQUAL(1, m.usb_cdc_clients);
  TEST_ASSERT_EQUAL(2, m.wifi_tcp_clients);
}

void test_uptime_is_64_bit_across_the_millis_wrap() {
  tick(0);                        // let the core see the starting time
  raw_clock->set(0xFFFFF000u);    // jump forward to just before the wrap
  tick(0);
  const uint8_t pkt[] = {0x45};
  proc->on_packet_received(-80, 10, pkt, 1);

  ClientToRadioMessage req = request(1);
  req.which_body = kOpGetDeviceInfo;
  sink->clear();
  send(req, sink);
  uint64_t before = sink->at(0).m().body.device_info.uptime_ms;
  TEST_ASSERT_EQUAL(0xFFFFF000ULL, before);

  // cross the 32-bit rollover: 0x2000 ms later the source reads 0x1000
  tick(0x2000);
  sink->clear();
  send(req, sink);
  uint64_t after = sink->at(0).m().body.device_info.uptime_ms;
  TEST_ASSERT_EQUAL(0x100001000ULL, after);  // one rollover, + 0x1000

  // the stored packet kept the full 64-bit stamp
  StoredPacket e;
  TEST_ASSERT_TRUE(store->get(1, &e));
  TEST_ASSERT_EQUAL(before, e.uptime_ms);
  TEST_ASSERT_EQUAL(0xFFFFF000ULL, e.uptime_ms);
}

// ---- main -----------------------------------------------------------------------

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cobs_roundtrip);
  RUN_TEST(test_cobs_empty);
  RUN_TEST(test_cobs_decode_respects_the_destination_cap);
  RUN_TEST(test_frame_reader_drops_a_body_longer_than_any_frame);
  RUN_TEST(test_frame_roundtrip_and_crc);
  RUN_TEST(test_frame_reader_stream);
  RUN_TEST(test_frame_reader_bad_crc_dropped);
  RUN_TEST(test_frame_reader_ignores_garbage_between_frames);
  RUN_TEST(test_store_append_and_fetch_order);
  RUN_TEST(test_store_packs_variable_lengths);
  RUN_TEST(test_store_wraps_tail_to_front);
  RUN_TEST(test_store_oversize_packet_never_fits);
  RUN_TEST(test_store_max_size_packet_roundtrip);
  RUN_TEST(test_store_append_reports_the_stored_entry);
  RUN_TEST(test_store_randomized_matches_model);
  RUN_TEST(test_store_since_cursor_is_resumable);
  RUN_TEST(test_store_get_across_wrap);
  RUN_TEST(test_store_purge_resets_history);
  RUN_TEST(test_settings_serialize_roundtrip);
  RUN_TEST(test_settings_corrupt_crc_rejected);
  RUN_TEST(test_settings_bad_size_rejected);
  RUN_TEST(test_device_settings_defaults_and_validation);
  RUN_TEST(test_ping_echoes);
  RUN_TEST(test_ping_skips_unknown_fields);
  RUN_TEST(test_unknown_operation_errors);
  RUN_TEST(test_malformed_or_unidentified_envelope_dropped);
  RUN_TEST(test_boot_uses_persisted_settings);
  RUN_TEST(test_boot_first_time_uses_safe_default);
  RUN_TEST(test_get_radio_settings);
  RUN_TEST(test_set_radio_persists_and_applies);
  RUN_TEST(test_set_radio_bad_payload);
  RUN_TEST(test_set_radio_apply_failure_is_tx_failed);
  RUN_TEST(test_rejected_retune_leaves_the_radio_usable);
  RUN_TEST(test_set_radio_without_settings_is_bad_payload);
  RUN_TEST(test_first_owner_lock_honors_only_first_set);
  RUN_TEST(test_radio_changed_broadcast_to_others);
  RUN_TEST(test_send_packet_refuses_a_store_that_cannot_take_it);
  RUN_TEST(test_send_packet_roundtrip);
  RUN_TEST(test_send_packet_tx_failure_reports_result);
  RUN_TEST(test_send_packet_busy_when_second_in_flight);
  RUN_TEST(test_send_packet_bad_length);
  RUN_TEST(test_send_packet_max_raw_size_accepted);
  RUN_TEST(test_fetch_packets_streams_entries_and_end);
  RUN_TEST(test_fetch_packets_since_cursor_skips_old);
  RUN_TEST(test_fetch_packets_is_capped_per_request);
  RUN_TEST(test_rx_packet_pushes_to_all_sinks);
  RUN_TEST(test_purge_store);
  RUN_TEST(test_sink_registry_is_bounded_and_shared);
  RUN_TEST(test_device_info);
  RUN_TEST(test_device_info_radio_failure);
  RUN_TEST(test_boot_reports_a_dead_radio_and_still_loads_settings);
  RUN_TEST(test_send_packet_survives_a_failed_apply);
  RUN_TEST(test_device_settings_defaults);
  RUN_TEST(test_set_device_name_renames_and_broadcasts);
  RUN_TEST(test_set_device_settings_wifi_applies_live);
  RUN_TEST(test_set_device_settings_is_atomic);
  RUN_TEST(test_set_device_settings_capability_gate);
  RUN_TEST(test_empty_pin_restores_the_factory_default);
  RUN_TEST(test_max_length_strings_survive_the_wire);
  RUN_TEST(test_device_settings_push_reaches_only_authorized_clients);
  RUN_TEST(test_changing_the_pin_re_gates_other_connections);
  RUN_TEST(test_auth_gate);
  RUN_TEST(test_auth_is_per_connection);
  RUN_TEST(test_auth_rate_limit);
  RUN_TEST(test_setting_a_new_pin_clears_the_lock_and_budget);
  RUN_TEST(test_factory_reset);
  RUN_TEST(test_reboot_and_bootloader);
  RUN_TEST(test_status_reads_the_hooks);
  RUN_TEST(test_status_never_inherits_the_previous_response);
  RUN_TEST(test_status_pushed_on_wifi_transition);
  RUN_TEST(test_uptime_is_64_bit_across_the_millis_wrap);
  return UNITY_END();
}
