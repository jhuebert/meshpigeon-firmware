#include "meshpigeon/command_processor.h"

#include <string.h>

#include "meshpigeon/framing.h"

namespace meshpigeon {

static const uint32_t kFirstOwnerGraceMs = 5 * 60 * 1000;  // 05 §3
static const uint8_t kFlagsSent = 0x01;
static const uint8_t kFlagsReceived = 0x02;

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

bool CommandProcessor::boot() {
  settings_ = RadioSettings::unset();
  settings_loaded_ = settings_store_.load(&settings_);
  if (!settings_loaded_) {
    // First boot: keep safe default, don't persist yet (avoid wear until
    // an app tunes us).
    settings_ = RadioSettings::unset();
  }
  return radio_.apply(settings_);
}

bool CommandProcessor::first_owner_lock_active() const {
  // 05 §3: during the grace window after boot, only the first SET_RADIO is
  // honored — the first-connected client owns the tuning decision.
  return set_count_since_boot_ >= 1 && clock_.uptime_ms() < kFirstOwnerGraceMs;
}

void CommandProcessor::on_frame(uint8_t cmd, uint8_t nonce, uint8_t status,
                                const uint8_t* payload, size_t len,
                                IFrameSink* from) {
  if (status != 0) return;  // async frames only flow radio->host; ignore
  switch (cmd) {
    case CMD_PING:
      respond(from, cmd, nonce, STATUS_OK, payload, len);
      return;

    case CMD_GET_INFO: {
      if (len != 0) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      uint8_t p[49];
      size_t i = 0;
      p[i++] = MESHPGEON_PROTOCOL_VERSION;
      p[i++] = 0;  // fw version major (kept in fw_version_ string too)
      p[i++] = 1;  // fw version minor
      memset(&p[i], 0, 16);
      strncpy((char*)&p[i], board_name_, 15);
      i += 16;
      uint32_t uptime = clock_.uptime_ms();
      uint32_t boot_count = clock_.boot_count();
      uint32_t store_count = store_.count();
      uint32_t store_capacity = store_.capacity();
      uint32_t store_dropped = store_.dropped();
      uint32_t oldest = store_.oldest_seq();
      memcpy(&p[i], &uptime, 4); i += 4;
      memcpy(&p[i], &boot_count, 4); i += 4;
      memcpy(&p[i], &store_count, 4); i += 4;
      memcpy(&p[i], &store_capacity, 4); i += 4;
      memcpy(&p[i], &store_dropped, 4); i += 4;
      memcpy(&p[i], &oldest, 4); i += 4;
      memcpy(&p[i], &settings_.config_epoch, 4); i += 4;
      uint16_t batt = hooks_ ? hooks_->battery_mv() : 0xFFFF;
      memcpy(&p[i], &batt, 2); i += 2;
      respond(from, cmd, nonce, STATUS_OK, p, i);
      return;
    }

    case CMD_GET_RADIO: {
      if (len != 0) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      uint8_t p[RadioSettings::kSerializedSize];
      settings_.serialize(p);
      respond(from, cmd, nonce, STATUS_OK, p, sizeof(p));
      return;
    }

    case CMD_SET_RADIO: {
      RadioSettings s;
      if (len != RadioSettings::kSerializedSize || !s.deserialize(payload, len)) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      if (first_owner_lock_active()) {
        respond(from, cmd, nonce, STATUS_ERR_BUSY, NULL, 0);
        return;
      }
      if (!radio_.apply(s)) {
        respond(from, cmd, nonce, STATUS_ERR_TX_FAILED, NULL, 0);
        return;
      }
      s.version = RadioSettings::kSerializedVersion;
      s.config_epoch = settings_.config_epoch + 1;
      settings_ = s;
      settings_store_.save(s);  // persists on every SET_RADIO (04 §1.4)
      set_count_since_boot_++;
      // Multi-client: everyone else learns about the change (05 §3).
      uint8_t evp[4 + RadioSettings::kSerializedSize];  // [epoch][settings]
      uint32_t ep = s.config_epoch;
      evp[0] = (uint8_t)(ep & 0xFF);
      evp[1] = (uint8_t)((ep >> 8) & 0xFF);
      evp[2] = (uint8_t)((ep >> 16) & 0xFF);
      evp[3] = (uint8_t)((ep >> 24) & 0xFF);
      s.serialize(&evp[4]);
      uint8_t ev[3 + sizeof(evp) + 2];
      size_t el = frame_build(ev, CMD_RADIO_CHANGED, 0, 0, evp, sizeof(evp));
      broadcast(ev, el, from);
      uint8_t p[RadioSettings::kSerializedSize];
      s.serialize(p);
      respond(from, cmd, nonce, STATUS_OK, p, sizeof(p));
      return;
    }

    case CMD_SEND_PACKET: {
      if (len < 1 || len > 1 + MESHPGEON_MAX_RAW_PACKET ||
          payload[0] != len - 1) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      if (tx_pending_ != 0) {
        // One TX at a time: the radio keys up serially. Report busy; the
        // app's outbox owns retry policy (04 §1.2).
        respond(from, cmd, nonce, STATUS_ERR_BUSY, NULL, 0);
        return;
      }
      uint32_t seq = store_.append(clock_.uptime_ms(), 0, 0, kFlagsSent,
                                   payload + 1, payload[0]);
      tx_pending_ = 1;
      tx_pending_seq_ = seq;
      tx_start_ok_ = radio_.transmit(payload + 1, payload[0]) == 0;
      if (!tx_start_ok_) {
        tx_pending_ = 0;  // refused immediately: report, don't wait
      }
      respond(from, cmd, nonce, STATUS_OK, (uint8_t*)&seq, 4);
      if (!tx_start_ok_) emit_tx_result(seq, false);
      return;
    }

    case CMD_FETCH_PACKETS: {
      if (len != 6) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      uint32_t since = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                       ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
      uint32_t max_count = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8);
      uint8_t entry[3 + kStoredPacketOverhead + MESHPGEON_MAX_RAW_PACKET + 2];
      uint32_t delivered = 0;
      store_.fetch_since(since, max_count, [&](const StoredPacket& e) {
        size_t i = 0;
        entry[i++] = (uint8_t)(e.seq & 0xFF);
        entry[i++] = (uint8_t)((e.seq >> 8) & 0xFF);
        entry[i++] = (uint8_t)((e.seq >> 16) & 0xFF);
        entry[i++] = (uint8_t)((e.seq >> 24) & 0xFF);
        entry[i++] = (uint8_t)(e.uptime_ms & 0xFF);
        entry[i++] = (uint8_t)((e.uptime_ms >> 8) & 0xFF);
        entry[i++] = (uint8_t)((e.uptime_ms >> 16) & 0xFF);
        entry[i++] = (uint8_t)((e.uptime_ms >> 24) & 0xFF);
        entry[i++] = (uint8_t)e.rssi;
        entry[i++] = (uint8_t)e.snr;
        entry[i++] = e.flags;
        entry[i++] = e.len;
        memcpy(&entry[i], e.raw, e.len);
        i += e.len;
        uint8_t frame[3 + kStoredPacketOverhead + MESHPGEON_MAX_RAW_PACKET + 2];
        size_t fl = frame_build(frame, CMD_RX_PACKET, nonce, 0, entry, i);
        from->send_frame(frame, fl);
        delivered++;
        return true;
      });
      uint8_t endp[2] = {(uint8_t)(delivered & 0xFF),
                         (uint8_t)((delivered >> 8) & 0xFF)};
      respond(from, CMD_FETCH_END, nonce, STATUS_OK, endp, 2);
      return;
    }

    case CMD_PURGE_STORE: {
      if (len != 0) {
        respond(from, cmd, nonce, STATUS_ERR_BAD_PAYLOAD, NULL, 0);
        return;
      }
      store_.clear();
      respond(from, cmd, nonce, STATUS_OK, NULL, 0);
      return;
    }

    case CMD_BOOTLOADER: {
      respond(from, cmd, nonce, STATUS_OK, NULL, 0);
      if (hooks_) hooks_->reboot_to_bootloader();
      return;
    }

    default:
      respond(from, cmd, nonce, STATUS_ERR_BAD_CMD, NULL, 0);
      return;
  }
}

uint32_t CommandProcessor::on_packet_received(int8_t rssi, int8_t snr,
                                              const uint8_t* raw, uint8_t len) {
  if (len > MESHPGEON_MAX_RAW_PACKET) len = MESHPGEON_MAX_RAW_PACKET;
  uint32_t seq = store_.append(clock_.uptime_ms(), rssi, snr, kFlagsReceived,
                               raw, len);
  // Live push while connected (04 §4), same entry layout as fetch replay.
  uint8_t entry[kStoredPacketOverhead + MESHPGEON_MAX_RAW_PACKET];
  size_t i = 0;
  entry[i++] = (uint8_t)(seq & 0xFF);
  entry[i++] = (uint8_t)((seq >> 8) & 0xFF);
  entry[i++] = (uint8_t)((seq >> 16) & 0xFF);
  entry[i++] = (uint8_t)((seq >> 24) & 0xFF);
  uint32_t up = clock_.uptime_ms();
  entry[i++] = (uint8_t)(up & 0xFF);
  entry[i++] = (uint8_t)((up >> 8) & 0xFF);
  entry[i++] = (uint8_t)((up >> 16) & 0xFF);
  entry[i++] = (uint8_t)((up >> 24) & 0xFF);
  entry[i++] = (uint8_t)rssi;
  entry[i++] = (uint8_t)snr;
  entry[i++] = kFlagsReceived;
  entry[i++] = len;
  memcpy(&entry[i], raw, len);
  i += len;
  uint8_t frame[3 + kStoredPacketOverhead + MESHPGEON_MAX_RAW_PACKET + 2];
  size_t fl = frame_build(frame, CMD_RX_PACKET, 0, 0, entry, i);
  broadcast(frame, fl, NULL);
  return seq;
}

void CommandProcessor::poll() {
  if (tx_pending_ != 0 && radio_.tx_done()) {
    on_tx_result(tx_pending_seq_, true);
  }
}

void CommandProcessor::on_tx_result(uint32_t seq, bool ok) {
  if (tx_pending_ == 0 || seq != tx_pending_seq_) return;
  tx_pending_ = 0;
  emit_tx_result(seq, ok);
}

void CommandProcessor::emit_tx_result(uint32_t seq, bool ok) {
  uint8_t p[5];
  p[0] = (uint8_t)(seq & 0xFF);
  p[1] = (uint8_t)((seq >> 8) & 0xFF);
  p[2] = (uint8_t)((seq >> 16) & 0xFF);
  p[3] = (uint8_t)((seq >> 24) & 0xFF);
  p[4] = ok ? STATUS_OK : STATUS_ERR_TX_FAILED;
  uint8_t frame[3 + 5 + 2];
  size_t len = frame_build(frame, CMD_TX_RESULT, 0, 0, p, 5);
  broadcast(frame, len, NULL);
}

void CommandProcessor::respond(IFrameSink* to, uint8_t cmd, uint8_t nonce,
                               uint8_t status, const uint8_t* payload,
                               size_t len) {
  if (to == NULL) return;
  uint8_t frame[FRAME_MAX_DECODED];
  size_t n = frame_build(frame, cmd, nonce, status, payload, len);
  to->send_frame(frame, n);
}

void CommandProcessor::broadcast(const uint8_t* decoded, size_t len,
                                 IFrameSink* except) {
  for (size_t i = 0; i < num_sinks_; i++) {
    if (sinks_[i] != NULL && sinks_[i] != except) sinks_[i]->send_frame(decoded, len);
  }
}

}  // namespace meshpigeon
