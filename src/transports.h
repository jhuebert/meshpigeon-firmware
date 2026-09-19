#ifndef MESHHOP_TRANSPORTS_H
#define MESHHOP_TRANSPORTS_H

#include <Arduino.h>

#if defined(MESHHOP_ESP32)
#include <NimBLEDevice.h>
#endif

#include "meshhop/command_processor.h"
#include "meshhop/framing.h"

namespace meshhop {

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

#if defined(MESHHOP_ESP32)

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
  void begin(CommandProcessor& proc, const char* device_name) {
    proc_ = &proc;
    NimBLEDevice::init(device_name);
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

#endif  // MESHHOP_ESP32

}  // namespace meshhop

#endif  // MESHHOP_TRANSPORTS_H
