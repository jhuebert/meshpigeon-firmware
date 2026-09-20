/**
 * MeshPigeon Radio Firmware — board main.
 *
 * Boot: load persisted settings, apply to the radio, start listening.
 * Loop: poll the radio for packets (store + live push), drain transports
 * (USB CDC, BLE), complete pending TX. No protocol, no keys — the app
 * decides everything (04-firmware).
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

#if defined(MESHPIGEON_ESP32)
#include <esp_system.h>
#else
#include <Adafruit_LittleFS.h>
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
static const char* const kBleName = "MeshPigeon";

#if defined(MESHPIGEON_STORE_BYTES)
static const uint32_t kStoreBytes = MESHPIGEON_STORE_BYTES;
#else
static const uint32_t kStoreBytes = 65536;
#endif

// ---- board hooks -------------------------------------------------------------

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

  void reboot_to_bootloader() override {
    // v1: plain restart. Real ROM/DFU entry ships with in-app flashing
    // (docs/radio-protocol.md §BOOTLOADER notes the contract).
    delay(10);
#if defined(ARDUINO_ARCH_ESP32)
    ESP.restart();
#else
    NVIC_SystemReset();
#endif
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

 private:
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
    Adafruit_LittleFS_Namespace::File f("boots.bin",
                                        Adafruit_LittleFS_Namespace::FILE_O_WRITE, InternalFS);
    if (!f) return;
    uint8_t buf[4] = {(uint8_t)(n & 0xFF), (uint8_t)((n >> 8) & 0xFF),
                      (uint8_t)((n >> 16) & 0xFF), (uint8_t)((n >> 24) & 0xFF)};
    f.write(buf, 4);
    f.close();
  }
};
#endif

// ---- main ---------------------------------------------------------------------

static PacketStore* g_store;
static BoardSettingsStore* g_settings_store;
static BoardHooks g_hooks;
#if defined(MESHPIGEON_RADIO_LR1110)
static Lr1110Radio* g_radio;
#else
static Sx1262Radio* g_radio;
#endif
static CommandProcessor* g_processor;
static UsbCdcSink g_usb;

#ifdef MESHPIGEON_ESP32
static BleSink g_ble;
#endif

class ArduinoMillis : public IMillisecondClock {
 public:
  uint32_t millis() override { return ::millis(); }
};
static ArduinoMillis g_arduino_millis;
static UptimeClock g_uptime(g_arduino_millis);

static void radio_loop() {
  // Complete pending TX first — TX owns the air.
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
  g_processor->add_sink(&g_usb);
#ifdef MESHPIGEON_ESP32
  g_ble.begin(*g_processor, kBleName);
#endif

  bool applied = g_radio->begin();
  if (applied) g_processor->boot();  // applies persisted settings
  // If the radio failed to init we still answer commands (GET_INFO works),
  // we just can't hear anything — the app will see apply failures.

  // USB CDC takes a moment on some boards; don't block boot on it.
#if defined(ARDUINO_ARCH_ESP32)
  Serial.setDebugOutput(false);
#endif
}

void loop() {
  g_usb.pump();
  radio_loop();
  delay(1);  // pace the loop; RX FIFO + IRQ tolerate this easily
}

#else
#error "This firmware requires an ESP32 or nRF52 board environment"
#endif
