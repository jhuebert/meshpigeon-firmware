#ifndef MESHPGEON_COMMAND_PROCESSOR_H
#define MESHPGEON_COMMAND_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#include "packet_store.h"
#include "protocol.h"
#include "settings.h"
#include "uptime_clock.h"

namespace meshpigeon {

/**
 * LoRa radio abstraction for the board ports. RadioLib sits behind this in
 * firmware; tests and the simulator use fakes. The firmware never parses
 * packets — raw bytes in, raw bytes out.
 */
class ILoRaRadio {
 public:
  virtual ~ILoRaRadio() {}
  /** Apply settings; false if the radio rejects them (out of range etc). */
  virtual bool apply(const RadioSettings& s) = 0;
  /** Start keying up with raw bytes: 0 started, <0 refused (immediate). */
  virtual int transmit(const uint8_t* raw, uint8_t len) = 0;
  /** True when the started transmission has finished. */
  virtual bool tx_done() = 0;
  /** Poll for a received packet. False if none available. */
  virtual bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi,
                       int8_t* snr) = 0;
};

/** Board-specific actions the core may request. */
class IBoardHooks {
 public:
  virtual ~IBoardHooks() {}
  /** Battery millivolts, or 0xFFFF if unknown (device status, not mesh). */
  virtual uint16_t battery_mv() { return 0xFFFF; }
  /** Reboot into bootloader/DFU for in-app flashing. */
  virtual void reboot_to_bootloader() {}
};

/** A connected client (BLE central, USB CDC, TCP socket). Sinks frame out. */
class IFrameSink {
 public:
  virtual ~IFrameSink() {}
  /** Send one decoded frame (cmd/nonce/status/payload/crc16). */
  virtual void send_frame(const uint8_t* decoded, size_t len) = 0;
};

/**
 * Board-neutral command dispatcher (docs/radio-protocol.md). Owns the
 * packet store, settings persistence and the connected-client registry.
 * Board ports feed it frames from any transport and drive it from their
 * main loop; everything here is unit-testable on the host.
 */
class CommandProcessor {
 public:
  CommandProcessor(PacketStore& store, UptimeClock& clock,
                   SettingsStore& settings_store, ILoRaRadio& radio,
                   const char* board_name, const char* fw_version);

  void set_hooks(IBoardHooks* hooks) { hooks_ = hooks; }

  void add_sink(IFrameSink* sink);
  void remove_sink(IFrameSink* sink);

  /** Boot: load persisted settings, apply to radio. Returns load result. */
  bool boot();

  /** Board loop tick: completes pending TX when the radio finishes. */
  void poll();

  /** Handle one decoded frame from `from`. Builds the response frame. */
  void on_frame(uint8_t cmd, uint8_t nonce, uint8_t status,
                const uint8_t* payload, size_t len, IFrameSink* from);

  /** Radio loop: a packet came off the air. Stores it, broadcasts it. */
  uint32_t on_packet_received(int8_t rssi, int8_t snr, const uint8_t* raw,
                              uint8_t len);

  /** Board loop: transmit finished. Broadcasts the result for a send seq. */
  void on_tx_result(uint32_t seq, bool ok);

  const RadioSettings& settings() const { return settings_; }
  uint32_t tx_in_flight() const { return tx_pending_; }
  size_t sink_count() const { return num_sinks_; }

 private:
  void emit_tx_result(uint32_t seq, bool ok);
  void respond(IFrameSink* to, uint8_t cmd, uint8_t nonce, uint8_t status,
               const uint8_t* payload, size_t len);
  void broadcast(const uint8_t* decoded, size_t len, IFrameSink* except);
  bool first_owner_lock_active() const;

  PacketStore& store_;
  UptimeClock& clock_;
  SettingsStore& settings_store_;
  ILoRaRadio& radio_;
  IBoardHooks* hooks_ = nullptr;
  char board_name_[17];
  char fw_version_[9];

  RadioSettings settings_;
  bool settings_loaded_ = false;
  uint32_t set_count_since_boot_ = 0;
  uint8_t tx_pending_ = 0;     // a SEND_PACKET is keying up
  uint32_t tx_pending_seq_ = 0;
  bool tx_start_ok_ = true;

  static const size_t kMaxSinks = 4;  // 04 §7: >=3 concurrent BLE clients
  IFrameSink* sinks_[kMaxSinks];
  size_t num_sinks_ = 0;
};

}  // namespace meshpigeon

#endif  // MESHPGEON_COMMAND_PROCESSOR_H
