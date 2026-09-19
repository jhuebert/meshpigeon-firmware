#ifndef MESHHOP_RADIO_SX1262_H
#define MESHHOP_RADIO_SX1262_H

#include <RadioLib.h>
#include <SPI.h>

#include "meshhop/command_processor.h"

namespace meshhop {

/**
 * SX1262 (RadioLib) implementation of ILoRaRadio. TX starts asynchronously
 * and completes via tx_done() polling from the board loop; RX is polled.
 * The firmware reads/writes raw bytes only — no protocol.
 */
class Sx1262Radio : public ILoRaRadio {
 public:
  Sx1262Radio() : radio_(new Module(MESHHOP_PIN_LORA_NSS, MESHHOP_PIN_LORA_DIO1,
                                    MESHHOP_PIN_LORA_RST,
                                    MESHHOP_PIN_LORA_BUSY)) {}

  bool begin() {
    float tcxo = 0.0f;
#ifdef MESHHOP_LORA_TCXO_MV
    tcxo = MESHHOP_LORA_TCXO_MV / 1000.0f;
#endif
    int state = radio_.begin(0, 0, 0, 0, 0, 0, 0, tcxo, tcxo > 0.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
#ifdef MESHHOP_PIN_LORA_DIO2
    // Some boards (XIAO WIO) switch the antenna via DIO2
    radio_.setDio2AsRfSwitch(true);
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
    state = radio_.setOutputPower((int8_t)s.power_dbm);
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setCRC(true);  // on-air CRC always on
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
    if (radio_.getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE) {
      radio_.finishTransmit();  // clears IRQ + returns to standby
      tx_started_ = false;
      start_rx();
      return true;
    }
    return false;
  }

  bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi, int8_t* snr) override {
    if (rx_paused_) return false;
    uint16_t irq = radio_.getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return false;
    bool crc_ok = !(irq & (RADIOLIB_SX126X_IRQ_CRC_ERR |
                           RADIOLIB_SX126X_IRQ_HEADER_ERR));
    int plen = radio_.getPacketLength(true);
    uint8_t buf[MESHHOP_MAX_RAW_PACKET];
    bool got = crc_ok && plen > 0 && plen <= MESHHOP_MAX_RAW_PACKET &&
               radio_.readData(buf, plen) == RADIOLIB_ERR_NONE;  // clears IRQ
    start_rx();
    if (!got) return false;
    *len = (uint8_t)plen;
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

  SX1262 radio_;
  RadioSettings last_settings_ = RadioSettings::unset();
  bool tx_started_ = false;
  bool rx_paused_ = false;
};

}  // namespace meshhop

#endif  // MESHHOP_RADIO_SX1262_H
