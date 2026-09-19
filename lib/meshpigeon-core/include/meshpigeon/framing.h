#ifndef MESHPGEON_FRAMING_H
#define MESHPGEON_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

namespace meshpigeon {

/**
 * Frame layout on the wire (uniform across BLE / USB CDC / Wi-Fi TCP):
 *
 *   decoded frame: [cmd:1][nonce:1][status_or_0:1][payload:n][crc16:2]
 *   wire frame:    COBS_encode(decoded) followed by a single 0x00 delimiter
 *
 * - crc16 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over
 *   cmd + nonce + status + payload, little-endian on the wire.
 * - Requests use status 0x00. Responses use the same command code as the
 *   request and carry the request's nonce; status != 0 means error
 *   (see STATUS_* codes), and payload holds a short message when errored.
 * - Async frames use their own CMD_* code, nonce 0, status 0.
 */

// Max bytes a decoded frame can occupy (cmd+nonce+status+payload+crc16).
#define FRAME_MAX_DECODED (3 + MESHPGEON_MAX_FRAME_PAYLOAD + 2)
// COBS worst case adds one overhead byte per 254 plus terminator.
#define FRAME_MAX_WIRE (FRAME_MAX_DECODED + (FRAME_MAX_DECODED + 253) / 254 + 1)

size_t crc16_ccitt(uint16_t* crc_out, const uint8_t* data, size_t len,
                   uint16_t init = 0xFFFF);

/**
 * COBS-encode `src` into `dst`. `dst` must hold src_len + src_len/254 + 2
 * bytes. Returns bytes written (not including any 0x00 delimiter; caller
 * appends it). src_len == 0 encodes to a single 0x01 overhead byte.
 */
size_t cobs_encode(uint8_t* dst, const uint8_t* src, size_t src_len);

/**
 * COBS-decode `src` (without delimiter) into `dst`. `dst` must hold at
 * least src_len bytes. Returns decoded length, or 0 on malformed input.
 */
size_t cobs_decode(uint8_t* dst, const uint8_t* src, size_t src_len);

/** Compute the CRC16 over a decoded frame (all bytes except the last 2). */
uint16_t frame_crc(const uint8_t* frame, size_t frame_len);

/**
 * Build a full decoded frame (cmd/nonce/status/payload + crc) into `out`.
 * `out` must hold payload_len + 5 bytes. Returns total decoded length.
 */
size_t frame_build(uint8_t* out, uint8_t cmd, uint8_t nonce, uint8_t status,
                   const uint8_t* payload, size_t payload_len);

/** Encode a decoded frame to wire format. `out` must hold FRAME_MAX_WIRE. */
size_t frame_encode_wire(uint8_t* out, const uint8_t* decoded, size_t decoded_len);

/**
 * Frame reader: accumulates wire bytes until the 0x00 delimiter and emits
 * complete decoded frames. Reusable across board ports (BLE fragments,
 * USB, TCP). Non-frame garbage before a delimiter corrupts that one
 * frame (dropped on CRC/decode failure) and self-heals at the next one.
 */
class FrameReader {
 public:
  FrameReader() { reset(); }

  void reset() {
    wire_len_ = 0;
    overflow_ = false;
  }

  /**
   * Feed one wire byte. If a complete frame is available after this call,
   * `frame_out` (must hold FRAME_MAX_DECODED) receives the decoded bytes
   * and the function returns its length; returns 0 otherwise; returns
   * (size_t)-1 if a frame was received but dropped (CRC/decode failure or
   * overflow).
   */
  size_t feed(uint8_t byte, uint8_t* frame_out);

 private:
  uint8_t wire_[FRAME_MAX_WIRE];
  size_t wire_len_;
  bool overflow_;
};

}  // namespace meshpigeon

#endif  // MESHPGEON_FRAMING_H
