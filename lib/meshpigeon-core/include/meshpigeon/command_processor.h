#ifndef MESHPIGEON_COMMAND_PROCESSOR_H
#define MESHPIGEON_COMMAND_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>

#include "packet_store.h"
#include "proto_alias.h"
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

/** Board capability bits (DeviceInfo.capabilities), assembled from board
 *  defines — compile-time facts, never probed at runtime (docs/radio-protocol.md §5). */
enum Capability {
  kCapWifiSta = 0,
  kCapBattery = 1,
  kCapBle = 2,
  kCapUsbCdc = 3,
};

/** Board-specific actions the core may request. */
class IBoardHooks {
 public:
  virtual ~IBoardHooks() {}
  /** Battery millivolts, or 0xFFFF if unknown (device status, not mesh). */
  virtual uint16_t battery_mv() { return 0xFFFF; }
  /** Reboot into bootloader/DFU for in-app flashing. */
  virtual void reboot_to_bootloader() {}
  /** Clean restart into the persisted configuration (REBOOT). */
  virtual void reboot() {}
  /** Wipe persisted device settings (not the radio tuning or boot count),
   *  then reboot — FACTORY_RESET (docs/radio-protocol.md §10). */
  virtual void factory_reset() {}
  /** The two low MAC bytes every derived identifier shares ("A3F2"). */
  virtual void mac_suffix(char out[5]) { out[0] = 0; }
  /** BLE advertising name change: rename and restart advertising (§9). */
  virtual void set_device_name(const char* name) { (void)name; }
  /** Connected BLE centrals, for Status.ble_clients. */
  virtual uint8_t ble_clients() { return 0; }
  /** Fill a Status from the board's live link state (Wi-Fi etc.). */
  virtual void fill_status(StatusMessage* status) { (void)status; }
  /** Device settings changed: the link reacts (connect/disconnect/rebind).
   *  No-op on boards without the Wi-Fi capability. */
  virtual void apply_wifi(const DeviceSettings& settings) {
    (void)settings;
  }
  /** False on boards without station capability: Wi-Fi setting writes are
   *  then rejected atomically with NOT_SUPPORTED (docs/radio-protocol.md §8.1). */
  virtual bool wifi_supported() const { return false; }
  /** Append this board's capability values; returns the new count. */
  virtual pb_size_t fill_capabilities(meshpigeon_Capability* out, pb_size_t max) {
    (void)out;
    (void)max;
    return 0;
  }
};

/**
 * A connected client (BLE central, USB CDC, TCP socket). Sinks frames out.
 * The auth state is per-connection: authenticating on BLE says nothing about
 * the TCP socket someone else opened.
 */
class IFrameSink {
 public:
  virtual ~IFrameSink() {}
  /** Send one serialized RadioToClient envelope (unchecked). */
  virtual void send_frame(const uint8_t* decoded, size_t len) = 0;
  bool authenticated = false;
};

/**
 * Board-neutral command dispatcher (docs/radio-protocol.md v2). Owns the
 * packet store, settings persistence and the connected-client registry.
 * Board ports feed it serialized envelopes from any transport and drive it
 * from their main loop; everything here is unit-testable on the host.
 */
class CommandProcessor {
 public:
  CommandProcessor(PacketStore& store, UptimeClock& clock,
                   SettingsStore& settings_store, ILoRaRadio& radio,
                   const char* board_name, const char* fw_version);

  void set_hooks(IBoardHooks* hooks) { hooks_ = hooks; }

  void add_sink(IFrameSink* sink);
  void remove_sink(IFrameSink* sink);

  /** Boot: load persisted settings, apply to radio. Returns radio result. */
  bool boot();

  /** Board loop tick: completes pending TX when the radio finishes. */
  void poll();

  /**
   * Handle one serialized ClientToRadio envelope from `from`. Builds the
   * response envelope. `from == NULL` is allowed for tests that only want
   * the broadcasts.
   */
  void on_envelope(const uint8_t* data, size_t len, IFrameSink* from);

  /** Radio loop: a packet came off the air. Stores it, broadcasts it. */
  uint32_t on_packet_received(int8_t rssi, int8_t snr, const uint8_t* raw,
                              uint8_t len);

  /** Board loop: transmit finished. Broadcasts the result for a send seq. */
  void on_tx_result(uint32_t seq, bool ok);

  /** The board's link changed the Wi-Fi state: broadcast Status (§11). */
  void on_wifi_state_changed();

  const RadioSettings& settings() const { return settings_; }
  const DeviceSettings& device_settings() const { return device_; }
  uint32_t tx_in_flight() const { return tx_pending_; }
  size_t sink_count() const { return num_sinks_; }

  /** The effective name: stored, else "MeshPigeon-XXXX" from the MAC (§9). */
  void effective_name(char out[MESHPIGEON_NAME_MAX + 1]) const;

 private:
  void handle_request(const ClientToRadioMessage& req, IFrameSink* from);
  bool require_auth(const ClientToRadioMessage& req, IFrameSink* from);
  bool pin_matches(const char* pin, size_t len) const;
  bool auth_backoff_active() const;
  void note_auth_failure();

  // ---- response building: fill response_, then deliver/broadcast it ----
  void build_radio_settings();
  void build_device_settings();
  void build_status();
  void deliver(IFrameSink* to);
  void broadcast_response(IFrameSink* except);

  void send_error(uint32_t id, ErrorCode code, IFrameSink* to);
  void send_ok(uint32_t id, IFrameSink* to);
  void send_pong(uint32_t id, const pb_byte_t* payload, pb_size_t size,
                 IFrameSink* to);
  void send_device_info(uint32_t id, IFrameSink* to);

  void emit_tx_result(uint32_t seq, bool ok);
  void notify_radio_changed(IFrameSink* except);
  void notify_device_settings_changed(IFrameSink* except);

  void build_name(const DeviceSettings& s,
                  char out[MESHPIGEON_NAME_MAX + 1]) const;
  bool first_owner_lock_active() const;

  PacketStore& store_;
  UptimeClock& clock_;
  SettingsStore& settings_store_;
  ILoRaRadio& radio_;
  IBoardHooks* hooks_ = NULL;
  char board_name_[17];
  char fw_version_[16];

  RadioSettings settings_;
  bool settings_loaded_ = false;
  uint32_t set_count_since_boot_ = 0;
  DeviceSettings device_;
  bool radio_ok_ = true;

  // AUTH rate limit: 3 failures are free, then a 1 s penalty window per
  // further attempt, during which attempts are rejected unevaluated (§8).
  uint8_t auth_fails_ = 0;
  uint64_t auth_backoff_until_ = 0;

  uint8_t tx_pending_ = 0;  // a SEND_PACKET is keying up
  uint32_t tx_pending_seq_ = 0;
  bool tx_start_ok_ = true;

  // Scratch for building responses; a member so the ~520-byte encode
  // struct exists once, not per handler.
  RadioToClientMessage response_;

  static const size_t kMaxSinks = 4;  // docs/radio-protocol.md §3: >=3 concurrent clients
  IFrameSink* sinks_[kMaxSinks];
  size_t num_sinks_ = 0;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_COMMAND_PROCESSOR_H
