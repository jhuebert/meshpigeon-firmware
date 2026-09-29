#ifndef MESHPIGEON_RADIO_SX1262_H
#define MESHPIGEON_RADIO_SX1262_H

#include <SPI.h>

#include "radio_lora.h"

namespace meshpigeon {

/** SX1262-specific facts for LoraRadioBase. */
struct Sx126xTraits {
  static constexpr uint32_t kIrqTxDone = RADIOLIB_SX126X_IRQ_TX_DONE;
  static constexpr uint32_t kIrqRxDone = RADIOLIB_SX126X_IRQ_RX_DONE;
  static constexpr uint32_t kIrqCrcErr = RADIOLIB_SX126X_IRQ_CRC_ERR;
  static constexpr uint32_t kIrqHeaderErr = RADIOLIB_SX126X_IRQ_HEADER_ERR;
  static constexpr uint8_t kCrcLen = 1;
  static constexpr bool kHeaderErrQuirk = false;
};

/**
 * SX1262 (RadioLib) implementation of ILoRaRadio. Bring-up and RF-switch
 * wiring are board work and live here; the tuning/TX/RX state machine is
 * shared with the LR1110 through LoraRadioBase. The firmware reads/writes
 * raw bytes only — no protocol.
 */
class Sx1262Radio : public LoraRadioBase<SX1262, Sx126xTraits> {
 public:
  Sx1262Radio()
      : LoraRadioBase<SX1262, Sx126xTraits>(
            new Module(MESHPIGEON_PIN_LORA_NSS, MESHPIGEON_PIN_LORA_DIO1,
                       MESHPIGEON_PIN_LORA_RST, MESHPIGEON_PIN_LORA_BUSY)) {}

  bool begin() override {
    float tcxo = 0.0f;
#ifdef MESHPIGEON_LORA_TCXO_MV
    tcxo = MESHPIGEON_LORA_TCXO_MV / 1000.0f;
#endif
#ifdef MESHPIGEON_PIN_RADIO_POWER_EN
    // Boards that gate the radio's supply (T114: SX126X_POWER_EN)
    pinMode(MESHPIGEON_PIN_RADIO_POWER_EN, OUTPUT);
    digitalWrite(MESHPIGEON_PIN_RADIO_POWER_EN, HIGH);
    delay(10);
#endif
    // RadioLib validates the tuning here; the safe default stands until
    // CommandProcessor::boot() applies whatever was persisted.
    int state = radio_.begin((float)last_settings_.freq_hz / 1000000.0f,
                             (float)last_settings_.bw_x100khz / 100.0f,
                             last_settings_.sf, last_settings_.cr,
                             RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                             (int8_t)last_settings_.power_dbm, 16, tcxo,
                             tcxo > 0.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
    // Headroom for +22 dBm TX (MeshCore SX126X_CURRENT_LIMIT=140)
#ifdef MESHPIGEON_PIN_RADIO_RXEN
    // Boards with a dedicated RX-enable line (XIAO WIO) use a manual RF-switch
    // pin, not SX1262 DIO2 (MeshCore SX126X_RXEN)
    radio_.setRfSwitchPins(MESHPIGEON_PIN_RADIO_RXEN, RADIOLIB_NC);
#endif
    state = radio_.setCurrentLimit(140.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
#ifdef MESHPIGEON_RADIO_RX_BOOSTED_GAIN
    radio_.setRxBoostedGainMode(true);
#endif
#ifdef MESHPIGEON_RADIO_DIO2_RFSWITCH
    // Some boards (XIAO WIO, Heltec V3) switch the antenna via DIO2
    radio_.setDio2AsRfSwitch(true);
#endif
    return apply(last_settings_);
  }
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_RADIO_SX1262_H
