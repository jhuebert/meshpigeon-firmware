#ifndef MESHPIGEON_SETTINGS_H
#define MESHPIGEON_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

namespace meshpigeon {

/**
 * Radio settings — the only opinionated thing the firmware holds, and the
 * only thing it persists. Applied at boot so a lone radio resumes
 * listening with no app attached (04-firmware §1.4).
 *
 * Serialized layout (little-endian, 17 bytes + 2-byte CRC16 over the 15
 * leading bytes... see serialize()):
 *   [version:1][region:1][freq_hz:u32][bw_x100khz:u16][sf:1][cr:1]
 *   [power_dbm:1][config_epoch:u32][crc16:2]
 */
struct RadioSettings {
  uint8_t version;        // serialized format version, currently 1
  uint8_t region;         // region preset id (0 = UNSET / app default)
  uint32_t freq_hz;
  uint16_t bw_x100khz;    // bandwidth in 0.01 kHz units (e.g. 12500 -> 125000)
  uint8_t sf;             // spreading factor 5..12
  uint8_t cr;             // coding rate 5..8 meaning 4/5..4/8
  uint8_t power_dbm;      // TX power
  uint32_t config_epoch;  // bumped on every SET_RADIO (multi-client sync)

  static const uint8_t kSerializedVersion = 1;
  static const size_t kSerializedSize = 17;  // incl. trailing CRC16

  static RadioSettings unset();

  void serialize(uint8_t* out) const;          // out: kSerializedSize bytes
  bool deserialize(const uint8_t* in, size_t len);

  bool operator==(const RadioSettings& o) const {
    return version == o.version && region == o.region &&
           freq_hz == o.freq_hz && bw_x100khz == o.bw_x100khz &&
           sf == o.sf && cr == o.cr && power_dbm == o.power_dbm &&
           config_epoch == o.config_epoch;
  }
  bool operator!=(const RadioSettings& o) const { return !(*this == o); }
};

/**
 * Persistence abstraction: NVS on ESP32, LittleFS/flash file on nRF52,
 * plain file in host tests/simulator. Implementations must be power-loss
 * tolerant (write-then-commit); apply at boot happens through load().
 */
class SettingsStore {
 public:
  virtual ~SettingsStore() {}
  virtual bool save(const RadioSettings& s) = 0;
  virtual bool load(RadioSettings* out) = 0;  // false => first boot
};

/** In-memory store for tests and the simulator. */
class MemorySettingsStore : public SettingsStore {
 public:
  bool save(const RadioSettings& s) override {
    saved_ = s;
    have_ = true;
    return true;
  }
  bool load(RadioSettings* out) override {
    if (!have_) return false;
    *out = saved_;
    return true;
  }

 private:
  RadioSettings saved_;
  bool have_ = false;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_SETTINGS_H
