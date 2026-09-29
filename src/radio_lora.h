#ifndef MESHPIGEON_RADIO_LORA_H
#define MESHPIGEON_RADIO_LORA_H

#include <RadioLib.h>

#include "meshpigeon/command_processor.h"

namespace meshpigeon {

/**
 * Everything the SX1262 and LR1110 ports have in common: the tuning
 * sequence, the async-transmit lifecycle and the polled receive path.
 *
 * The two radios differ only in their IRQ bit names, their on-air CRC
 * length, and one known RX quirk — so a Traits type carries exactly those
 * three facts, and there is a single copy of the TX/RX state machine to
 * keep correct. Two hand-maintained copies of it is how one board ends up
 * with a fix the other never gets.
 *
 * Derived classes own begin() only: bring-up (TCXO, current limit, RF
 * switch) is board work and differs per part.
 */
template <class Radio, class Traits>
class LoraRadioBase : public ILoRaRadio {
 public:
  bool apply(const RadioSettings& s) override {
    // Refuse rather than re-tune mid-TX. `standby()` below would abort the
    // transmission on the silicon, and nothing would ever see kIrqTxDone
    // again: the port would report "still transmitting" until the board was
    // power-cycled, and the core's one-TX-at-a-time lock would never clear.
    // The core already rejects a retune while a send is in flight; this is
    // the port refusing to corrupt its own state machine.
    if (tx_started_) return false;
    last_settings_ = s;
    int state = radio_.standby();
    if (state != RADIOLIB_ERR_NONE) return false;
    state = radio_.setFrequency((float)s.freq_hz / 1000000.0f);
    if (state != RADIOLIB_ERR_NONE) return false;
    // 0.01 kHz units internally, MHz to the driver (docs/radio-protocol.md §6).
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
    // On-air CRC always on. The length is the only per-part difference
    // (SX1262 takes 1, LR1110 2) — the CRC itself is on in both.
    state = radio_.setCRC(Traits::kCrcLen);
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
    if (radio_.getIrqFlags() & Traits::kIrqTxDone) {
      radio_.finishTransmit();  // clears IRQ + returns to standby
      tx_started_ = false;
      start_rx();
      return true;
    }
    return false;
  }

  bool receive(uint8_t* raw, uint8_t* len, int8_t* rssi, int8_t* snr) override {
    const uint32_t irq = radio_.getIrqFlags();
    if (!(irq & Traits::kIrqRxDone)) return false;
    if (Traits::kHeaderErrQuirk && (irq & Traits::kIrqHeaderErr) &&
        radio_.getPacketLength(true) == 0) {
      // Known LR11x0 quirk (MeshCore CustomLR1110): a corrupted header can
      // desync the RX buffer — return to standby before restarting RX.
      radio_.standby();
      start_rx();
      return false;
    }
    const bool crc_ok = !(irq & (Traits::kIrqCrcErr | Traits::kIrqHeaderErr));
    const int plen = (int)radio_.getPacketLength(true);
    // Short-circuit order matters: readData is only called once the length
    // is known to fit, so the caller's buffer is never overrun. A rejected
    // packet leaves raw/len untouched.
    const bool got = crc_ok && plen > 0 && plen <= MESHPIGEON_MAX_RAW_PACKET &&
                     radio_.readData(raw, plen) == RADIOLIB_ERR_NONE;
    start_rx();
    if (!got) return false;
    *len = (uint8_t)plen;
    *rssi = (int8_t)radio_.getRSSI();
    *snr = (int8_t)radio_.getSNR();
    return true;
  }

  bool start_rx() {
    int state = radio_.startReceive();
    return state == RADIOLIB_ERR_NONE;
  }

 protected:
  explicit LoraRadioBase(Module* mod) : radio_(mod) {}
  /** The tuning currently in force, so begin() can prime the silicon and a
   *  failed apply() can report what the radio is really using. */
  RadioSettings last_settings_ = RadioSettings::unset();
  /** The RadioLib handle, so a derived begin() can do its bring-up on the
   *  same part the shared state machine drives. */
  Radio radio_;

 private:
  bool tx_started_ = false;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_RADIO_LORA_H
