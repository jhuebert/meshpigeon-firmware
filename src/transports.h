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
    proc_->add_sink(this);
  }

  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_wire(wire, decoded, len);
    Serial.write(wire, n);
    Serial.flush();
  }

  /** Board loop: drain incoming bytes into frames. */
  void pump() {
    while (Serial.available() > 0) {
      size_t res = reader_.feed((uint8_t)Serial.read(), frame_);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_frame(frame_[0], frame_[1], frame_[2], frame_ + 3, res - 5,
                        this);
      }
    }
  }

 private:
  CommandProcessor* proc_ = nullptr;
  FrameReader reader_;
  uint8_t frame_[FRAME_MAX_DECODED];
};

#if defined(MESHPIGEON_ESP32)

static const BLEUUID kNusServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static const BLEUUID kNusWriteCharUUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
static const BLEUUID kNusNotifyCharUUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

/**
 * BLE transport (Nordic-UART-Service-compatible so generic tools work).
 * Supports multiple connected centrals (04 §2); each connection gets its
 * own FrameReader and sink (notifications go only to subscribers).
 */
class BleSink : public IFrameSink, public NimBLEServerCallbacks,
                public NimBLECharacteristicCallbacks {
 public:
  void begin(CommandProcessor& proc, const char* base_name) {
    proc_ = &proc;
    NimBLEDevice::init(base_name);
    // Distinguish multiple pigeons in scan lists: append the two low bytes of
    // the BLE address ("MeshPigeon-A3F2"); nRF52 boards use the same scheme.
    const NimBLEAddress addr = NimBLEDevice::getAddress();
    const uint8_t* a = addr.getNative();  // little-endian: a[0] is the LSB
    char device_name[24];
    snprintf(device_name, sizeof(device_name), "%s-%02X%02X", base_name, a[1],
             a[0]);
    NimBLEDevice::setDeviceName(device_name);
    NimBLEDevice::setMTU(247);
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
    proc_->add_sink(this);
  }

  // NimBLEServerCallbacks (1.4.x signatures)
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    (void)server;
    conns_.push_back(desc->conn_handle);
  }
  void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    for (size_t i = 0; i < conns_.size(); i++) {
      if (conns_[i] == desc->conn_handle) {
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
        proc_->on_frame(c.frame[0], c.frame[1], c.frame[2], c.frame + 3,
                        res - 5, this);
      }
    }
  }

  // IFrameSink: notify every connected central. Chunk at the BLE-default
  // MTU (23 - 3 = 20) so it works before MTU exchange; NimBLE splits per
  // connection otherwise.
  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_wire(wire, decoded, len);
    size_t off = 0;
    while (off < n) {
      size_t chunk = min(n - off, (size_t)20);
      tx_char_->notify(&wire[off], chunk);
      off += chunk;
    }
  }

  /** Board loop: RX is callback-driven and TX immediate — nothing to do. */
  void pump() {}

 private:
  struct PerConn {
    uint16_t handle = 0;
    FrameReader reader;
    uint8_t frame[FRAME_MAX_DECODED];
  };

  PerConn& reader_for(uint16_t handle) {
    for (PerConn& c : conns_state_) {
      if (c.handle == handle) return c;
    }
    PerConn c;
    c.handle = handle;
    conns_state_.push_back(c);
    return conns_state_.back();
  }

  CommandProcessor* proc_ = nullptr;
  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* tx_char_ = nullptr;
  NimBLECharacteristic* rx_char_ = nullptr;
  std::vector<uint16_t> conns_;
  std::vector<PerConn> conns_state_;
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
 * the SoftDevice buffer is full. send_frame blocks (draining) only when the
 * queue is full, which cannot deadlock: the SoftDevice drains in its own
 * context, independent of this loop.
 */
class BleSink : public IFrameSink {
 public:
  void begin(CommandProcessor& proc, const char* base_name) {
    proc_ = &proc;
    self_ = this;
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    // Distinguish multiple pigeons in scan lists: append the two low bytes of
    // the BLE address ("MeshPigeon-A3F2"); matches the ESP32 sink naming.
    char device_name[24];
    ble_gap_addr_t addr;
    if (sd_ble_gap_addr_get(&addr) == NRF_SUCCESS) {
      snprintf(device_name, sizeof(device_name), "%s-%02X%02X", base_name,
               addr.addr[1], addr.addr[0]);
    } else {
      snprintf(device_name, sizeof(device_name), "%s", base_name);
    }
    Bluefruit.setName(device_name);
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
    proc_->add_sink(this);
  }

  /** Board loop: drain BLE RX into frames, then TX queue into notifications. */
  void pump() {
    while (bleuart.available() > 0) {
      size_t res = reader_.feed((uint8_t)bleuart.read(), frame_);
      if (res != 0 && res != (size_t)-1) {
        proc_->on_frame(frame_[0], frame_[1], frame_[2], frame_ + 3, res - 5,
                        this);
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
  // thread; blocks only on a full queue, see class comment).
  void send_frame(const uint8_t* decoded, size_t len) override {
    uint8_t wire[FRAME_MAX_WIRE];
    size_t n = frame_encode_wire(wire, decoded, len);
    while (count_ == kQueueDepth) drain_step_blocking();
    Queued& q = queue_[(head_ + count_) % kQueueDepth];
    memcpy(q.buf, wire, n);
    q.len = n;
    q.off = 0;
    count_++;
  }

 private:
  struct Queued {
    size_t len;
    size_t off;
    uint8_t buf[FRAME_MAX_WIRE];
  };

  static const size_t kQueueDepth = 16;
  static const size_t kChunk = 20;  // BLE default MTU (23) minus 3 overhead

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

  void drain_step_blocking() {
    while (count_ > 0 && !drain_step()) {
      if (!connected()) {
        // Frames for a lost client are useless — drop and stop waiting.
        head_ = 0;
        count_ = 0;
        return;
      }
      delay(1);
    }
  }

  static BleSink* self_;

  CommandProcessor* proc_ = nullptr;
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
