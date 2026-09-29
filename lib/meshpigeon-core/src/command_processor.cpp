#include "meshpigeon/command_processor.h"

#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

namespace meshpigeon {

static const uint32_t kFirstOwnerGraceMs = 5 * 60 * 1000;  // docs/radio-protocol.md §6.1
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

void CommandProcessor::effective_name(
    char out[MESHPIGEON_NAME_MAX + 1]) const {
  if (device_.name[0] != 0) {
    memcpy(out, device_.name, MESHPIGEON_NAME_MAX);
    out[MESHPIGEON_NAME_MAX] = 0;
    return;
  }
  char suffix[5] = "0000";
  if (hooks_) hooks_->mac_suffix(suffix);
  suffix[4] = 0;
  snprintf(out, MESHPIGEON_NAME_MAX + 1, "%s%s", kDefaultNamePrefix, suffix);
}

bool CommandProcessor::boot() {
  // Settings load even when the radio is dead: the device still answers, and
  // the app needs its name, PIN and store stats (docs/radio-protocol.md §5).
  //
  // A failed load leaves the safe default in place and does NOT persist it
  // (avoid flash wear until an app tunes us); a later apply() is what writes.
  RadioSettings stored = RadioSettings::unset();
  settings_ = RadioSettings::unset();
  set_count_since_boot_ = 0;
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
  // The ONE place a response is reset. Every builder below assumes it: it
  // zeroes the whole envelope, union included, so a field a builder (or a
  // board hook) does not set reads as "unset" instead of as whatever the
  // previous response left behind. Any new builder that skips this inherits
  // the previous response's fields — see AGENTS.md §6.
  response_ = RadioToClientMessage_init_zero;
  response_.id = id;  // 0 marks an async push
}

size_t CommandProcessor::encode_response(uint8_t* buf) {
  return encode_envelope(buf, MESHPIGEON_MAX_FRAME_PAYLOAD, response_);
}

void CommandProcessor::dispatch(IFrameSink* to, IFrameSink* except,
                                bool authorized_only) {
  // Encoded once and reused for every recipient: a FetchPackets burst or a
  // Status push is one serialization, not one per sink.
  uint8_t buf[MESHPIGEON_MAX_FRAME_PAYLOAD];
  size_t len = encode_response(buf);
  if (len == 0) return;
  if (to != NULL) {
    to->send_frame(buf, len);
    return;
  }
  for (size_t i = 0; i < num_sinks_; i++) {
    if (sinks_[i] == NULL || sinks_[i] == except) continue;
    // `authorized_only` is for the three pushes that carry protected
    // material: a broadcast is not an operation, so without it a stranger
    // who merely attached a socket would collect the Wi-Fi passphrase from
    // every rename, every packet off the air, and the node's tuning from
    // every retune by someone else (docs/radio-protocol.md §3, §8.2).
    if (authorized_only && !is_authorized(sinks_[i])) continue;
    sinks_[i]->send_frame(buf, len);
  }
}

void CommandProcessor::build_radio_settings() {
  // begin_response() has already zeroed the body; see its comment.
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
  effective_name(m.name);
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
      (e.flags & kFlagSent)
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
  // GetRadioSettings is auth-gated, so this push of the same read model is
  // filtered by the same question: a client that may not ask for the tuning
  // has no business being told it. Costs an authorized peer nothing — they
  // are exactly the recipients the filter keeps.
  broadcast_response(except, /*authorized_only=*/true);
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
  // Saturating, not wrapping: a uint8_t that rolled back to 0 would hand the
  // attacker a free attempt every time it came round (the window is only
  // re-armed above the threshold, and a reset count is below it).
  if (auth_fails_ < 0xFF) auth_fails_++;
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
      // Start from a fully-initialized struct: the radio port keeps its own
      // copy of what it is handed, and this one is persisted. The region
      // preset the v1 layout carried is gone, so unset()'s region 0 is what
      // a new write always carries (docs/radio-protocol.md §6).
      RadioSettings s = RadioSettings::unset();
      s.freq_hz = m.freq_hz;
      // Plain Hz on the wire, 0.01 kHz units internally (10 Hz steps). The
      // conversion has to be validated on the wire value, not the truncated
      // one: a bandwidth of 655370 Hz would otherwise wrap to 10 Hz.
      // power_dbm is bounded to the silicon's ceiling because the value is
      // narrowed to int8_t on its way to the radio: anything above 127 would
      // arrive negative (200 -> -56 dBm) and key up at the wrong power
      // instead of failing. Validating it here is the only place that can
      // still reject it.
      if (m.bandwidth_hz == 0 || m.bandwidth_hz % 10 != 0 ||
          m.bandwidth_hz / 10 > UINT16_MAX || m.freq_hz == 0 || m.sf < 5 ||
          m.sf > 12 || m.cr < 5 || m.cr > 8 ||
          m.power_dbm > MESHPIGEON_TX_POWER_MAX) {
        send_error(req.id, meshpigeon_Error_ErrorCode_ERROR_CODE_BAD_PAYLOAD,
                   from);
        return;
      }
      s.bw_x100khz = (uint16_t)(m.bandwidth_hz / 10);
      s.sf = (uint8_t)m.sf;
      s.cr = (uint8_t)m.cr;
      s.power_dbm = (uint8_t)m.power_dbm;
      if (first_owner_lock_active() || tx_pending_) {
        // §6.1: within the grace window only the first-connected client owns
        // the tuning decision. §4: a retune while the radio is keying up is
        // BUSY for the same reason a second send is — and here it is not
        // merely politeness. The tuning sequence puts the part in standby,
        // which aborts the transmission on the silicon, so the in-flight
        // TX would never complete and every later SendPacket would answer
        // BUSY until the board was power-cycled. Refuse instead.
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
      s.config_epoch = settings_.config_epoch + 1;  // the firmware's own bump
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
          store_.append(clock_.uptime_ms64(), 0, 0, kFlagSent, raw, n, NULL);
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
      // max_count = 0 means "everything", the same convention since_seq = 0
      // uses: 0 is never a meaningful bound, and a client that meant "all of
      // them" would otherwise get an empty FetchEnd and read it as an empty
      // store. It is clamped like any other value.
      if (max_count == 0 || max_count > kMaxFetchPerRequest) {
        max_count = kMaxFetchPerRequest;
      }
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
      // ---- apply: validated, so every one of these succeeds. Build the
      // whole candidate first and decide once whether it is a change, so
      // the side effects below can only ever run for a real change. ----
      if (m.has_name) {
        strncpy(next.name, m.name, MESHPIGEON_NAME_MAX);
        next.name[MESHPIGEON_NAME_MAX] = 0;
      }
      if (m.has_pin) DeviceSettings::set_pin(next.pin, m.pin);
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

      // A write that lands on the values already in force is a no-op, and a
      // no-op is not a change: no flash write (the persistence inventory is
      // closed and NVS erases wear out), nothing re-advertised or re-applied,
      // and no "another client changed settings" push to wake every other
      // client with. The response is still sent — the client asked for the
      // read model. Rewriting the PIN with the value it already holds is
      // therefore a no-op too, so it does not de-authorize the other
      // connections: a rotation moves the flags, a no-op does not
      // (docs/radio-protocol.md §8.1).
      if (next == device_) {
        begin_response(req.id);
        build_device_settings();
        deliver(from);
        return;
      }
      device_ = next;

      if (m.has_pin) {
        // The lock changed, so the lock rule now applies in both directions:
        // every other connection loses the session it held (a peer that knew
        // the old PIN must not survive a rotation), and the writer gains
        // one. Without the second half, setting a PIN from the shipped
        // default would lock the writer out of the device it just
        // configured — it proved it knows the new PIN by writing it
        // (docs/radio-protocol.md §8.1).
        from->authenticated = true;
        for (size_t i = 0; i < num_sinks_; i++) {
          if (sinks_[i] != NULL && sinks_[i] != from) {
            sinks_[i]->authenticated = false;
          }
        }
        auth_fails_ = 0;         // the lock changed: fresh brute-force budget
        auth_backoff_until_ = 0;  // and no pending penalty
      }
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
      store_.append(clock_.uptime_ms64(), rssi, snr, kFlagReceived, raw, len,
                    &e);
  // seq 0 means the store dropped the packet outright (larger than the whole
  // pool), and a live push for a seq the app could never fetch again would
  // be a lie.
  if (seq == 0) return 0;
  // Live push while connected (docs/radio-protocol.md §10), id = 0. Like
  // the DeviceSettings push, it is filtered: FetchPackets is auth-gated, so
  // pushing the same raw bytes to an unauthenticated socket that merely
  // attached would hand a stranger every packet the node hears and make the
  // gate on fetch pointless. Connections on the shipped default PIN are
  // authorized, so an unconfigured device is unaffected.
  build_packet_entry(0, e);
  broadcast_response(NULL, /*authorized_only=*/true);
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
