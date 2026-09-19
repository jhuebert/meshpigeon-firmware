#ifndef MESHPGEON_RADIO_LR1110_H
#define MESHPGEON_RADIO_LR1110_H

#include <RadioLib.h>
#include <SPI.h>

#include "meshpigeon/command_processor.h"

namespace meshpigeon {

/**
 * LR1110 (RadioLib) implementation of ILoRaRadio — same async-TX / polled-RX
 * model as Sx1262Radio. Board config mirrors MeshCore's t1000-e variant:
 * DIO3 TCXO, DIO5-8 RF switch table, boosted RX gain.
 */
class Lr1110Radio : public ILoRaRadio {
 public:
  Lr1110Radio() : radio_(new Module(MESHPGEON_PIN_LORA_NSS, MESHPGEON_PIN_LORA_DIO1,
                                    MESHPGEON_PIN_LORA_RST,
                                    MESHPGEON_PIN_LORA_BUSY)) {}

  bool begin() {
    float tcxo = 0.0f;
#ifdef MESHPGEON_LORA_TCXO_MV
    tcxo = MESHPGEON_LORA_TCXO_MV / 1000.0f;
#endif
    // RadioLib validates these at begin(); the CommandProcessor applies the
    // persisted settings right after (last_settings_ starts at the unset
    // region preset).
    int state = radio_.begin((float)last_settings_.freq_hz / 1000000.0f,
                             (float)last_settings_.bw_x100khz / 100.0f,
                             last_settings_.sf, last_settings_.cr,
                             RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE,
                             (int8_t)last_settings_.power_dbm, 16, tcxo);
    if (state != RADIOLIB_ERR_NONE) return false;
#ifdef MESHPGEON_RADIO_RF_SWITCH_TABLE
    // T1000-E switches the antenna via DIO5-8 (MeshCore rfswitch_table)
    radio_.setRfSwitchTable(k_rfswitch_dios, k_rfswitch_table);
#endif
#ifdef MESHPGEON_RADIO_RX_BOOSTED_GAIN
    radio_.setRxBoostedGainMode(true);
#endif
    return apply(last_settings_);
  }

  bool apply(const RadioSettings& s) override {
    last_settings_ = s;
    int state = radio_.standby();
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setFrequency((float)s.freq_hz / 1000000.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setBandwidth((float)s.bw_x100khz / 100.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setSpreadingFactor(s.sf);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setCodingRate(s.cr);
    if (state != RADIOLIB_ERR_NONE) return false;
    // Longer preamble for lower SF (MeshCore preambleLengthForSF)
    state = radio_.setPreambleLength(s.sf <= 8 ? 32 : 16);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setOutputPower((int8_t)s.power_dbm);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setCRC(2);  // on-air CRC always on
    if (state != RADIOLIB_ERR_NONE) return false;
    return start_rx();
  }

  int transmit(const uint8_t* raw, uint8_t len) override {
    if (tx_started_) return -1;  // already keying up
    // stop RX so we can transmit
    radio_.standby();
    int state = radio_.startTransmit(const_cast<uint8_t*>(raw), len);
    if (state != RADIOLIB_ERR_NONE) {
      start_rx();
      return -1;
    }
    tx_started_ = true;
    return 0;
  }

  bool tx_done() override {
    if (!tx_started_) return true;
    if (radio_.getIrqFlags() & RADIOLIB_LR11X0_IRQ_TX_DONE) {
      radio_.finishTransmit();  // clears IRQ + returns to standby
      tx_started_ = false;
      start_rx();
      return true;
    }
    return false;
  }

  bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi, int8_t* snr) override {
    if (rx_paused_) return false;
    uint32_t irq = radio_.getIrqFlags();
    if (!(irq & RADIOLIB_LR11X0_IRQ_RX_DONE)) return false;
    // Known LR11x0 quirk (MeshCore CustomLR1110): a corrupted header can
    // desync the RX buffer — return to standby before restarting RX.
    if ((irq & RADIOLIB_LR11X0_IRQ_HEADER_ERR) &&
        radio_.getPacketLength(true) == 0) {
      radio_.standby();
      start_rx();
      return false;
    }
    bool crc_ok = !(irq & (RADIOLIB_LR11X0_IRQ_CRC_ERR |
                           RADIOLIB_LR11X0_IRQ_HEADER_ERR));
    uint8_t plen = radio_.getPacketLength(true);
    uint8_t buf[MESHPGEON_MAX_RAW_PACKET];
    bool got = crc_ok && plen > 0 && plen <= MESHPGEON_MAX_RAW_PACKET &&
               radio_.readData(buf, plen) == RADIOLIB_ERR_NONE;  // clears IRQ
    start_rx();
    if (!got) return false;
    *len = plen;
    memcpy(raw, buf, *len);
    *rssi = (int8_t)radio_.getRSSI();
    *snr = (int8_t)radio_.getSNR();
    return true;
  }

 private:
  bool start_rx() {
    int state = radio_.startReceive();
    return state == RADIOLIB_ERR_NONE;
  }

#ifdef MESHPGEON_RADIO_RF_SWITCH_TABLE
  static const uint32_t k_rfswitch_dios[Module::RFSWITCH_MAX_PINS];
  static const Module::RfSwitchMode_t k_rfswitch_table[];
#endif

  LR1110 radio_;
  RadioSettings last_settings_ = RadioSettings::unset();
  bool tx_started_ = false;
  bool rx_paused_ = false;
};

#ifdef MESHPGEON_RADIO_RF_SWITCH_TABLE
// DIO5-8 RF switch table from MeshCore's t1000-e target.cpp
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

#endif  // MESHPGEON_RADIO_LR1110_H
