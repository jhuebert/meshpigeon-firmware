/**
 * MeshPigeon Radio Firmware — board main.
 *
 * Boot: load persisted settings, apply to the radio, start listening.
 * Loop: poll the radio for packets (store + live push), drain transports
 * (USB CDC, BLE, Wi-Fi TCP), complete pending TX. No protocol, no keys —
 * the app decides everything (docs/radio-protocol.md §12).
 */
#if defined(MESHPIGEON_ESP32) || defined(MESHPIGEON_NRF52)

#include <Arduino.h>

#include "meshpigeon/command_processor.h"
#include "meshpigeon/packet_store.h"
#include "meshpigeon/settings.h"
#include "meshpigeon/uptime_clock.h"
#if defined(MESHPIGEON_RADIO_LR1110)
#include "radio_lr1110.h"
#else
#include "radio_sx1262.h"
#endif
#include "transports.h"
#if defined(MESHPIGEON_HAS_WIFI)
#include "wifi_transport.h"
#endif

#if defined(MESHPIGEON_ESP32)
#include <esp_system.h>
#endif

using namespace meshpigeon;

static const char* const kBoardName =
#if defined(MESHPIGEON_BOARD_XIAO_WIO)
    "XIAO WIO";
#elif defined(MESHPIGEON_BOARD_HELTEC_V3)
    "HELTEC V3";
#elif defined(MESHPIGEON_BOARD_T114)
    "T114";
#elif defined(MESHPIGEON_BOARD_T1000E)
    "T1000-E";
#else
    "UNKNOWN";
#endif

static const char* const kFwVersion = "0.1.0";

#if defined(MESHPIGEON_STORE_BYTES)
static const uint32_t kStoreBytes = MESHPIGEON_STORE_BYTES;
#else
static const uint32_t kStoreBytes = 65536;
#endif

// ---- board hooks -------------------------------------------------------------

// Defined below; the hooks reach the board's own facilities through these.
class BoardSettingsStore;
static BoardSettingsStore* g_settings_store = NULL;
static BleSink* g_ble = NULL;
// How long reboot_now() lets the transports push what they have queued
// before the board actually restarts.
static const uint32_t kRebootFlushMs = 100;
#if defined(MESHPIGEON_HAS_WIFI)
static WifiTransport* g_wifi = NULL;
#endif

// Reboot, bootloader and factory reset all end the same way; the difference
// is what was persisted before we got here.
static void reboot_now() {
  // The `Ok` that answers Reboot / FactoryReset / Bootloader is still in
  // flight when we get here — the core sends it and *then* asks for the
  // restart (docs/radio-protocol.md §3), and on BLE a frame is queued rather
  // than handed to the link. Give the transports a bounded chance to push
  // it, or the client that asked for the reboot never learns it was
  // accepted. Bounded, because a central that has stopped reading must cost
  // this delay and nothing more: the reset is coming either way.
  if (g_ble) g_ble->flush(kRebootFlushMs);
  delay(10);
#if defined(ARDUINO_ARCH_ESP32)
  ESP.restart();
#else
  NVIC_SystemReset();
#endif
}

class BoardHooks : public IBoardHooks {
 public:
  uint16_t battery_mv() override {
#if defined(MESHPIGEON_BOARD_T1000E)
    // MeshCore T1000eBoard::getBattMilliVolts: sense rail on for the read,
    // 3.0 V internal reference at 12 bits, ADC_MULTIPLIER 2.0.
    digitalWrite(MESHPIGEON_PIN_3V3_EN, HIGH);
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    delay(10);
    float volts = (analogRead(MESHPIGEON_PIN_VBAT_ADC) * MESHPIGEON_ADC_MULTIPLIER *
                   3.0f) / 4096.0f;
    digitalWrite(MESHPIGEON_PIN_3V3_EN, LOW);
    analogReference(AR_DEFAULT);  // put back to default
    analogReadResolution(10);
    return (uint16_t)(volts * 1000);
#elif defined(MESHPIGEON_PIN_VBAT_ADC) && defined(MESHPIGEON_VBAT_DIVIDER)
    analogReadResolution(10);
    uint32_t raw = analogRead(MESHPIGEON_PIN_VBAT_ADC);
    float mv = (raw / 1023.0f) * 3600.0f * MESHPIGEON_VBAT_DIVIDER;
    return (uint16_t)mv;
#else
    return 0xFFFF;  // unknown on boards without a wired divider
#endif
  }

  // NOT a ROM/DFU entry yet: this is a plain restart, the same as reboot().
  // Real in-app flashing (ESP32 download mode, nRF52 DFU via the UICR
  // bootloader address) is board work still to do, so the protocol doc says
  // BOOTLOADER restarts rather than claiming a DFU it does not perform. The
  // operation stays ungated either way: flashing must work on a locked node.
  void reboot_to_bootloader() override { reboot_now(); }

  void reboot() override { reboot_now(); }

  void factory_reset() override { reboot_now(); }

  void mac_suffix(char out[5]) override {
    // The same two bytes the BLE name uses, so the derived device name and
    // the advertised name agree (docs/radio-protocol.md §8.3).
    char hex[5] = {0};
#if defined(ARDUINO_ARCH_ESP32)
    const NimBLEAddress addr = NimBLEDevice::getAddress();
    const uint8_t* a = addr.getNative();  // little-endian: a[0] is the LSB
    snprintf(hex, sizeof(hex), "%02X%02X", a[1], a[0]);
#else
    ble_gap_addr_t addr;
    if (sd_ble_gap_addr_get(&addr) == NRF_SUCCESS) {
      snprintf(hex, sizeof(hex), "%02X%02X", addr.addr[1], addr.addr[0]);
    }
#endif
    strncpy(out, hex, 4);
    out[4] = 0;
  }

  void set_device_name(const char* name) override {
    if (g_ble) g_ble->set_name(name);
  }

  uint8_t ble_clients() override {
    return g_ble ? g_ble->ble_clients() : 0;
  }

  uint8_t usb_cdc_clients() override {
    // The console is one implicit client, and no Arduino core exposes a
    // portable "a host has attached" signal (ESP32's HWCDC and the nRF52
    // BSP's USB CDC both lack one). So this reports the transport being
    // present, which is always true here — the CDC sink is unconditionally
    // built. Documented as such in docs/radio-protocol.md §9.
    return 1;
  }

  uint8_t wifi_tcp_clients() override {
#if defined(MESHPIGEON_HAS_WIFI)
    return g_wifi ? (uint8_t)g_wifi->client_count() : 0;
#else
    return 0;
#endif
  }

  void fill_status(StatusMessage* status) override {
#if defined(MESHPIGEON_HAS_WIFI)
    if (g_wifi) {
      g_wifi->fill_status(status);
      return;
    }
#endif
    status->wifi_state = meshpigeon_Status_WifiState_WIFI_STATE_OFF;
  }

  void apply_wifi(const DeviceSettings& settings) override {
#if defined(MESHPIGEON_HAS_WIFI)
    if (g_wifi) g_wifi->apply(settings);
#else
    (void)settings;
#endif
  }

  bool wifi_supported() const override {
#if defined(MESHPIGEON_HAS_WIFI)
    return true;
#else
    return false;
#endif
  }

  pb_size_t fill_capabilities(meshpigeon_Capability* out,
                              pb_size_t max) override {
    // Compile-time facts, so they are just the #ifdefs — no probing, and
    // nothing that can fail at runtime (docs/radio-protocol.md §5).
    pb_size_t n = 0;
    auto add = [&](Capability c) {
      // `max` is the contract: a value that does not fit is neither written
      // nor counted. Counting it anyway is how a board ends up advertising
      // CAPABILITY_UNSPECIFIED for capabilities it actually has.
      if (n >= max) return;
      out[n++] = static_cast<meshpigeon_Capability>(c);
    };
    if (wifi_supported()) add(kCapWifiSta);
#if defined(MESHPIGEON_PIN_VBAT_ADC) || defined(MESHPIGEON_BOARD_T1000E)
    add(kCapBattery);
#endif
    add(kCapBle);
    add(kCapUsbCdc);  // the CDC console is built unconditionally (main.cpp)
    return n;
  }
};

// ---- persisted settings + boot count -----------------------------------------

#ifdef ARDUINO_ARCH_ESP32
#include <Preferences.h>

class BoardSettingsStore : public SettingsStore {
 public:
  BoardSettingsStore() { nvs_.begin("meshpigeon", false); }

  bool save(const RadioSettings& s) override {
    uint8_t buf[RadioSettings::kSerializedSize];
    s.serialize(buf);
    nvs_.putBytes("radio", buf, sizeof(buf));
    return true;
  }
  bool load(RadioSettings* out) override {
    size_t n = nvs_.getBytesLength("radio");
    if (n != RadioSettings::kSerializedSize) return false;
    uint8_t buf[RadioSettings::kSerializedSize];
    nvs_.getBytes("radio", buf, sizeof(buf));
    return out->deserialize(buf, sizeof(buf));
  }
  uint32_t load_boot_count() { return nvs_.getULong("boots", 0); }
  void save_boot_count(uint32_t n) { nvs_.putULong("boots", n); }

  // Device settings: one NVS key per field, so a firmware that grows a
  // field never has to migrate a blob.
  bool save_device(const DeviceSettings& s) override {
    nvs_.putString("name", s.name);
    nvs_.putString("pin", s.pin);
    nvs_.putUChar("wifie", s.wifi_enabled ? 1 : 0);
    nvs_.putString("wifissid", s.wifi_ssid);
    nvs_.putString("wifipass", s.wifi_password);
    nvs_.putUShort("wifiport", s.wifi_port);
    // The commit marker, written LAST and never in the middle. NVS commits
    // each key separately, so a power cut between two of them leaves a
    // partial record — and a partial record is indistinguishable from a
    // complete one unless something says "I finished". The field that goes
    // missing is the Wi-Fi configuration, which is exactly what strands a
    // pigeon that came back after the power went (docs/radio-protocol.md §11).
    nvs_.putUChar("dev", kDeviceVersion);
    return true;
  }
  bool load_device(DeviceSettings* out) override {
    out->clear();
    if (nvs_.getUChar("dev", 0) != kDeviceVersion) return false;  // never written
    strncpy(out->name, nvs_.getString("name", "").c_str(),
            MESHPIGEON_NAME_MAX);
    out->name[MESHPIGEON_NAME_MAX] = 0;
    strncpy(out->pin, nvs_.getString("pin", "0000").c_str(), MESHPIGEON_PIN_MAX);
    out->pin[MESHPIGEON_PIN_MAX] = 0;
    out->wifi_enabled = nvs_.getUChar("wifie", 0) != 0;
    strncpy(out->wifi_ssid, nvs_.getString("wifissid", "").c_str(),
            MESHPIGEON_SSID_MAX);
    out->wifi_ssid[MESHPIGEON_SSID_MAX] = 0;
    strncpy(out->wifi_password, nvs_.getString("wifipass", "").c_str(),
            MESHPIGEON_PASS_MAX);
    out->wifi_password[MESHPIGEON_PASS_MAX] = 0;
    out->wifi_port = nvs_.getUShort("wifiport", MESHPIGEON_WIFI_PORT_DEFAULT);
    return true;
  }
  void clear_device() override {
    nvs_.remove("name");
    nvs_.remove("pin");
    nvs_.remove("wifie");
    nvs_.remove("wifissid");
    nvs_.remove("wifipass");
    nvs_.remove("wifiport");
    nvs_.remove("dev");
  }

 private:
  // Bumped when the key set below changes shape, so a record written by an
  // older firmware reads as "never written" instead of as garbage.
  static const uint8_t kDeviceVersion = 1;

  Preferences nvs_;
};

#else  // nRF52: internal file system
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

class BoardSettingsStore : public SettingsStore {
 public:
  bool save(const RadioSettings& s) override {
    uint8_t buf[RadioSettings::kSerializedSize];
    s.serialize(buf);
    InternalFS.begin();
    // FILE_O_WRITE does not truncate (appends) on this BSP — remove first,
    // or load() would keep reading the first record (MeshCore IdentityStore
    // uses the same remove-then-write pattern on nRF52).
    InternalFS.remove("/radio.bin");
    Adafruit_LittleFS_Namespace::File f("radio.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_WRITE, InternalFS);
    if (!f) return false;
    f.write(buf, sizeof(buf));
    f.close();
    return true;
  }
  bool load(RadioSettings* out) override {
    InternalFS.begin();
    if (!InternalFS.exists("/radio.bin")) return false;
    Adafruit_LittleFS_Namespace::File f("radio.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_READ, InternalFS);
    if (!f) return false;
    uint8_t buf[RadioSettings::kSerializedSize];
    int n = f.read(buf, sizeof(buf));
    f.close();
    if (n != (int)RadioSettings::kSerializedSize) return false;
    return out->deserialize(buf, sizeof(buf));
  }
  uint32_t load_boot_count() {
    InternalFS.begin();
    if (!InternalFS.exists("/boots.bin")) return 0;
    Adafruit_LittleFS_Namespace::File f("boots.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_READ, InternalFS);
    if (!f) return 0;
    uint8_t buf[4] = {0, 0, 0, 0};
    int n = f.read(buf, 4);
    f.close();
    if (n != 4) return 0;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  }
  void save_boot_count(uint32_t n) {
    InternalFS.begin();
    InternalFS.remove("/boots.bin");  // FILE_O_WRITE appends, no truncate
    Adafruit_LittleFS_Namespace::File f("boots.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_WRITE, InternalFS);
    if (!f) return;
    uint8_t buf[4] = {(uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF),
                      (uint8_t)((n >> 16) & 0xFF), (uint8_t)((n >> 24) & 0xFF)};
    f.write(buf, 4);
    f.close();
  }

  // Device settings in one fixed-layout record — the format lives in the
  // core (DeviceSettings::serialize), so it is the same bytes on every
  // board, carries its own version byte, and is covered by the host tests
  // rather than only ever being compiled for these two targets. A short
  // write is caught by the length check and the rest by the CRC: the record
  // carries the PIN and the Wi-Fi passphrase, so a bad read has to fall back
  // to defaults rather than load a half-valid credential.
  bool save_device(const DeviceSettings& s) override {
    uint8_t buf[DeviceSettings::kSerializedSize];
    s.serialize(buf);
    InternalFS.begin();
    InternalFS.remove("/dev.bin");  // FILE_O_WRITE appends, no truncate
    Adafruit_LittleFS_Namespace::File f("dev.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_WRITE, InternalFS);
    if (!f) return false;
    f.write(buf, sizeof(buf));
    f.close();
    return true;
  }
  bool load_device(DeviceSettings* out) override {
    out->clear();
    InternalFS.begin();
    if (!InternalFS.exists("/dev.bin")) return false;
    Adafruit_LittleFS_Namespace::File f("dev.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_READ, InternalFS);
    if (!f) return false;
    uint8_t buf[DeviceSettings::kSerializedSize];
    int n = f.read(buf, sizeof(buf));
    f.close();
    if (n != (int)sizeof(buf)) return false;
    return out->deserialize(buf, sizeof(buf));
  }
  void clear_device() override {
    InternalFS.begin();
    InternalFS.remove("/dev.bin");
  }
};
#endif

// ---- main ---------------------------------------------------------------------

static PacketStore* g_store;
static BoardHooks g_hooks;
#if defined(MESHPIGEON_HAS_WIFI)
// The mDNS name shares the derived device-name suffix (docs/radio-protocol.md §8.3).
static void wifi_hostname(char out[24]) {
  char suffix[5];
  g_hooks.mac_suffix(suffix);
  snprintf(out, 24, "meshpigeon-%s", suffix);
}
#endif
#if defined(MESHPIGEON_RADIO_LR1110)
static Lr1110Radio* g_radio;
#else
static Sx1262Radio* g_radio;
#endif
static CommandProcessor* g_processor;
static UsbCdcSink g_usb;

class ArduinoMillis : public IMillisecondClock {
 public:
  uint32_t millis() override { return ::millis(); }
};
static ArduinoMillis g_arduino_millis;
static UptimeClock g_uptime(g_arduino_millis);

static void radio_loop() {
  // Complete pending TX first — TX owns the air. poll() also refreshes the
  // 64-bit uptime, so the board loop never has to touch the clock itself.
  g_processor->poll();
  // Then pull anything the radio caught.
  uint8_t raw[MESHPIGEON_MAX_RAW_PACKET];
  uint8_t len;
  int8_t rssi, snr;
  if (g_radio->receive(raw, &len, &rssi, &snr)) {
    g_processor->on_packet_received(rssi, snr, raw, len);
  }
}

void setup() {
  Serial.begin(115200);

#if defined(MESHPIGEON_NRF52) && defined(MESHPIGEON_BOARD_T1000E)
  // DC/DC converter on (MeshCore T1000eBoard power profile).
  uint8_t sd_enabled = 0;
  sd_softdevice_is_enabled(&sd_enabled);
  if (sd_enabled) {
    sd_power_dcdc_mode_set(1);
  } else {
    NRF_POWER->DCDCEN = 1;
  }
#endif

  g_store = new PacketStore(kStoreBytes);
  g_settings_store = new BoardSettingsStore();
  g_uptime.set_boot_count(g_settings_store->load_boot_count() + 1);
  g_settings_store->save_boot_count(g_uptime.boot_count());

#if defined(MESHPIGEON_RADIO_LR1110)
  g_radio = new Lr1110Radio();
#else
  g_radio = new Sx1262Radio();
#endif
  g_processor = new CommandProcessor(*g_store, g_uptime, *g_settings_store,
                                     *g_radio, kBoardName, kFwVersion);
  g_processor->set_hooks(&g_hooks);
  // Boot before the transports exist: the settings have to be loaded to know
  // the effective name, so the first advertisement is already the right one
  // (docs/radio-protocol.md §8.3). A radio that failed to come up still
  // leaves a device that answers, with radio_ok = false.
  g_processor->boot();

  g_usb.begin(*g_processor);  // sets the back-pointer; add_sink() alone leaves proc_ null
  // transports.h defines the platform's BLE sink class.
  g_ble = new BleSink();
  // BLE before the name, never after: the derived default is built from the
  // BLE address, and on both families that address does not exist until the
  // stack is up (NimBLEDevice::init() on ESP32, Bluefruit.begin() on nRF52).
  // Asking first would advertise MeshPigeon-0000 — and, on nRF52, a name
  // with no suffix at all (docs/radio-protocol.md §8.3).
  char effective[MESHPIGEON_NAME_MAX + 1];
  g_ble->begin(*g_processor);
  g_processor->effective_name(effective);
  g_ble->set_name(effective);
#if defined(MESHPIGEON_HAS_WIFI)
  g_wifi = new WifiTransport();
  char hostname[24];
  wifi_hostname(hostname);
  g_wifi->begin(*g_processor, hostname);
  // A pigeon on a shelf with Wi-Fi enabled comes back up on the network
  // with no app attached (docs/radio-protocol.md §7).
  if (g_processor->device_settings().wifi_enabled) {
    g_wifi->apply(g_processor->device_settings());
  }
#endif

  // USB CDC takes a moment on some boards; don't block boot on it.
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setDebugOutput(false);
#endif
}

void loop() {
  g_usb.pump();
  g_ble->pump();
#if defined(MESHPIGEON_HAS_WIFI)
  g_wifi->pump();
#endif
  radio_loop();
  delay(1);  // pace the loop; RX FIFO + IRQ tolerate this easily
}

#else
#error "This firmware requires an ESP32 or nRF52 board environment"
#endif
