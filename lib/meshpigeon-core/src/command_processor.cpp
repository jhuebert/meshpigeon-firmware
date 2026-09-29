#include "meshpigeon/command_processor.h"

#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

namespace meshpigeon {

static const uint32_t kFirstOwnerGraceMs = 5 * 60 * 1000;  // docs/radio-protocol.md §6.1
static const uint8_t kFlagsSent = 0x01;
static const uint8_t kFlagsReceived = 0x02;
static const uint8_t kAuthFailsBeforeDelay = 3;
static const uint32_t kAuthFailDelayMs = 1000;
static const char kDefaultNamePrefix[] = "MeshPigeon-";
// Upper bound on one FetchPackets response. The stream is written straight
// out of the sink inside a single request handler, so an unbounded
// max_count would let one client hold the board loop (and a BLE queue) for
// as long as the store is deep. Clients resume from the last seq they got
// (docs/radio-protocol.md §10).
static const uint32_t kMaxFetchPerRequest = 64;

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
  fw_version_[sizeof(fw_version_) - 1] = 0;
  memset(sinks_, 0, sizeof(sinks_));
}

bool CommandProcessor::add_sink(IFrameSink* sink) {
  if (num_sinks_ >= kMaxSinks) return false;
  sinks_[num_sinks_++] = sink;
  return true;
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
  // Settings load even when the radio is dead: the device still answers, and
  // the app needs its name, PIN and store stats (docs/radio-protocol.md §5).
  //
  // A failed load leaves the safe default in place and does NOT persist it
  // (avoid flash wear until an app tunes us); a later apply() is what writes.
  RadioSettings stored;
  settings_ = RadioSettings::unset();
  if (settings_store_.load(&stored)) settings_ = stored;
  // A false load means "never written": the store hands back the defaults.
  settings_store_.load_device(&device_);
  if (device_.wifi_port == 0) device_.wifi_port = MESHPIGEON_WIFI_PORT_DEFAULT;
  radio_ok_ = radio_.begin() && radio_.apply(settings_);
  return radio_ok_;
}

bool CommandProcessor::first_owner_lock_active() const {
  // docs/radio-protocol.md §6.1: in the grace window after boot only the first
  // SET_RADIO is honored — the first-connected client owns the tuning decision.
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

void CommandProcessor::begin_response(uint32_t id) {
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;  // 0 marks an async push
}

size_t CommandProcessor::encode_response(uint8_t* buf) {
  return encode_envelope(buf, MESHPIGEON_MAX_FRAME_PAYLOAD, response_);
}

void CommandProcessor::deliver(IFrameSink* to) {
  if (to == NULL) return;
  uint8_t buf[MESHPIGEON_MAX_FRAME_PAYLOAD];
  size_t len = encode_response(buf);
  if (len != 0) to->send_frame(buf, len);
}

void CommandProcessor::broadcast_response(IFrameSink* except,
                                          bool authorized_only) {
  // Encoded once and reused for every recipient: a FetchPackets burst or a
  // Status push is one serialization, not one per sink.
  uint8_t buf[MESHPIGEON_MAX_FRAME_PAYLOAD];
  size_t len = encode_response(buf);
  if (len == 0) return;
  for (size_t i = 0; i < num_sinks_; i++) {
    if (sinks_[i] == NULL || sinks_[i] == except) continue;
    // `authorized_only` is for the one push that carries a credential: a
    // broadcast is not an operation, so without it a stranger who merely
    // attached a socket would collect the Wi-Fi passphrase from every
    // rename (docs/radio-protocol.md §8.3).
    if (authorized_only && !is_authorized(sinks_[i])) continue;
    sinks_[i]->send_frame(buf, len);
  }
}

void CommandProcessor::build_radio_settings() {
  response_.which_body = meshpigeon_RadioToClient_radio_settings_tag;
  // response_.body is a union: start from a zeroed message so a field this
  // builder does not set cannot inherit the previous response's value.
  // (A local, not `body.x = {...}`: the xtensa toolchain rejects assigning a
  // brace initializer even though gcc and arm-none-eabi accept it.)
  const RadioSettingsMessage empty = RadioSettingsMessage_init_zero;
  response_.body.radio_settings = empty;
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
  const DeviceSettingsMessage empty = DeviceSettingsMessage_init_zero;
  response_.body.device_settings = empty;
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
  // carries it, and none ever will (docs/radio-protocol.md §8).
}

void CommandProcessor::build_status() {
  response_.which_body = meshpigeon_RadioToClient_status_tag;
  // Zeroed first, so a hook that only fills the fields it knows about (a
  // board with no Wi-Fi, say) leaves the rest reading as "unknown" instead
  // of as the previous Status's values.
  const StatusMessage empty = StatusMessage_init_zero;
  response_.body.status = empty;
  StatusMessage& m = response_.body.status;
  // The hook owns the link state; the client counts are the core's, because
  // the sinks it broadcasts to are the core's registry.
  if (hooks_) hooks_->fill_status(&m);
  m.ble_clients = hooks_ ? hooks_->ble_clients() : 0;
  m.usb_cdc_clients = hooks_ ? hooks_->usb_cdc_clients() : 0;
  m.wifi_tcp_clients = hooks_ ? hooks_->wifi_tcp_clients() : 0;
}

void CommandProcessor::build_packet_entry(uint32_t id, const StoredPacket& e) {
  begin_response(id);
  response_.which_body = meshpigeon_RadioToClient_packet_entry_tag;
  response_.body.packet_entry.seq = e.seq;
  response_.body.packet_entry.raw.size = e.len;
  memcpy(response_.body.packet_entry.raw.bytes, e.raw, e.len);
  response_.body.packet_entry.uptime_ms = e.uptime_ms;
  response_.body.packet_entry.rssi = e.rssi;
  response_.body.packet_entry.snr = e.snr;
  response_.body.packet_entry.origin =
      (e.flags & kFlagsSent)
          ? meshpigeon_PacketEntry_Origin_ORIGIN_SENT
          : meshpigeon_PacketEntry_Origin_ORIGIN_RECEIVED;
}

void CommandProcessor::send_error(uint32_t id, ErrorCode code,
                                  IFrameSink* to) {
  begin_response(id);
  response_.which_body = meshpigeon_RadioToClient_error_tag;
  response_.body.error.code = code;
  deliver(to);
}

void CommandProcessor::send_ok(uint32_t id, IFrameSink* to) {
  begin_response(id);
  response_.which_body = meshpigeon_RadioToClient_ok_tag;
  deliver(to);
}

void CommandProcessor::send_pong(uint32_t id, const pb_byte_t* payload,
                                 pb_size_t size, IFrameSink* to) {
  begin_response(id);
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

void CommandProcessor::send_device_info(uint32_t id, IFrameSink* from) {
  begin_response(id);
  response_.which_body = meshpigeon_RadioToClient_device_info_tag;
  DeviceInfoMessage& m = response_.body.device_info;
  m.spec_version = MESHPIGEON_SPEC_VERSION;
  memcpy(m.fw_version, fw_version_, sizeof(m.fw_version) - 1);
  m.fw_version[sizeof(m.fw_version) - 1] = 0;
  memcpy(m.board_name, board_name_, sizeof(m.board_name) - 1);
  m.board_name[sizeof(m.board_name) - 1] = 0;
  m.capabilities_count =
      hooks_ ? hooks_->fill_capabilities(m.capabilities, kMaxCapabilities) : 0;
  if (m.capabilities_count > kMaxCapabilities) {
    m.capabilities_count = kMaxCapabilities;  // a board that over-reports
  }
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
  m.noise_floor_dbm = noise_floor_dbm_;
  // §8.2: an unauthenticated client sees the lock before it hits it.
  m.auth_required = !is_authorized(from);
  deliver(from);
}

// ---- semantic pushes -------------------------------------------------------

void CommandProcessor::emit_tx_result(uint32_t seq, bool ok) {
  begin_response(0);
  response_.which_body = meshpigeon_RadioToClient_tx_result_tag;
  response_.body.tx_result.seq = seq;
  response_.body.tx_result.success = ok;
  broadcast_response(NULL);
}

void CommandProcessor::notify_radio_changed(IFrameSink* except) {
  begin_response(0);
  build_radio_settings();
  broadcast_response(except);
}

void CommandProcessor::notify_device_settings_changed(IFrameSink* except) {
  begin_response(0);
  build_device_settings();
  broadcast_response(except, /*authorized_only=*/true);
}

void CommandProcessor::on_wifi_state_changed() {
  begin_response(0);
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

bool CommandProcessor::is_authorized(const IFrameSink* from) const {
  // The device ships open (the public default PIN); a user-chosen PIN is
  // what actually gates it. Every gate and every filter asks this one
  // question, so they cannot drift apart (docs/radio-protocol.md §8.2).
  return from->authenticated || device_.pin_is_default();
}

bool CommandProcessor::require_auth(uint32_t id, IFrameSink* from) {
  if (is_authorized(from)) return true;
  send_error(id, meshpigeon_Error_ErrorCode_ERROR_CODE_AUTH_REQUIRED, from);
  return false;
}

// ---- the dispatcher --------------------------------------------------------

void CommandProcessor::on_envelope(const uint8_t* data, size_t len,
                                  IFrameSink* from) {
  if (from == NULL) return;  // no connection, no auth state, no reply
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
      if (!require_auth(req.id, from)) return;
      begin_response(req.id);
      build_radio_settings();
      deliver(from);
      return;
    }

    case kOpSetRadioSettings: {
      if (!require_auth(req.id, from)) return;
      // nanopb decodes an absent submessage as an empty one, so has_settings
      // is the only thing that distinguishes "no settings at all" from "all
      // defaults" (AGENTS.md §6).
      if (!req.body.set_radio_settings.has_settings) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      const RadioSettingsMessage& m = req.body.set_radio_settings.settings;
      RadioSettings s;
      s.version = RadioSettings::kSerializedVersion;
      s.region = 0;  // the region-preset concept is gone (docs/radio-protocol.md §6)
      s.freq_hz = m.freq_hz;
      // Plain Hz on the wire, 0.01 kHz units internally (10 Hz steps). The
      // conversion has to be validated on the wire value, not the truncated
      // one: a bandwidth of 655370 Hz would otherwise wrap to 10 Hz.
      if (m.bandwidth_hz == 0 || m.bandwidth_hz % 10 != 0 ||
          m.bandwidth_hz / 10 > UINT16_MAX || m.freq_hz == 0 || m.sf < 5 ||
          m.sf > 12 || m.cr < 5 || m.cr > 8 || m.power_dbm > UINT8_MAX) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      s.bw_x100khz = (uint16_t)(m.bandwidth_hz / 10);
      s.sf = (uint8_t)m.sf;
      s.cr = (uint8_t)m.cr;
      s.power_dbm = (uint8_t)m.power_dbm;
      s.config_epoch = settings_.config_epoch;
      if (first_owner_lock_active()) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, from);
        return;
      }
      // A rejected tuning leaves the previous one in force, and the previous
      // one is what the device is still using — so it must not clear
      // radio_ok_. Only boot() does that, when the radio failed to come up or
      // could not accept what was persisted (docs/radio-protocol.md §5).
      if (!radio_.apply(s)) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_TX_FAILED,
                   from);
        return;
      }
      radio_ok_ = true;  // it just accepted a tuning: the air is usable
      s.config_epoch = settings_.config_epoch + 1;
      settings_ = s;
      settings_store_.save(s);  // persists on every SET_RADIO (docs/radio-protocol.md §6)
      set_count_since_boot_++;
      // Multi-client: everyone else learns about the change (docs/radio-protocol.md §6.1).
      notify_radio_changed(from);
      begin_response(req.id);
      build_radio_settings();  // post-bump, to the issuer
      deliver(from);
      return;
    }

    case kOpSendPacket: {
      if (!require_auth(req.id, from)) return;
      const pb_byte_t* raw = req.body.send_packet.raw.bytes;
      pb_size_t n = req.body.send_packet.raw.size;
      if (n == 0 || n > MESHPIGEON_MAX_RAW_PACKET) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      if (!radio_ok_) {
        // No air to use: refuse before storing, so the app never gets a
        // TxResult(success) for a packet that was never keyed up.
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_NO_RADIO,
                   from);
        return;
      }
      if (tx_pending_) {
        // One TX at a time: the radio keys up serially. Report busy; the
        // app's outbox owns retry policy (docs/radio-protocol.md §10).
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, from);
        return;
      }
      uint32_t seq =
          store_.append(clock_.uptime_ms64(), 0, 0, kFlagsSent, raw, n, NULL);
      if (seq == 0) {
        // The store could not take the packet at all (a pool that cannot
        // hold it). Refuse before accepting: a PacketAccepted for a seq that
        // can never be fetched — and a TxResult for it — would both lie.
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BUSY, from);
        return;
      }
      // The response comes first (it is what the app is waiting on), then
      // the outcome — even when the radio refused the send outright.
      begin_response(req.id);
      response_.which_body = meshpigeon_RadioToClient_packet_accepted_tag;
      response_.body.packet_accepted.seq = seq;
      deliver(from);
      if (radio_.transmit(raw, n) != 0) {
        emit_tx_result(seq, false);  // refused immediately: no wait
        return;
      }
      tx_pending_ = true;
      tx_pending_seq_ = seq;
      return;
    }

    case kOpFetchPackets: {
      if (!require_auth(req.id, from)) return;
      const uint32_t id = req.id;
      uint32_t max_count = req.body.fetch_packets.max_count;
      if (max_count > kMaxFetchPerRequest) max_count = kMaxFetchPerRequest;
      uint32_t delivered = 0;
      store_.fetch_since(req.body.fetch_packets.since_seq, max_count,
                         [&](const StoredPacket& e) {
                           build_packet_entry(id, e);
                           deliver(from);
                           delivered++;
                           return true;
                         });
      begin_response(id);
      response_.which_body = meshpigeon_RadioToClient_fetch_end_tag;
      response_.body.fetch_end.count = delivered;
      deliver(from);
      return;
    }

    case kOpPurgeStore: {
      if (!require_auth(req.id, from)) return;
      store_.clear();
      send_ok(req.id, from);
      return;
    }

    case kOpBootloader: {
      // Always allowed: flashing must be reachable on a locked device (§8.2).
      send_ok(req.id, from);
      if (hooks_) hooks_->reboot_to_bootloader();
      return;
    }

    case kOpGetDeviceSettings: {
      // The Wi-Fi password lives in here, hence the gate (§8).
      if (!require_auth(req.id, from)) return;
      begin_response(req.id);
      build_device_settings();
      deliver(from);
      return;
    }

    case kOpSetDeviceSettings: {
      if (!require_auth(req.id, from)) return;
      const SetDeviceSettingsMessage& m = req.body.set_device_settings;
      DeviceSettings next = device_;
      bool wifi_touched = m.has_wifi_enabled || m.has_wifi_ssid ||
                          m.has_wifi_password || m.has_wifi_port;
      // Validate everything first: one bad field rejects the whole request
      // with nothing applied (docs/radio-protocol.md §8.1).
      if ((m.has_name &&
           !DeviceSettings::valid_name(m.name, strlen(m.name))) ||
          (m.has_pin &&
           !DeviceSettings::valid_pin_set(m.pin, strlen(m.pin))) ||
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
        // (docs/radio-protocol.md §8.1).
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
        DeviceSettings::set_pin(next.pin, m.pin);
        // The lock changed, so every session that knew the *old* PIN is no
        // longer entitled to the new one — including the issuer's peers
        // (docs/radio-protocol.md §8.1 "a PIN change re-gates immediately").
        for (size_t i = 0; i < num_sinks_; i++) {
          if (sinks_[i] != NULL && sinks_[i] != from) {
            sinks_[i]->authenticated = false;
          }
        }
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
      settings_store_.save_device(device_);  // persists immediately (docs §8.1)
      if (m.has_name && hooks_) {            // rename + re-advertise (§8.3)
        char effective[MESHPIGEON_NAME_MAX + 1];
        effective_name(effective);
        hooks_->set_device_name(effective);
      }
      if (wifi_touched && hooks_) hooks_->apply_wifi(device_);
      // Everyone else learns about the change — the RADIO_CHANGED analogue.
      notify_device_settings_changed(from);
      begin_response(req.id);
      build_device_settings();  // full post-write state to the issuer
      deliver(from);
      return;
    }

    case kOpGetStatus: {
      // Readable on every transport, auth or not: this is how a client on
      // BLE learns what the Wi-Fi is doing (docs/radio-protocol.md §9).
      begin_response(req.id);
      build_status();
      deliver(from);
      return;
    }

    case kOpAuth: {
      if (auth_backoff_active()) {  // slow brute force, not even evaluated (§8.2)
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
      if (!require_auth(req.id, from)) return;
      send_ok(req.id, from);
      if (hooks_) hooks_->reboot();
      return;
    }

    case kOpFactoryReset: {
      if (!require_auth(req.id, from)) return;
      device_.clear();              // in RAM too: the response must agree
      // The persistence layer forgets the record rather than having defaults
      // written back over it (docs/radio-protocol.md §10).
      settings_store_.clear_device();
      store_.clear();  // history too: "as it shipped" (docs/radio-protocol.md §10)
      send_ok(req.id, from);
      if (hooks_) hooks_->factory_reset();  // then reboot
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
  // One reading of the clock: the stored stamp and the wire stamp must not
  // disagree across a rollover (docs/radio-protocol.md §5.1).
  // The noise-floor estimate is taken from whatever arrived, stored or not:
  // it is a reading of the receiver, not of the store.
  noise_floor_dbm_ = (int32_t)rssi - (int32_t)snr;  // the last estimate wins
  StoredPacket e;
  uint32_t seq =
      store_.append(clock_.uptime_ms64(), rssi, snr, kFlagsReceived, raw, len,
                    &e);
  // seq 0 means the store dropped the packet outright (larger than the whole
  // pool), and a live push for a seq the app could never fetch again would
  // be a lie.
  if (seq == 0) return 0;
  // Live push while connected (docs/radio-protocol.md §10), id = 0.
  build_packet_entry(0, e);
  broadcast_response(NULL);
  return seq;
}

void CommandProcessor::poll() {
  clock_.poll();
  if (tx_pending_ && radio_.tx_done()) {
    // Started transmissions are reported as sent: the radio layer refuses
    // synchronously (ERROR_CODE_NO_RADIO / TxResult(false)) long before this
    // point, so anything still in flight did key up.
    on_tx_result(tx_pending_seq_, true);
  }
}

void CommandProcessor::on_tx_result(uint32_t seq, bool ok) {
  if (!tx_pending_ || seq != tx_pending_seq_) return;
  tx_pending_ = false;
  emit_tx_result(seq, ok);
}

}  // namespace meshpigeon
