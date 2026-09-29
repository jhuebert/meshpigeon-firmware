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
  /** Bring the radio up (SPI, TCXO, RF switch). False if the silicon does
   *  not answer; the device then reports radio_ok = false and answers
   *  ERROR_CODE_NO_RADIO. Nothing else needs to know: CommandProcessor::boot
   *  calls this and owns the result. */
  virtual bool begin() { return true; }
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

/** How many Capability values DeviceInfo can carry — the generated
 *  `capabilities` array size (device.options caps it at the same number). */
static const pb_size_t kMaxCapabilities = 16;

/** Board capability values (DeviceInfo.capabilities), assembled from board
 *  defines — compile-time facts, never probed at runtime. */
enum Capability {
  kCapWifiSta = meshpigeon_Capability_CAPABILITY_WIFI_STA,
  kCapBattery = meshpigeon_Capability_CAPABILITY_BATTERY,
  kCapBle = meshpigeon_Capability_CAPABILITY_BLE,
  kCapUsbCdc = meshpigeon_Capability_CAPABILITY_USB_CDC,
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
  /** BLE advertising name change: rename and restart advertising (§8.3). */
  virtual void set_device_name(const char* name) { (void)name; }
  /** Connected BLE centrals, for Status.ble_clients. */
  virtual uint8_t ble_clients() { return 0; }
  /** USB CDC hosts with the port open (0 or 1), for Status.usb_cdc_clients. */
  virtual uint8_t usb_cdc_clients() { return 0; }
  /** TCP sockets on the Wi-Fi server, for Status.wifi_tcp_clients. */
  virtual uint8_t wifi_tcp_clients() { return 0; }
  /** Fill a Status from the board's live link state (Wi-Fi etc). Must not
   *  touch the three *_clients counts: those are the core's, it owns them. */
  virtual void fill_status(StatusMessage* status) { (void)status; }
  /** Device settings changed: the link reacts (connect/disconnect/rebind).
   *  No-op on boards without the Wi-Fi capability. */
  virtual void apply_wifi(const DeviceSettings& settings) {
    (void)settings;
  }
  /** False on boards without station capability: Wi-Fi setting writes are
   *  then rejected atomically with NOT_SUPPORTED (docs/radio-protocol.md §8.1). */
  virtual bool wifi_supported() const { return false; }
  /** Append this board's capability values; returns the new count. Values
   *  past `max` are not written and must not be counted. */
  virtual pb_size_t fill_capabilities(meshpigeon_Capability* out,
                                      pb_size_t max) {
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

  /** Register/unregister a connected client. add_sink returns false when
   *  the registry is full (kMaxSinks is shared by every transport), so a
   *  transport can turn the extra client away instead of silently
   *  broadcasting to nobody. */
  bool add_sink(IFrameSink* sink);
  void remove_sink(IFrameSink* sink);

  /** Boot: bring the radio up, load the persisted settings and apply them.
   *  Runs unconditionally — a dead radio still leaves a device that answers,
   *  with radio_ok = false. Returns the radio result. */
  bool boot();

  /** Board loop tick: completes pending TX when the radio finishes. */
  void poll();

  /**
   * Handle one serialized ClientToRadio envelope from `from`. Builds the
   * response envelope. `from` is the connection the request arrived on: it
   * carries the per-connection auth state, so it is never NULL.
   */
  void on_envelope(const uint8_t* data, size_t len, IFrameSink* from);

  /** Radio loop: a packet came off the air. Stores it, broadcasts it. */
  uint32_t on_packet_received(int8_t rssi, int8_t snr, const uint8_t* raw,
                              uint8_t len);

  /** Board loop: transmit finished. Broadcasts the result for a send seq. */
  void on_tx_result(uint32_t seq, bool ok);

  /** The board's link changed the Wi-Fi state: broadcast Status (§9). */
  void on_wifi_state_changed();

  const RadioSettings& settings() const { return settings_; }
  const DeviceSettings& device_settings() const { return device_; }
  /** Last estimated noise floor in dBm; 0 when nothing has been received. */
  int32_t noise_floor_dbm() const { return noise_floor_dbm_; }
  /** False once the radio has failed to come up or to accept a tuning. */
  bool radio_ok() const { return radio_ok_; }
  bool tx_in_flight() const { return tx_pending_; }
  size_t sink_count() const { return num_sinks_; }
  size_t sink_free() const { return kMaxSinks - num_sinks_; }

  /** The effective name: stored, else "MeshPigeon-XXXX" from the MAC (§8.3). */
  void effective_name(char out[MESHPIGEON_NAME_MAX + 1]) const;

 private:
  void handle_request(const ClientToRadioMessage& req, IFrameSink* from);

  // ---- response building: fill response_, then deliver/broadcast it ----
  /** Start a fresh response carrying `id` (0 for an async push). */
  void begin_response(uint32_t id);
  /** Serialize response_ into `buf` (MESHPIGEON_MAX_FRAME_PAYLOAD); 0 if
   *  it does not fit, which is the only way an answer can go missing. */
  size_t encode_response(uint8_t* buf);
  void build_radio_settings();
  void build_device_settings();
  void build_status();
  void build_packet_entry(uint32_t id, const StoredPacket& e);
  void deliver(IFrameSink* to);
  /** Push response_ to every sink but `except`. `authorized_only` restricts
   *  it to connections that could call the gated operations — required for
   *  the DeviceSettings push, which carries the Wi-Fi passphrase. */
  void broadcast_response(IFrameSink* except, bool authorized_only = false);

  void send_error(uint32_t id, ErrorCode code, IFrameSink* to);
  void send_ok(uint32_t id, IFrameSink* to);
  void send_pong(uint32_t id, const pb_byte_t* payload, pb_size_t size,
                 IFrameSink* to);
  void send_device_info(uint32_t id, IFrameSink* from);
  /** true if the client may proceed; otherwise answers AUTH_REQUIRED. */
  bool require_auth(uint32_t id, IFrameSink* from);
  /** May this connection see gated material? True when it authenticated, or
   *  while the device still holds the public default PIN. */
  bool is_authorized(const IFrameSink* from) const;
  bool pin_matches(const char* pin, size_t len) const;
  bool auth_backoff_active() const;
  void note_auth_failure();

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
  uint32_t set_count_since_boot_ = 0;
  DeviceSettings device_;
  bool radio_ok_ = false;
  // RSSI - SNR of the last received packet: the receiver's noise floor as
  // far as the radio can tell us without a dedicated register read. 0 =
  // no measurement yet (DeviceInfo.noise_floor_dbm).
  int32_t noise_floor_dbm_ = 0;

  // AUTH rate limit: 3 failures are free, then a 1 s penalty window per
  // further attempt, during which attempts are rejected unevaluated (§8.2).
  uint8_t auth_fails_ = 0;
  uint64_t auth_backoff_until_ = 0;

  bool tx_pending_ = false;  // a SEND_PACKET is keying up
  uint32_t tx_pending_seq_ = 0;

  // Scratch for building responses; a member so the ~520-byte encode
  // struct exists once, not per handler.
  RadioToClientMessage response_;

  // One sink per connection, shared by every transport (docs/radio-protocol.md
  // §7). Sized so an ESP32 board can hold USB + BLE + the documented four
  // TCP clients at once; the nRF52 boards only ever use two of them.
  static const size_t kMaxSinks = 6;
  IFrameSink* sinks_[kMaxSinks];
  size_t num_sinks_ = 0;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_COMMAND_PROCESSOR_H
