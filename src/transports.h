#ifndef MESHPIGEON_TRANSPORTS_H
#define MESHPIGEON_TRANSPORTS_H

#include <Arduino.h>

#if defined(MESHPIGEON_ESP32)
#include <NimBLEDevice.h>
#elif defined(MESHPIGEON_NRF52)
#include <bluefruit.h>
#endif

#include "meshpigeon/command_processor.h"
#include "meshpigeon/frame_drain.h"
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
    Serial.write(wire(), len);
    Serial.flush();
  }

  /** Board loop: drain incoming bytes into frames, bounded per tick so one
   *  chatty client cannot own the loop (FRAME_MAX_DRAIN_BYTES_PER_PUMP). */
  void pump() { drain_frames(*proc_, reader_, frame_, this, Serial); }

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
 * A bounded byte FIFO between a transport's event context and the board
 * loop: one producer (the BLE callback), one consumer (pump).
 *
 * This exists because a NimBLE callback is NOT the board loop.
 * NimBLE-Arduino runs the BLE host in its own FreeRTOS task
 * (`esp_nimble_enable` -> `nimble_host`, priority `configMAX_PRIORITIES-4`),
 * and the GATT write handler runs there. The Arduino `loop()` task sits at
 * priority 1, so the host task *preempts* it: a callback that reaches into
 * the core races the tick that is polling the radio, and the core is
 * single-threaded by design — one response scratch and one sink registry.
 * The loser of that race is a half-built envelope encoded and broadcast to
 * every transport, or a registry walked while it is being mutated.
 *
 * So the callback only copies bytes, and pump() decodes them: the core is
 * touched from exactly one task, on every board. (The nRF52 twin needs none
 * of this — BLEUart's own FIFO is already the producer side of the same
 * shape, and its callback does nothing but set two flags.)
 *
 * `N` must be a power of two. Bytes that do not fit are dropped rather than
 * overwriting what is queued: a hole in the stream costs at most the frame
 * it lands in, and framing self-heals at the next delimiter (a CRC catches
 * anything that still decodes).
 */
template <size_t N>
class RxFifo {
 public:
  void write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (head_ - tail_ >= N) return;  // full: drop the rest of this write
      buf_[(head_++) & (N - 1)] = data[i];
    }
  }
  /** The two methods drain_frames() needs from a byte source. */
  int available() const { return (int)(head_ - tail_); }
  int read() { return head_ == tail_ ? -1 : (int)buf_[(tail_++) & (N - 1)]; }
  void clear() { tail_ = head_; }

 private:
  // The standard single-producer/single-consumer ring: `head_` is written
  // by the producer and read by the consumer, `tail_` the other way round,
  // and neither is ever written by both. Volatile is what keeps each side
  // from caching the other's index.
  uint8_t buf_[N];
  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;
};

/**
 * BLE transport (Nordic-UART-Service-compatible so generic tools work).
 * Supports multiple connected centrals (docs/radio-protocol.md §1).
 *
 * Each central is a *connection*, not just a subscriber: it owns its own
 * FrameReader, its own auth flag and its own sink slot. That matters for
 * the PIN — a shared sink would let a second central connect mid-session and
 * inherit a session the first one authenticated, so "auth is per
 * connection" would quietly be false on the one transport that accepts
 * strangers without a password (docs/radio-protocol.md §8.2). Outbound
 * frames still fan out to every central, so one push is one serialization
 * either way.
 *
 * The host task owns the callbacks; the board loop owns the core. Only
 * pump() crosses that line, in both directions: it registers and releases
 * the sinks, and it decodes the bytes the callbacks queued.
 */
class BleSink : public NimBLEServerCallbacks,
                public NimBLECharacteristicCallbacks {
 public:
  /** How many centrals this transport admits. Along with USB CDC and the
   *  four documented TCP clients it fills CommandProcessor's shared sink
   *  registry exactly, so the two limits can never disagree. */
  static const size_t kMaxCentrals = 3;
  /** Inbound bytes buffered per central between a write callback and the
   *  board loop. A BLE write is at most one MTU, and a central that writes
   *  faster than the loop decodes is dropped to the framing's own recovery,
   * which is the same trade the nRF52 port's 512-byte BLEUart makes. */
  static const size_t kRxFifoBytes = 512;

  /**
   * Bring the BLE stack up and publish the NUS service.
   *
   * The device name is NOT taken here and advertising does not start until
   * set_name() runs: the derived default name is built from the BLE address
   * (docs/radio-protocol.md §8.3), and on every board that address does not
   * exist until this function has initialized the stack. Naming first would
   * advertise MeshPigeon-0000. So the board brings BLE up, asks the core
   * for the effective name, and hands it back — and this is the only place
   * a name is ever set, for the first advertisement and for every rename
   * alike.
   */
  void begin(CommandProcessor& proc) {
    proc_ = &proc;
    NimBLEDevice::init("");  // the real name is applied by set_name()
    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(this);
    NimBLEService* svc = server_->createService(kNusServiceUUID);
    tx_char_ = svc->createCharacteristic(kNusNotifyCharUUID, NIMBLE_PROPERTY::NOTIFY);
    rx_char_ = svc->createCharacteristic(
        kNusWriteCharUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char_->setCallbacks(this);
    svc->start();
    server_->getAdvertising()->addServiceUUID(kNusServiceUUID);
  }

  // NimBLEServerCallbacks (1.4.x signatures). Both run on the NimBLE host
  // task, so neither may touch the core: they take or flag a connection and
  // return. pump() does the rest.
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    if (claim(desc->conn_handle) == NULL) {
      // No room for a connection at all: a connected central nothing will
      // ever be sent to is worse than no connection. Turn it away rather
      // than queue it. (Whether the *shared sink registry* still has a slot
      // is a board-loop question, so that half is answered in pump().)
      server->disconnect(desc->conn_handle);
    }
  }
  void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    (void)server;
    // The state goes with the connection. Each Connection carries a
    // FrameReader and a frame buffer (~1 KB) plus the auth flag, so
    // releasing the slot releases all of it — a session that authenticated
    // must not outlive the link it authenticated on. The slot stays claimed
    // until the board loop has actually released it, so a reconnect that
    // arrives first cannot be handed a slot that is still registered.
    Connection* c = find(desc->conn_handle);
    if (c != NULL) c->pending_disconnect = true;
  }

  // NimBLECharacteristicCallbacks (1.4.x signature)
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    Connection* c = desc == NULL ? NULL : find(desc->conn_handle);
    if (c == NULL) return;  // a link we have already dropped
    NimBLEAttValue v = ch->getValue();
    c->rx.write(v.data(), v.length());  // pump() decodes it
  }

  /** Apply the effective device name and start advertising. Called once
   *  after boot and again on every rename (§8.3). Live connections are
   *  unaffected; scanners see the new name at the next advertising round. */
  void set_name(const char* name) {
    if (server_ == NULL) return;
    NimBLEDevice::setDeviceName(name);
    // The name has to go into the advertising payload too, not only into
    // the GAP database, or a scanner that has never connected never sees it.
    server_->getAdvertising()->setName(name);
    server_->getAdvertising()->start();
  }

  /** Give the link a bounded chance to carry what is already queued, before
   *  the board goes down. The core answers Reboot / FactoryReset /
   *  Bootloader with `Ok` and *then* asks for the restart, so without this
   *  the client that asked for the reboot never learns it was accepted
   *  (docs/radio-protocol.md §3). NimBLE queues a notification and its own
   *  host task transmits it, so this is a bounded wait, not a flush. */
  void flush(uint32_t timeout_ms) { (void)timeout_ms; delay(timeout_ms); }

  /**
   * Board loop, and the ONLY place this transport touches the core:
   * reconcile the connections the host task reported, then decode whatever
   * bytes they queued. Order matters — a connection is registered before
   * its bytes are dispatched, so a request can never be answered by a sink
   * the registry does not know about, and a broadcast can never race the
   * registration.
   */
  void pump() {
    if (proc_ == NULL) return;
    for (size_t i = 0; i < kMaxCentrals; i++) {
      Connection& c = conns_[i];
      if (c.pending_disconnect) {
        c.pending_disconnect = false;
        if (c.registered) {
          proc_->remove_sink(&c);
          c.registered = false;
        }
        c.rx.clear();
        c.reader.reset();
        c.in_use = false;  // last: the slot is now claimable again
        if (server_ != NULL) server_->getAdvertising()->start();
        continue;
      }
      if (!c.in_use) continue;
      if (!c.registered) {
        // The sink registry is shared with every transport and is the
        // board loop's to take, so it is the thing that can still say no.
        // A central nothing will ever be sent to is turned away, as at
        // connect time.
        if (!proc_->add_sink(&c)) {
          server_->disconnect(c.handle);
          c.pending_disconnect = true;
          continue;
        }
        c.registered = true;
      }
      // Bounded per tick, like every other transport: a chatty central
      // must not be able to own the loop (FRAME_MAX_DRAIN_BYTES_PER_PUMP).
      drain_frames(*proc_, c.reader, c.frame, &c, c.rx);
    }
  }

  /** Connected centrals, for Status.ble_clients. */
  uint8_t ble_clients() {
    size_t n = 0;
    for (size_t i = 0; i < kMaxCentrals; i++) {
      if (conns_[i].in_use) n++;
    }
    return (uint8_t)n;
  }

 private:
  /** One connected central: the connection, and therefore the sink. */
  class Connection : public IFrameSink {
   public:
    // Chunk at the BLE-default MTU (23 - 3 = 20) so it works before MTU
    // exchange; NimBLE splits per connection otherwise. Every attached
    // central gets every frame.
    void send_frame(const uint8_t* decoded, size_t len) override {
      const size_t n = encode_wire(decoded, len);
      const uint8_t* bytes = wire();
      size_t off = 0;
      while (off < n) {
        size_t chunk = min(n - off, (size_t)20);
        notify_->notify(&bytes[off], chunk);
        off += chunk;
      }
    }

    NimBLECharacteristic* notify_ = NULL;  // the shared NUS TX char
    uint16_t handle = 0;
    // Who may write what, in a port with two tasks. The host task owns
    // `handle`, `notify_` and the two flags it sets (in_use,
    // pending_disconnect) plus the FIFO's head; the board loop owns
    // `registered` (the sink registry), the reader, the frame buffer and the
    // FIFO's tail. Nothing else crosses between them, and each field has
    // exactly one writer.
    volatile bool in_use = false;
    volatile bool registered = false;
    volatile bool pending_disconnect = false;
    RxFifo<kRxFifoBytes> rx;
    FrameReader reader;
    uint8_t frame[FRAME_MAX_DECODED];
  };

  Connection* find(uint16_t handle) {
    for (size_t i = 0; i < kMaxCentrals; i++) {
      if (conns_[i].in_use && conns_[i].handle == handle) return &conns_[i];
    }
    return NULL;
  }

  /** Take the first free slot for a freshly connected central. NULL when
   *  every slot is taken, which is the caller's cue to turn it away. The
   *  sink registration happens in pump(), on the board loop — so `registered`
   *  belongs to the board loop alone and is not touched here: a slot that is
   *  free has already had it cleared, before `in_use` went false. */
  Connection* claim(uint16_t handle) {
    for (size_t i = 0; i < kMaxCentrals; i++) {
      if (conns_[i].in_use) continue;
      conns_[i].handle = handle;
      conns_[i].in_use = true;
      conns_[i].pending_disconnect = false;
      conns_[i].authenticated = false;
      conns_[i].notify_ = tx_char_;
      conns_[i].rx.clear();
      conns_[i].reader.reset();
      return &conns_[i];
    }
    return NULL;
  }

  CommandProcessor* proc_ = NULL;
  NimBLEServer* server_ = NULL;
  NimBLECharacteristic* tx_char_ = NULL;
  NimBLECharacteristic* rx_char_ = NULL;
  // A fixed array, not a vector: the core's sink registry holds pointers
  // into it, and a reallocation would leave every registered connection
  // dangling. Slots are reused, so a connect/disconnect cycle costs no
  // heap and cannot leak the ~1 KB of per-connection frame state.
  Connection conns_[kMaxCentrals];
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
  /** Bring the BLE stack up and join the sink registry. The name and the
   *  advertising start are deliberately not here — see the ESP32 twin's
   *  begin() for why (the derived name needs the BLE address, which does
   *  not exist until Bluefruit.begin() has run). */
  void begin(CommandProcessor& proc) {
    proc_ = &proc;
    self_ = this;
    if (!proc_->add_sink(this)) return;  // registry full: nothing to serve
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    Bluefruit.Periph.setConnectCallback(on_connect);
    Bluefruit.Periph.setDisconnectCallback(on_disconnect);
    bleuart.begin();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(bleuart);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.setInterval(kAdvIntervalMin, kAdvIntervalMax);
    Bluefruit.Advertising.setFastTimeout(kAdvFastTimeout);
    Bluefruit.Advertising.restartOnDisconnect(true);
  }

  /** Board loop: drain BLE RX into frames, then the TX queue into
   *  notifications. */
  void pump() {
    drain_frames(*proc_, reader_, frame_, this, bleuart);
    if (drop_pending_) {
      // The link went away. Everything queued was for the central that just
      // left, and so was the session it authenticated: a session must not
      // outlive the link it was authenticated on, or whoever connects next
      // starts inside the gate. The ESP32 twin gets this for free by giving
      // every central its own sink; with one static sink it is ours to do.
      drop_pending_ = false;
      head_ = 0;
      count_ = 0;
      authenticated = false;
    }
    drain_step();
  }

  // IFrameSink: enqueue for the board loop (never writes from a callback
  // thread). Bounded wait when full — see make_room().
  void send_frame(const uint8_t* decoded, size_t len) override {
    // Nobody to notify: drop it rather than queue it. A frame left in the
    // queue with no central attached would sit there until one connects and
    // then be delivered as a stale push into that central's session.
    if (!connected()) return;
    size_t n = encode_wire(decoded, len);
    make_room();
    if (count_ == kQueueDepth) return;  // central not reading: drop this frame
    Queued& q = queue_[(head_ + count_) % kQueueDepth];
    memcpy(q.buf, wire(), n);  // queued: the board loop drains it later
    q.len = n;
    q.off = 0;
    count_++;
  }

  /** Apply the effective device name and start advertising: the initial
   *  name after boot and every rename alike (§8.3). Live connections are
   *  unaffected; scanners see the new name at the next advertising round. */
  void set_name(const char* name) {
    Bluefruit.setName(name);
    Bluefruit.Advertising.start(0);
  }

  /** Push what is queued for as long as the caller allows. The core answers
   *  Reboot / FactoryReset / Bootloader with `Ok` and *then* asks for the
   *  restart, and on this board that answer is still sitting in the TX
   *  queue when the reset arrives — without this the client that asked for
   *  the reboot never learns it was accepted (docs/radio-protocol.md §3).
   *  Bounded: a central that has stopped reading costs the wait, not a hang. */
  void flush(uint32_t timeout_ms) {
    uint32_t waited = 0;
    while (count_ > 0 && waited < timeout_ms) {
      if (drain_step()) continue;
      delay(1);
      waited++;
    }
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

BleSink* BleSink::self_ = NULL;

#endif  // MESHPIGEON_ESP32 / MESHPIGEON_NRF52

}  // namespace meshpigeon

#endif  // MESHPIGEON_TRANSPORTS_H
