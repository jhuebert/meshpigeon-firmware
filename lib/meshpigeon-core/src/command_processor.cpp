#include "meshpigeon/command_processor.h"

#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

namespace meshpigeon {

static const uint32_t kFirstOwnerGraceMs = 5 * 60 * 1000;  // 05 §3
static const uint8_t kFlagsSent = 0x01;
static const uint8_t kFlagsReceived = 0x02;
static const uint8_t kAuthFailsBeforeDelay = 3;
static const uint32_t kAuthFailDelayMs = 1000;
static const char kDefaultNamePrefix[] = "MeshPigeon-";

CommandProcessor::CommandProcessor(PacketStore& store, UptimeClock& clock,
                                   SettingsStore& settings_store,
                                   ILoRaRadio& radio, const char* board_name,
                                   const char* fw_version)
    : store_(store),
      clock_(clock),
      settings_store_(settings_store),
      radio_(radio) {
  strncpy(board_name_, board_name, sizeof(board_name_) - 1);
  board_name_[sizeof(board_name_) - 1] = 0;
  strncpy(fw_version_, fw_version, sizeof(fw_version_) - 1);
  fw_version_[sizeof(fw_version_) - 1] = 0;  memset(sinks_, 0, sizeof(sinks_));
}

void CommandProcessor::add_sink(IFrameSink* sink) {
  if (num_sinks_ < kMaxSinks) sinks_[num_sinks_++] = sink;
}

void CommandProcessor::remove_sink(IFrameSink* sink) {
  for (size_t i = 0; i < num_sinks_; i++) {
    if (sinks_[i] == sink) {
      memmove(&sinks_[i], &sinks_[i + 1], (num_sinks_ - i - 1) * sizeof(sink));
      sinks_[--num_sinks_] = NULL;
      return;
    }
  }
}

void CommandProcessor::build_name(
    const DeviceSettings& s, char out[MESHPIGEON_NAME_MAX + 1]) const {
  if (s.name[0] != 0) {
    memcpy(out, s.name, MESHPIGEON_NAME_MAX);
    out[MESHPIGEON_NAME_MAX] = 0;
    return;
  }
  char suffix[5] = "0000";
  if (hooks_) hooks_->mac_suffix(suffix);
  suffix[4] = 0;
  snprintf(out, MESHPIGEON_NAME_MAX + 1, "%s%s", kDefaultNamePrefix, suffix);
}

void CommandProcessor::effective_name(
    char out[MESHPIGEON_NAME_MAX + 1]) const {
  build_name(device_, out);
}

bool CommandProcessor::boot() {
  settings_ = RadioSettings::unset();
  settings_loaded_ = settings_store_.load(&settings_);
  if (!settings_loaded_) {
    // First boot: keep safe default, don't persist yet (avoid wear until
    // an app tunes us).
    settings_ = RadioSettings::unset();
  }
  settings_store_.load_device(&device_);  // false => defaults, which it wrote
  if (device_.wifi_port == 0) device_.wifi_port = MESHPIGEON_WIFI_PORT_DEFAULT;
  radio_ok_ = radio_.apply(settings_);
  return radio_ok_;
}

bool CommandProcessor::first_owner_lock_active() const {
  // 05 §3: during the grace window after boot, only the first SET_RADIO is
  // honored — the first-connected client owns the tuning decision.
  return set_count_since_boot_ >= 1 && clock_.uptime_ms64() < kFirstOwnerGraceMs;
}

// ---- envelope encode/decode -------------------------------------------------

static bool decode_envelope(const uint8_t* data, size_t len,
                            ClientToRadioMessage* out) {
  pb_istream_t stream = pb_istream_from_buffer(data, len);
  *out = ClientToRadioMessage_init_zero;
  if (!pb_decode(&stream, ClientToRadioMessage_fields, out)) return false;
  return out->id != 0;  // requests carry a client-chosen correlation id
}

static size_t encode_envelope(uint8_t* out, size_t cap,
                              const RadioToClientMessage& msg) {
  pb_ostream_t stream = pb_ostream_from_buffer(out, cap);
  if (!pb_encode(&stream, RadioToClientMessage_fields, &msg)) return 0;
  return stream.bytes_written;
}

// ---- response delivery ------------------------------------------------------

void CommandProcessor::deliver(IFrameSink* to) {
  if (to == NULL) return;
  uint8_t buf[MESHPIGEON_MAX_FRAME_PAYLOAD];
  size_t len = encode_envelope(buf, sizeof(buf), response_);
  if (len != 0) to->send_frame(buf, len);
}

void CommandProcessor::broadcast_response(IFrameSink* except) {
  uint8_t buf[MESHPIGEON_MAX_FRAME_PAYLOAD];
  size_t len = encode_envelope(buf, sizeof(buf), response_);
  if (len == 0) return;
  for (size_t i = 0; i < num_sinks_; i++) {
    if (sinks_[i] != NULL && sinks_[i] != except) {
      sinks_[i]->send_frame(buf, len);
    }
  }
}

void CommandProcessor::build_radio_settings() {
  response_.which_body = meshpigeon_RadioToClient_radio_settings_tag;
  RadioSettingsMessage& m = response_.body.radio_settings;
  m.freq_hz = settings_.freq_hz;
  // bw_x100khz is in 0.01 kHz units, i.e. 10 Hz steps.
  m.bandwidth_hz = (uint32_t)settings_.bw_x100khz * 10;
  m.sf = settings_.sf;
  m.cr = settings_.cr;
  m.power_dbm = settings_.power_dbm;
  m.config_epoch = settings_.config_epoch;
}

void CommandProcessor::build_device_settings() {
  response_.which_body = meshpigeon_RadioToClient_device_settings_tag;
  DeviceSettingsMessage& m = response_.body.device_settings;
  build_name(device_, m.name);
  m.wifi_enabled = device_.wifi_enabled;
  strncpy(m.wifi_ssid, device_.wifi_ssid, sizeof(m.wifi_ssid) - 1);
  m.wifi_ssid[sizeof(m.wifi_ssid) - 1] = 0;
  strncpy(m.wifi_password, device_.wifi_password,
          sizeof(m.wifi_password) - 1);
  m.wifi_password[sizeof(m.wifi_password) - 1] = 0;
  m.wifi_port = device_.wifi_port;
  // The PIN is write-only on the wire: no field of DeviceSettingsMessage
  // carries it, and none ever will (plan 14 §3.1).
}

void CommandProcessor::build_status() {
  response_.which_body = meshpigeon_RadioToClient_status_tag;
  StatusMessage& m = response_.body.status;
  if (hooks_) hooks_->fill_status(&m);
  m.ble_clients = hooks_ ? hooks_->ble_clients() : 0;
}

void CommandProcessor::send_error(uint32_t id, ErrorCode code,
                                  IFrameSink* to) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;
  response_.which_body = meshpigeon_RadioToClient_error_tag;
  response_.body.error.code = code;
  deliver(to);
}

void CommandProcessor::send_ok(uint32_t id, IFrameSink* to) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;
  response_.which_body = meshpigeon_RadioToClient_ok_tag;
  deliver(to);
}

void CommandProcessor::send_pong(uint32_t id, const pb_byte_t* payload,
                                 pb_size_t size, IFrameSink* to) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;
  response_.which_body = meshpigeon_RadioToClient_pong_tag;
  if (size > sizeof(response_.body.pong.payload.bytes)) {
    size = sizeof(response_.body.pong.payload.bytes);
  }
  if (payload != NULL) {
    memcpy(response_.body.pong.payload.bytes, payload, size);
  }
  response_.body.pong.payload.size = size;
  deliver(to);
}

void CommandProcessor::send_device_info(uint32_t id, IFrameSink* to) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;
  response_.which_body = meshpigeon_RadioToClient_device_info_tag;
  DeviceInfoMessage& m = response_.body.device_info;
  m.spec_version = MESHPIGEON_SPEC_VERSION;
  memcpy(m.fw_version, fw_version_, sizeof(m.fw_version) - 1);
  m.fw_version[sizeof(m.fw_version) - 1] = 0;
  memcpy(m.board_name, board_name_, sizeof(m.board_name) - 1);
  m.board_name[sizeof(m.board_name) - 1] = 0;
  m.capabilities_count =
      hooks_ ? hooks_->fill_capabilities(m.capabilities, m.capabilities_count)
             : 0;
  m.uptime_ms = clock_.uptime_ms64();
  m.boot_count = clock_.boot_count();
  m.has_store = true;
  m.store.count = store_.count();
  m.store.capacity_bytes = store_.capacity();
  m.store.dropped = store_.dropped();
  m.store.oldest_seq = store_.oldest_seq();
  m.radio_config_epoch = settings_.config_epoch;
  m.battery_mv = hooks_ ? hooks_->battery_mv() : 0xFFFF;
  m.radio_ok = radio_ok_;
  // §8: an unauthenticated client sees the lock before it hits it.
  m.auth_required = to != NULL && !to->authenticated && !device_.pin_is_default();
  deliver(to);
}

// ---- semantic pushes -------------------------------------------------------

void CommandProcessor::emit_tx_result(uint32_t seq, bool ok) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = 0;  // async push
  response_.which_body = meshpigeon_RadioToClient_tx_result_tag;
  response_.body.tx_result.seq = seq;
  response_.body.tx_result.success = ok;
  broadcast_response(NULL);
}

void CommandProcessor::notify_radio_changed(IFrameSink* except) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = 0;
  build_radio_settings();
  broadcast_response(except);
}

void CommandProcessor::notify_device_settings_changed(IFrameSink* except) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = 0;
  build_device_settings();
  broadcast_response(except);
}

void CommandProcessor::on_wifi_state_changed() {
  response_ = RadioToClientMessage_init_zero;
  response_.id = 0;
  build_status();
  broadcast_response(NULL);
}

// ---- auth ------------------------------------------------------------------

bool CommandProcessor::pin_matches(const char* pin, size_t len) const {
  if (len == 0 || len > MESHPIGEON_PIN_MAX) return false;
  return memcmp(pin, device_.pin, len) == 0 && device_.pin[len] == 0;
}

bool CommandProcessor::auth_backoff_active() const {
  return auth_backoff_until_ != 0 &&
         (int64_t)(clock_.uptime_ms64() - auth_backoff_until_) < 0;
}

void CommandProcessor::note_auth_failure() {
  auth_fails_++;
  if (auth_fails_ > kAuthFailsBeforeDelay) {
    auth_backoff_until_ = clock_.uptime_ms64() + kAuthFailDelayMs;
  }
}

bool CommandProcessor::require_auth(const ClientToRadioMessage& req,
                                    IFrameSink* from) {
  if (from == NULL) return true;  // test path: the auth op is tested directly
  if (from->authenticated) return true;
  if (device_.pin_is_default()) return true;  // ships open; the user's PIN gates
  send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
             from);
  return false;
}

// ---- the dispatcher --------------------------------------------------------

void CommandProcessor::on_envelope(const uint8_t* data, size_t len,
                                  IFrameSink* from) {
  ClientToRadioMessage req;
  if (!decode_envelope(data, len, &req)) {
    // Correlation id unrecoverable; this is a drop, not an error (§2).
    return;
  }
  handle_request(req, from);
}

void CommandProcessor::handle_request(const ClientToRadioMessage& req,
                                      IFrameSink* from) {
  switch (req.which_body) {
    case kOpPing: {
      send_pong(req.id, req.body.ping.payload.bytes, req.body.ping.payload.size,
                from);
      return;
    }

    case kOpGetDeviceInfo:
      send_device_info(req.id, from);
      return;

    case kOpGetRadioSettings: {
      if (!require_auth(req, from)) return;
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      build_radio_settings();
      deliver(from);
      return;
    }

    case kOpSetRadioSettings: {
      if (!require_auth(req, from)) return;
      const RadioSettingsMessage& m = req.body.set_radio_settings.settings;
      RadioSettings s;
      s.version = RadioSettings::kSerializedVersion;
      s.region = 0;  // the region-preset concept is gone (plan 14 §6.3)
      s.freq_hz = m.freq_hz;
      // Plain Hz on the wire, 0.01 kHz units internally (10 Hz steps).
      s.bw_x100khz = (uint16_t)(m.bandwidth_hz / 10);
      s.sf = (uint8_t)m.sf;
      s.cr = (uint8_t)m.cr;
      s.power_dbm = (uint8_t)m.power_dbm;
      s.config_epoch = settings_.config_epoch;
      if (s.freq_hz == 0 || s.bw_x100khz == 0 || s.sf < 5 || s.sf > 12 ||
          s.cr < 5 || s.cr > 8 || m.power_dbm > 255) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      if (first_owner_lock_active()) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, from);
        return;
      }
      if (!radio_.apply(s)) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_TX_FAILED,
                   from);
        return;
      }
      s.config_epoch = settings_.config_epoch + 1;
      settings_ = s;
      settings_store_.save(s);  // persists on every SET_RADIO (04 §1.4)
      set_count_since_boot_++;
      // Multi-client: everyone else learns about the change (05 §3).
      notify_radio_changed(from);
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      build_radio_settings();  // post-bump, to the issuer
      deliver(from);
      return;
    }

    case kOpSendPacket: {
      if (!require_auth(req, from)) return;
      const pb_byte_t* raw = req.body.send_packet.raw.bytes;
      pb_size_t n = req.body.send_packet.raw.size;
      if (n == 0 || n > MESHPIGEON_MAX_RAW_PACKET) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      if (tx_pending_ != 0) {
        // One TX at a time: the radio keys up serially. Report busy; the
        // app's outbox owns retry policy (04 §1.2).
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, from);
        return;
      }
      uint32_t seq =
          store_.append(clock_.uptime_ms64(), 0, 0, kFlagsSent, raw, n);
      tx_pending_ = 1;
      tx_pending_seq_ = seq;
      tx_start_ok_ = radio_.transmit(raw, n) == 0;
      if (!tx_start_ok_) tx_pending_ = 0;  // refused immediately: report, don't wait
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      response_.which_body = meshpigeon_RadioToClient_packet_accepted_tag;
      response_.body.packet_accepted.seq = seq;
      deliver(from);
      if (!tx_start_ok_) emit_tx_result(seq, false);
      return;
    }

    case kOpFetchPackets: {
      if (!require_auth(req, from)) return;
      const uint32_t id = req.id;
      uint32_t delivered = 0;
      store_.fetch_since(req.body.fetch_packets.since_seq,
                         req.body.fetch_packets.max_count,
                         [&](const StoredPacket& e) {
                           response_ = RadioToClientMessage_init_zero;
                           response_.id = id;
                           response_.which_body =
                               meshpigeon_RadioToClient_packet_entry_tag;
                           response_.body.packet_entry.seq = e.seq;
                           response_.body.packet_entry.raw.size = e.len;
                           memcpy(response_.body.packet_entry.raw.bytes, e.raw,
                                  e.len);
                           response_.body.packet_entry.uptime_ms = e.uptime_ms;
                           response_.body.packet_entry.rssi = e.rssi;
                           response_.body.packet_entry.snr = e.snr;
                           response_.body.packet_entry.origin =
                               (e.flags & kFlagsSent)
                                   ? meshpigeon_PacketEntry_Origin_ORIGIN_SENT
                                   : meshpigeon_PacketEntry_Origin_ORIGIN_RECEIVED;
                           deliver(from);
                           delivered++;
                           return true;
                         });
      response_ = RadioToClientMessage_init_zero;
      response_.id = id;
      response_.which_body = meshpigeon_RadioToClient_fetch_end_tag;
      response_.body.fetch_end.count = delivered;
      deliver(from);
      return;
    }

    case kOpPurgeStore: {
      if (!require_auth(req, from)) return;
      store_.clear();
      send_ok(req.id, from);
      return;
    }

    case kOpBootloader: {
      // Always allowed: DFU must be reachable on a locked device (§8).
      send_ok(req.id, from);
      if (hooks_) hooks_->reboot_to_bootloader();
      return;
    }

    case kOpGetDeviceSettings: {
      // The Wi-Fi password lives in here, hence the gate (§6).
      if (!require_auth(req, from)) return;
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      build_device_settings();
      deliver(from);
      return;
    }

    case kOpSetDeviceSettings: {
      if (!require_auth(req, from)) return;
      const SetDeviceSettingsMessage& m = req.body.set_device_settings;
      DeviceSettings next = device_;
      bool wifi_touched = m.has_wifi_enabled || m.has_wifi_ssid ||
                          m.has_wifi_password || m.has_wifi_port;
      // Validate everything first: one bad field rejects the whole request
      // with nothing applied (plan 13 §6).
      if ((m.has_name &&
           !DeviceSettings::valid_name(m.name, strlen(m.name))) ||
          (m.has_pin && !DeviceSettings::valid_pin(m.pin, strlen(m.pin))) ||
          (m.has_wifi_ssid &&
           !DeviceSettings::valid_ssid(m.wifi_ssid, strlen(m.wifi_ssid))) ||
          (m.has_wifi_password &&
           !DeviceSettings::valid_password(m.wifi_password,
                                           strlen(m.wifi_password))) ||
          (m.has_wifi_port && !DeviceSettings::valid_port(m.wifi_port))) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      if (wifi_touched && hooks_ != NULL && !hooks_->wifi_supported()) {
        // Capability-gated: the board would never honor these; reject
        // atomically so a stale client can't leave a dead setting behind
        // (plan 13 §7).
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                   from);
        return;
      }
      // ---- apply: validated, so these all succeed ----
      if (m.has_name) {
        strncpy(next.name, m.name, MESHPIGEON_NAME_MAX);
        next.name[MESHPIGEON_NAME_MAX] = 0;
      }
      if (m.has_pin) {
        strncpy(next.pin, m.pin, MESHPIGEON_PIN_MAX);
        next.pin[MESHPIGEON_PIN_MAX] = 0;
        auth_fails_ = 0;         // the lock changed: fresh brute-force budget
        auth_backoff_until_ = 0;  // and no pending penalty
      }
      if (m.has_wifi_enabled) next.wifi_enabled = m.wifi_enabled;
      if (m.has_wifi_ssid) {
        strncpy(next.wifi_ssid, m.wifi_ssid, MESHPIGEON_SSID_MAX);
        next.wifi_ssid[MESHPIGEON_SSID_MAX] = 0;
      }
      if (m.has_wifi_password) {
        strncpy(next.wifi_password, m.wifi_password, MESHPIGEON_PASS_MAX);
        next.wifi_password[MESHPIGEON_PASS_MAX] = 0;
      }
      if (m.has_wifi_port) next.wifi_port = (uint16_t)m.wifi_port;
      device_ = next;
      settings_store_.save_device(device_);  // persists immediately (04 §1.4)
      if (m.has_name && hooks_) {            // rename + re-advertise (§9)
        char effective[MESHPIGEON_NAME_MAX + 1];
        effective_name(effective);
        hooks_->set_device_name(effective);
      }
      if (wifi_touched && hooks_) hooks_->apply_wifi(device_);
      // Everyone else learns about the change — the RADIO_CHANGED analogue.
      notify_device_settings_changed(from);
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      build_device_settings();  // full post-write state to the issuer
      deliver(from);
      return;
    }

    case kOpGetStatus: {
      // Readable on every transport, auth or not: this is how a client on
      // BLE learns what the Wi-Fi is doing (plan 13 §11).
      response_ = RadioToClientMessage_init_zero;
      response_.id = req.id;
      build_status();
      deliver(from);
      return;
    }

    case kOpAuth: {
      if (from == NULL) return;
      if (auth_backoff_active()) {  // slow brute force, not even evaluated (§8)
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                   from);
        return;
      }
      const AuthMessage& m = req.body.auth;
      if (from->authenticated || device_.pin_is_default() ||
          pin_matches(m.pin, strlen(m.pin))) {
        from->authenticated = true;
        auth_fails_ = 0;
        send_ok(req.id, from);
        return;
      }
      note_auth_failure();
      send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED,
                 from);
      return;
    }

    case kOpReboot: {
      if (!require_auth(req, from)) return;
      send_ok(req.id, from);
      if (hooks_) hooks_->reboot();
      return;
    }

    case kOpFactoryReset: {
      if (!require_auth(req, from)) return;
      device_.clear();
      settings_store_.save_device(device_);
      store_.clear();  // history too: "as it shipped" (plan 13 §10.4)
      send_ok(req.id, from);
      if (hooks_) hooks_->factory_reset();  // wipe persistence, then reboot
      return;
    }

    default:
      // Unknown oneof variant: newer client, older radio (evolution policy).
      send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_COMMAND,
                 from);
      return;
  }
}

uint32_t CommandProcessor::on_packet_received(int8_t rssi, int8_t snr,
                                              const uint8_t* raw, uint8_t len) {
  if (len > MESHPIGEON_MAX_RAW_PACKET) len = MESHPIGEON_MAX_RAW_PACKET;
  uint32_t seq =
      store_.append(clock_.uptime_ms64(), rssi, snr, kFlagsReceived, raw, len);
  // Live push while connected (04 §4), id = 0.
  response_ = RadioToClientMessage_init_zero;
  response_.id = 0;
  response_.which_body = meshpigeon_RadioToClient_packet_entry_tag;
  response_.body.packet_entry.seq = seq;
  response_.body.packet_entry.raw.size = len;
  memcpy(response_.body.packet_entry.raw.bytes, raw, len);
  response_.body.packet_entry.uptime_ms = clock_.uptime_ms64();
  response_.body.packet_entry.rssi = rssi;
  response_.body.packet_entry.snr = snr;
  response_.body.packet_entry.origin =
      meshpigeon_PacketEntry_Origin_ORIGIN_RECEIVED;
  broadcast_response(NULL);
  return seq;
}

void CommandProcessor::poll() {
  clock_.poll();
  if (tx_pending_ != 0 && radio_.tx_done()) {
    on_tx_result(tx_pending_seq_, true);
  }
}

void CommandProcessor::on_tx_result(uint32_t seq, bool ok) {
  if (tx_pending_ == 0 || seq != tx_pending_seq_) return;
  tx_pending_ = 0;
  emit_tx_result(seq, ok);
}

}  // namespace meshpigeon
