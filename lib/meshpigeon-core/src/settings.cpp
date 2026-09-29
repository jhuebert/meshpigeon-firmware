#include "meshpigeon/settings.h"

#include <string.h>

#include "meshpigeon/framing.h"

namespace meshpigeon {

// The trailing CRC both serialized records carry: CCITT-FALSE over the
// bytes before it, little-endian on the medium. One place, because the
// little-endian detail is exactly what silently breaks a record.
static size_t append_crc(uint8_t* out, size_t body_len) {
  uint16_t crc = 0;
  crc16_ccitt(&crc, out, body_len);
  out[body_len] = (uint8_t)(crc & 0xFF);
  out[body_len + 1] = (uint8_t)(crc >> 8);
  return body_len + 2;
}

/** Read back what append_crc() wrote. False when the record is short or the
 *  CRC does not match — which is what a half-completed write looks like. */
static bool check_crc(const uint8_t* in, size_t len) {
  if (len < 2) return false;
  uint16_t crc = 0;
  crc16_ccitt(&crc, in, len - 2);
  return crc == (uint16_t)(in[len - 2] | (in[len - 1] << 8));
}

RadioSettings RadioSettings::unset() {
  RadioSettings s;
  memset(&s, 0, sizeof(s));
  s.version = kSerializedVersion;
  s.region = 0;  // UNSET: radio listens on a safe default until an app tunes it
  s.freq_hz = 869525000;   // default-region fallback
  s.bw_x100khz = 12500;    // 125.00 kHz
  s.sf = 9;
  s.cr = 5;                // 4/5
  s.power_dbm = 14;
  s.config_epoch = 0;
  return s;
}

void RadioSettings::serialize(uint8_t* out) const {
  size_t i = 0;
  out[i++] = version;
  out[i++] = region;
  out[i++] = (uint8_t)(freq_hz & 0xFF);
  out[i++] = (uint8_t)((freq_hz >> 8) & 0xFF);
  out[i++] = (uint8_t)((freq_hz >> 16) & 0xFF);
  out[i++] = (uint8_t)((freq_hz >> 24) & 0xFF);
  out[i++] = (uint8_t)(bw_x100khz & 0xFF);
  out[i++] = (uint8_t)((bw_x100khz >> 8) & 0xFF);
  out[i++] = sf;
  out[i++] = cr;
  out[i++] = power_dbm;
  out[i++] = (uint8_t)(config_epoch & 0xFF);
  out[i++] = (uint8_t)((config_epoch >> 8) & 0xFF);
  out[i++] = (uint8_t)((config_epoch >> 16) & 0xFF);
  out[i++] = (uint8_t)((config_epoch >> 24) & 0xFF);
  append_crc(out, i);
}

bool RadioSettings::deserialize(const uint8_t* in, size_t len) {
  // Defaults FIRST, so a rejected record still leaves a valid object behind:
  // a caller that ignores the return value then has the safe tuning rather
  // than whatever the stack held. Through an ASSIGNMENT, not a bare
  // `defaults();` statement — a discarded return value is dead code the
  // optimizer is free to delete, and it did.
  *this = unset();
  if (len != kSerializedSize) return false;
  if (!check_crc(in, len)) return false;
  size_t i = 0;
  version = in[i++];
  if (version != kSerializedVersion) return false;
  region = in[i++];
  freq_hz = (uint32_t)in[i] | ((uint32_t)in[i + 1] << 8) |
            ((uint32_t)in[i + 2] << 16) | ((uint32_t)in[i + 3] << 24);
  i += 4;
  bw_x100khz = (uint16_t)(in[i] | (in[i + 1] << 8));
  i += 2;
  sf = in[i++];
  cr = in[i++];
  power_dbm = in[i++];
  config_epoch = (uint32_t)in[i] | ((uint32_t)in[i + 1] << 8) |
                 ((uint32_t)in[i + 2] << 16) | ((uint32_t)in[i + 3] << 24);
  return true;
}

void DeviceSettings::serialize(uint8_t* out) const {
  size_t i = 0;
  out[i++] = kSerializedVersion;
  // Fixed-width fields, copied whole: the layout is exact on any host, so a
  // stored name cannot run into the PIN the way a bounded copy could.
  memcpy(out + i, name, MESHPIGEON_NAME_MAX + 1);
  i += MESHPIGEON_NAME_MAX + 1;
  memcpy(out + i, pin, MESHPIGEON_PIN_MAX + 1);
  i += MESHPIGEON_PIN_MAX + 1;
  out[i++] = wifi_enabled ? 1 : 0;
  memcpy(out + i, wifi_ssid, MESHPIGEON_SSID_MAX + 1);
  i += MESHPIGEON_SSID_MAX + 1;
  memcpy(out + i, wifi_password, MESHPIGEON_PASS_MAX + 1);
  i += MESHPIGEON_PASS_MAX + 1;
  out[i++] = (uint8_t)(wifi_port & 0xFF);
  out[i++] = (uint8_t)(wifi_port >> 8);
  append_crc(out, i);
}

bool DeviceSettings::deserialize(const uint8_t* in, size_t len) {
  // Defaults FIRST, so a rejected record still leaves a valid object behind
  // (the SettingsStore contract is "false => defaults", and a caller that
  // ignores the return value must not be left holding a stack full of
  // garbage with an unterminated PIN in it).
  *this = defaults();
  if (len != kSerializedSize) return false;
  if (!check_crc(in, len)) return false;
  if (in[0] != kSerializedVersion) return false;  // a different layout
  size_t i = 1;
  memcpy(name, in + i, MESHPIGEON_NAME_MAX + 1);
  i += MESHPIGEON_NAME_MAX + 1;
  memcpy(pin, in + i, MESHPIGEON_PIN_MAX + 1);
  i += MESHPIGEON_PIN_MAX + 1;
  wifi_enabled = in[i++] != 0;
  memcpy(wifi_ssid, in + i, MESHPIGEON_SSID_MAX + 1);
  i += MESHPIGEON_SSID_MAX + 1;
  memcpy(wifi_password, in + i, MESHPIGEON_PASS_MAX + 1);
  i += MESHPIGEON_PASS_MAX + 1;
  wifi_port = (uint16_t)(in[i] | (in[i + 1] << 8));
  // A record is only ever written from a validated DeviceSettings, so the
  // strings arrive NUL-terminated inside their fields. The terminators are
  // re-imposed anyway: the one field a CRC-clean but hand-edited record
  // could leave open is the PIN, and an unterminated one would make
  // pin_is_default() read past the end of the field.
  name[MESHPIGEON_NAME_MAX] = 0;
  pin[MESHPIGEON_PIN_MAX] = 0;
  wifi_ssid[MESHPIGEON_SSID_MAX] = 0;
  wifi_password[MESHPIGEON_PASS_MAX] = 0;
  return true;
}

}  // namespace meshpigeon
