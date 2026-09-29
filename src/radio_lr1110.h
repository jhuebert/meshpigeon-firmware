#ifndef MESHPIGEON_RADIO_LR1110_H
#define MESHPIGEON_RADIO_LR1110_H

#include <SPI.h>

#include "radio_lora.h"

namespace meshpigeon {

/** LR1110-specific facts for LoraRadioBase. */
struct Lr1110Traits {
  static constexpr uint32_t kIrqTxDone = RADIOLIB_LR11X0_IRQ_TX_DONE;
  static constexpr uint32_t kIrqRxDone = RADIOLIB_LR11X0_IRQ_RX_DONE;
  static constexpr uint32_t kIrqCrcErr = RADIOLIB_LR11X0_IRQ_CRC_ERR;
  static constexpr uint32_t kIrqHeaderErr = RADIOLIB_LR11X0_IRQ_HEADER_ERR;
  static constexpr uint8_t kCrcLen = 2;
  // A corrupted LoRa header can desync the RX buffer; recover via standby.
  static constexpr bool kHeaderErrQuirk = true;
};

/**
 * LR1110 (RadioLib) implementation of ILoRaRadio — the same async-TX /
 * polled-RX model as the SX1262, sharing one state machine through
 * LoraRadioBase. Board config mirrors MeshCore's t1000-e variant: DIO3
 * TCXO, DIO5-8 RF switch table, boosted RX gain.
 */
class Lr1110Radio : public LoraRadioBase<LR1110, Lr1110Traits> {
 public:
  Lr1110Radio()
      : LoraRadioBase<LR1110, Lr1110Traits>(
            new Module(MESHPIGEON_PIN_LORA_NSS, MESHPIGEON_PIN_LORA_DIO1,
                       MESHPIGEON_PIN_LORA_RST, MESHPIGEON_PIN_LORA_BUSY)) {}

  bool begin() override {
    float tcxo = 0.0f;
#ifdef MESHPIGEON_LORA_TCXO_MV
    tcxo = MESHPIGEON_LORA_TCXO_MV / 1000.0f;
#endif
    // RadioLib validates the tuning here, so prime the silicon with the safe
    // default: CommandProcessor::boot() applies whatever was persisted (or
    // this same default) immediately afterwards, and apply() is the only
    // thing that ever changes the tuning from then on.
    const RadioSettings s = RadioSettings::unset();
    int state = radio_.begin((float)s.freq_hz / 1000000.0f,
                             (float)s.bw_x100khz / 100.0f, s.sf, s.cr,
                             RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                             (int8_t)s.power_dbm, 16, tcxo);
    if (state != RADIOLIB_ERR_NONE) return false;
#ifdef MESHPIGEON_RADIO_RF_SWITCH_TABLE
    // T1000-E switches the antenna via DIO5-8 (MeshCore rfswitch_table)
    radio_.setRfSwitchTable(k_rfswitch_dios, k_rfswitch_table);
#endif
#ifdef MESHPIGEON_RADIO_RX_BOOSTED_GAIN
    radio_.setRxBoostedGainMode(true);
#endif
    return apply(s);
  }

#ifdef MESHPIGEON_RADIO_RF_SWITCH_TABLE
  // DIO5-8 RF switch table from MeshCore's t1000-e target.cpp
  static const uint32_t k_rfswitch_dios[Module::RFSWITCH_MAX_PINS];
  static const Module::RfSwitchMode_t k_rfswitch_table[];
#endif
};

#ifdef MESHPIGEON_RADIO_RF_SWITCH_TABLE
const uint32_t Lr1110Radio::k_rfswitch_dios[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_LR11X0_DIO7,
    RADIOLIB_LR11X0_DIO8, RADIOLIB_NC,
};
const Module::RfSwitchMode_t Lr1110Radio::k_rfswitch_table[] = {
    // mode                 DIO5  DIO6  DIO7  DIO8
    {LR11x0::MODE_STBY, {LOW, LOW, LOW, LOW}},
    {LR11x0::MODE_RX, {HIGH, LOW, LOW, HIGH}},
    {LR11x0::MODE_TX, {HIGH, HIGH, LOW, HIGH}},
    {LR11x0::MODE_TX_HP, {LOW, HIGH, LOW, HIGH}},
    {LR11x0::MODE_TX_HF, {LOW, LOW, LOW, LOW}},
    {LR11x0::MODE_GNSS, {LOW, LOW, HIGH, LOW}},
    {LR11x0::MODE_WIFI, {LOW, LOW, LOW, LOW}},
    END_OF_MODE_TABLE,
};
#endif

}  // namespace meshpigeon

#endif  // MESHPIGEON_RADIO_LR1110_H
