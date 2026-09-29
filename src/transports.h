#ifndef MESHPIGEON_TRANSPORTS_H
#define MESHPIGEON_TRANSPORTS_H

#include <Arduino.h>

#if defined(MESHPIGEON_ESP32)
#include <NimBLEDevice.h>
#elif defined(MESHPIGEON_NRF52)
#include <bluefruit.h>
#endif

#include "meshpigeon/command_processor.h"
#include "meshpigeon/framing.h"

namespace meshpigeon {

/**
 * USB CDC transport (host-facing serial console). One client. FrameReader
 * reassembles COBS frames from whatever byte chunks the UART delivers.
 */
class UsbCdcSink : public IFrameSink {
 public:
  void begin(CommandProcessor& proc) {
    proc_ = &proc;
    if (!proc_->add_sink(this)) return;  // registry full: nothing to serve
  }

  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_envelope(wire, decoded, len);
    Serial.write(wire, n);
    Serial.flush();
  }

  /** Board loop: drain incoming bytes into frames. */
  void pump() {
    while (Serial.available() > 0) {
      size_t res = reader_.feed((uint8_t)Serial.read(), frame_);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_envelope(frame_, res, this);
      }
    }
  }

 private:
  CommandProcessor* proc_ = NULL;
  FrameReader reader_;
  uint8_t frame_[FRAME_MAX_DECODED];
};

#if defined(MESHPIGEON_ESP32)

static const BLEUUID kNusServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static const BLEUUID kNusWriteCharUUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static const BLEUUID kNusNotifyCharUUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

/**
 * BLE transport (Nordic-UART-Service-compatible so generic tools work).
 * Supports multiple connected centrals (docs/radio-protocol.md §1); each connection gets its
 * own FrameReader, but they share one sink — notifications go to every
 * subscriber, and auth state is per-*transport* (one BLE link, one serial
 * console), not per-central.
 */
class BleSink : public IFrameSink, public NimBLEServerCallbacks,
                public NimBLECharacteristicCallbacks {
 public:
  /** `name` is the effective device name: the stored name, else the derived
   *  "MeshPigeon-XXXX" (docs/radio-protocol.md §8.3). The core owns that
   *  derivation — the transport must not compute the suffix a second time. */
  void begin(CommandProcessor& proc, const char* name) {
    proc_ = &proc;
    if (!proc_->add_sink(this)) return;  // registry full: nothing to serve
    NimBLEDevice::init(name);
    NimBLEDevice::setDeviceName(name);
    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(this);
    NimBLEService* svc = server_->createService(kNusServiceUUID);
    tx_char_ = svc->createCharacteristic(kNusNotifyCharUUID, NIMBLE_PROPERTY::NOTIFY);
    rx_char_ = svc->createCharacteristic(
        kNusWriteCharUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char_->setCallbacks(this);
    svc->start();
    server_->getAdvertising()->addServiceUUID(kNusServiceUUID);
    server_->getAdvertising()->start();
  }

  // NimBLEServerCallbacks (1.4.x signatures)
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    (void)server;
    PerConn c;
    c.handle = desc->conn_handle;
    conns_.push_back(c);
  }
  void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    // The state goes with the connection. Each PerConn carries a FrameReader
    // and a frame buffer (~1 KB), so leaving them behind would grow the heap
    // by that much per connect/disconnect cycle for the life of the boot.
    for (size_t i = 0; i < conns_.size(); i++) {
      if (conns_[i].handle == desc->conn_handle) {
        conns_.erase(conns_.begin() + i);
        break;
      }
    }
    server->getAdvertising()->start();
  }

  // NimBLECharacteristicCallbacks (1.4.x signature)
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    (void)desc;
    NimBLEAttValue v = ch->getValue();
    PerConn& c = reader_for(desc ? desc->conn_handle : 0);
    for (size_t i = 0; i < v.length(); i++) {
      size_t res = c.reader.feed(v.data()[i], c.frame);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_envelope(c.frame, res, this);
      }
    }
  }

  // IFrameSink: notify every connected central. Chunk at the BLE-default
  // MTU (23 - 3 = 20) so it works before MTU exchange; NimBLE splits per
  // connection otherwise.
  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_envelope(wire, decoded, len);
    size_t off = 0;
    while (off < n) {
      size_t chunk = min(n - off, (size_t)20);
      tx_char_->notify(&wire[off], chunk);
      off += chunk;
    }
  }

  /** Board loop: RX is callback-driven and TX immediate — nothing to do. */
  void pump() {}

  /** Apply a device-settings name: rename and re-advertise (docs/radio-protocol.md §8.3).
   *  Live connections are unaffected; scanners see the new name. */
  void set_name(const char* name) {
    if (server_ == NULL) return;
    NimBLEDevice::setDeviceName(name);
    server_->getAdvertising()->start();
  }

  /** Connected centrals, for Status.ble_clients. */
  uint8_t ble_clients() { return (uint8_t)conns_.size(); }

 private:
  struct PerConn {
    uint16_t handle = 0;
    FrameReader reader;
    uint8_t frame[FRAME_MAX_DECODED];
  };

  PerConn& reader_for(uint16_t handle) {
    for (PerConn& c : conns_) {
      if (c.handle == handle) return c;
    }
    PerConn c;
    c.handle = handle;
    conns_.push_back(c);
    return conns_.back();
  }

  CommandProcessor* proc_ = NULL;
  NimBLEServer* server_ = NULL;
  NimBLECharacteristic* tx_char_ = nullptr;
  NimBLECharacteristic* rx_char_ = nullptr;
  // One entry per connected central: the handle plus the frame state that
  // belongs to it. A single vector, so the two cannot fall out of step.
  std::vector<PerConn> conns_;
};

#elif defined(MESHPIGEON_NRF52)

static const uint8_t kAdvIntervalMin = 32;   // units: 0.625 ms (20 ms)
static const uint8_t kAdvIntervalMax = 244;  // 152.5 ms (MeshCore-tested)
static const uint16_t kAdvFastTimeout = 30;  // seconds

/**
 * BLE transport (Adafruit Bluefruit, Nordic-UART-Service-compatible so
 * generic tools and the MeshPigeon app work unchanged). Single central (v1);
 * no pairing — same security posture as the ESP32 sink.
 *
 * RX: bleuart's FIFO is filled from the BLE event context, drained here in
 * the board loop. TX: notify's SoftDevice buffer is only a few packets deep,
 * so frames queue (the app fetches history in 32-packet bursts) and drain as
 * 20-byte chunks — one hvx packet per chunk, retrying in the board loop when
 * the SoftDevice buffer is full. send_frame drains the queue itself when it
 * is full, but only for a bounded time: a central that has stopped reading
 * must cost us dropped frames, never a wedged board loop.
 */
class BleSink : public IFrameSink {
 public:
  /** `name` is the effective device name: the stored name, else the derived
   *  "MeshPigeon-XXXX" (docs/radio-protocol.md §8.3). The core owns that
   *  derivation — the transport must not compute the suffix a second time. */
  void begin(CommandProcessor& proc, const char* name) {
    proc_ = &proc;
    self_ = this;
    if (!proc_->add_sink(this)) return;  // registry full: nothing to serve
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    Bluefruit.setName(name);
    Bluefruit.Periph.setConnectCallback(on_connect);
    Bluefruit.Periph.setDisconnectCallback(on_disconnect);
    bleuart.begin();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(bleuart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.setInterval(kAdvIntervalMin, kAdvIntervalMax);
    Bluefruit.Advertising.setFastTimeout(kAdvFastTimeout);
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.start(0);
  }

  /** Board loop: drain BLE RX into frames, then TX queue into notifications. */
  void pump() {
    while (bleuart.available() > 0) {
      size_t res = reader_.feed((uint8_t)bleuart.read(), frame_);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_envelope(frame_, res, this);
      }
    }
    if (drop_pending_) {
      drop_pending_ = false;
      head_ = 0;
      count_ = 0;
    }
    drain_step();
  }

  // IFrameSink: enqueue for the board loop (never writes from a callback
  // thread). Bounded wait when full — see make_room().
  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_envelope(wire, decoded, len);
    make_room();
    if (count_ == kQueueDepth) return;  // central not reading: drop this frame
    Queued& q = queue_[(head_ + count_) % kQueueDepth];
    memcpy(q.buf, wire, n);
    q.len = n;
    q.off = 0;
    count_++;
  }

  /** Apply a device-settings name: rename and re-advertise (docs/radio-protocol.md §8.3).
   *  Live connections are unaffected; scanners see the new name. */
  void set_name(const char* name) {
    Bluefruit.setName(name);
    Bluefruit.Advertising.start(0);
  }

  /** Connected centrals (0 or 1), for Status.ble_clients. */
  uint8_t ble_clients() { return connected() ? 1 : 0; }

 private:
  struct Queued {
    size_t len;
    size_t off;
    uint8_t buf[FRAME_MAX_WIRE];
  };

  static const size_t kQueueDepth = 16;
  static const size_t kChunk = 20;  // BLE default MTU (23) minus 3 overhead
  // How long send_frame waits for room before dropping the frame it was
  // handed. Long enough for the SoftDevice to absorb a burst, short enough
  // that a central which has stopped reading cannot stall the board loop.
  static const uint32_t kDrainWaitMs = 50;

  static void on_connect(uint16_t) { if (self_) self_->connected_ = true; }
  static void on_disconnect(uint16_t, uint8_t) {
    if (!self_) return;
    self_->connected_ = false;
    // Drop TX frames for the lost client from the board loop, not here.
    self_->drop_pending_ = true;
  }

  bool connected() const { return connected_ && Bluefruit.connected() > 0; }

  /** Write what the SoftDevice accepts now; true if anything drained. */
  bool drain_step() {
    if (!connected() || count_ == 0) return false;
    Queued& q = queue_[head_];
    size_t chunk = min(q.len - q.off, kChunk);
    size_t written = bleuart.write(q.buf + q.off, chunk);
    if (written == 0) return false;
    q.off += written;
    if (q.off == q.len) {
      head_ = (head_ + 1) % kQueueDepth;
      count_--;
    }
    return true;
  }

  /** Make room for one more frame: drain for at most kDrainWaitMs, then give
   *  up and let the caller drop the frame it was handed. The wait is bounded
   *  because `connected` staying true says nothing about the central still
   *  consuming notifications. */
  void make_room() {
    uint32_t waited = 0;
    while (count_ > 0 && !drain_step() && waited < kDrainWaitMs) {
      if (!connected()) {
        // Frames for a lost client are useless — drop and stop waiting.
        head_ = 0;
        count_ = 0;
        return;
      }
      delay(1);
      waited++;
    }
    if (count_ == kQueueDepth) count_ = 0;  // still stuck: drop the backlog
  }

  static BleSink* self_;

  CommandProcessor* proc_ = NULL;
  // 512 B: two max wire frames — the app streams frames as 20-byte chunks
  // faster than the 1 ms board loop drains, and BLEUart's default FIFO (256 B)
  // would silently overflow mid-frame.
  BLEUart bleuart{512};
  FrameReader reader_;
  uint8_t frame_[FRAME_MAX_DECODED];
  Queued queue_[kQueueDepth];
  size_t head_ = 0;
  size_t count_ = 0;
  volatile bool connected_ = false;
  volatile bool drop_pending_ = false;
};

BleSink* BleSink::self_ = nullptr;

#endif  // MESHPIGEON_ESP32 / MESHPIGEON_NRF52

}  // namespace meshpigeon

#endif  // MESHPIGEON_TRANSPORTS_H
