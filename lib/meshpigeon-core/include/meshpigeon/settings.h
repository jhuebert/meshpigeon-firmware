#ifndef MESHPIGEON_SETTINGS_H
#define MESHPIGEON_SETTINGS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"

namespace meshpigeon {

/**
 * Radio settings — the only opinionated thing the firmware holds, and the
 * only thing it persists. Applied at boot so a lone radio resumes
 * listening with no app attached (docs/radio-protocol.md §6).
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
 * Device settings (docs/radio-protocol.md §8.1): the advertised name, the PIN that gates use
 * of the node, and the Wi-Fi station. Everything here is device-level —
 * no mesh concept lives in it.
 *
 * The PIN is always set: it ships as the public default "0000" (like every
 * Bluetooth device's default), and a user may replace it with 4..8 ASCII
 * digits. There is no "no PIN" state. The value is write-only on the wire —
 * DeviceSettingsMessage responses never carry it.
 */
struct DeviceSettings {
  char name[MESHPIGEON_NAME_MAX + 1];  // empty = derived default name
  char pin[MESHPIGEON_PIN_MAX + 1];
  bool wifi_enabled;
  char wifi_ssid[MESHPIGEON_SSID_MAX + 1];
  char wifi_password[MESHPIGEON_PASS_MAX + 1];
  uint16_t wifi_port;

  static const char* kDefaultPin() { return "0000"; }

  static DeviceSettings defaults() {
    DeviceSettings d;
    memset(&d, 0, sizeof(d));
    strncpy(d.pin, kDefaultPin(), sizeof(d.pin) - 1);
    d.wifi_port = MESHPIGEON_WIFI_PORT_DEFAULT;
    return d;
  }

  /** The PIN is "0000" — i.e. still the public factory default. */
  bool pin_is_default() const { return strcmp(pin, kDefaultPin()) == 0; }

  void clear() { *this = defaults(); }

  bool operator==(const DeviceSettings& o) const {
    return strcmp(name, o.name) == 0 && strcmp(pin, o.pin) == 0 &&
           wifi_enabled == o.wifi_enabled &&
           strcmp(wifi_ssid, o.wifi_ssid) == 0 &&
           strcmp(wifi_password, o.wifi_password) == 0 &&
           wifi_port == o.wifi_port;
  }
  bool operator!=(const DeviceSettings& o) const { return !(*this == o); }

  /** One field of a SetDeviceSettings request, validated against the rules
   *  in docs/radio-protocol.md §8.1. Rejects the whole request atomically on any failure. */
  static bool valid_name(const char* s, size_t len) {
    if (s == NULL || len > MESHPIGEON_NAME_MAX) return false;
    // Printable only. The name goes straight into the BLE advertising
    // payload, a length-constrained UTF-8 field, so control bytes would put
    // bytes in it that no scanner should have to render. Bytes >= 0x80 are
    // left alone, so a non-ASCII name still works.
    for (size_t i = 0; i < len; i++) {
      const unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
  }
  /** A PIN *value* is 4..8 ASCII digits. */
  static bool valid_pin(const char* s, size_t len) {
    if (s == NULL || len < MESHPIGEON_PIN_MIN || len > MESHPIGEON_PIN_MAX) {
      return false;
    }
    for (size_t i = 0; i < len; i++) {
      if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
  }
  /** What a SetDeviceSettings `pin` field may carry: a PIN, or the empty
   *  string, which is the documented "restore the factory default"
   *  (absent = unchanged). There is no "no PIN" state. */
  static bool valid_pin_set(const char* s, size_t len) {
    return s != NULL && (len == 0 || valid_pin(s, len));
  }
  /** Apply a validated SetDeviceSettings `pin` value. */
  static void set_pin(char* dst, const char* s) {
    size_t len = strlen(s);
    if (len == 0) {
      // "restore the factory default" — there is no "no PIN" state
      strncpy(dst, kDefaultPin(), MESHPIGEON_PIN_MAX);
    } else {
      memcpy(dst, s, len);  // bounded by valid_pin_set above
    }
    dst[MESHPIGEON_PIN_MAX] = 0;
  }
  static bool valid_ssid(const char* s, size_t len) {
    return s != NULL && len <= MESHPIGEON_SSID_MAX;
  }
  static bool valid_password(const char* s, size_t len) {
    return s != NULL && len <= MESHPIGEON_PASS_MAX;
  }
  static bool valid_port(uint32_t port) { return port != 0 && port <= 0xFFFF; }
};

/**
 * Persistence abstraction: NVS on ESP32, LittleFS/flash file on nRF52,
 * plain file in host tests/simulator. Implementations must be power-loss
 * tolerant (write-then-commit); apply at boot happens through load().
 * Absent records fall back to defaults on read, so firmware upgraded onto
 * an older install boots cleanly.
 */
class SettingsStore {
 public:
  virtual ~SettingsStore() {}
  virtual bool save(const RadioSettings& s) = 0;
  virtual bool load(RadioSettings* out) = 0;  // false => first boot

  /** Device settings (name, PIN, Wi-Fi). save() persists immediately;
   *  load() yields defaults when nothing is stored yet. */
  virtual bool save_device(const DeviceSettings& s) = 0;
  virtual bool load_device(DeviceSettings* out) = 0;  // false => defaults
  /** Forget the stored device settings entirely (FACTORY_RESET) — the
   *  next load_device() reports defaults. Removing the record beats writing
   *  defaults over it: no wear, and no stale keys left behind. */
  virtual void clear_device() = 0;
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
  bool save_device(const DeviceSettings& s) override {
    saved_device_ = s;
    have_device_ = true;
    return true;
  }
  bool load_device(DeviceSettings* out) override {
    if (!have_device_) {
      *out = DeviceSettings::defaults();
      return false;
    }
    *out = saved_device_;
    return true;
  }
  void clear_device() override { have_device_ = false; }

 private:
  RadioSettings saved_;
  bool have_ = false;
  DeviceSettings saved_device_ = DeviceSettings::defaults();
  bool have_device_ = false;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_SETTINGS_H
