#include "meshpigeon/settings.h"

#include <string.h>

#include "meshpigeon/framing.h"

namespace meshpigeon {

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
  uint16_t crc = 0;
  crc16_ccitt(&crc, out, i);
  out[i++] = (uint8_t)(crc & 0xFF);
  out[i++] = (uint8_t)(crc >> 8);
}

bool RadioSettings::deserialize(const uint8_t* in, size_t len) {
  if (len != kSerializedSize) return false;
  uint16_t crc = 0;
  crc16_ccitt(&crc, in, kSerializedSize - 2);
  uint16_t stored = (uint16_t)(in[kSerializedSize - 2] |
                               (in[kSerializedSize - 1] << 8));
  if (crc != stored) return false;
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

}  // namespace meshpigeon
