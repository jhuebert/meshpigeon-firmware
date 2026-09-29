#ifndef MESHPIGEON_FRAMING_H
#define MESHPIGEON_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

namespace meshpigeon {

/**
 * Frame layout on the wire (uniform across BLE / USB CDC / Wi-Fi TCP):
 *
 *   decoded frame: [payload:n][crc16:2]
 *   wire frame:    COBS_encode(decoded) followed by a single 0x00 delimiter
 *
 * Since v2 the payload is a serialized protobuf envelope — one
 * ClientToRadio or RadioToClient per frame (docs/radio-protocol.md); the
 * v1 [cmd][nonce][status] header is gone. The envelope carries its own
 * correlation id, operation and error code.
 *
 * - crc16 is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the
 *   serialized envelope, little-endian on the wire.
 * - Requests and responses both echo the envelope's id; async pushes
 *   (id = 0) use the same framing.
 */

// Max bytes a decoded frame can occupy (envelope + crc16).
#define FRAME_MAX_DECODED (MESHPIGEON_MAX_FRAME_PAYLOAD + 2)
// COBS worst case adds one overhead byte per 254 plus terminator.
#define FRAME_MAX_WIRE (FRAME_MAX_DECODED + (FRAME_MAX_DECODED + 253) / 254 + 1)

// How many frames' worth of bytes one board-loop pump may decode and
// dispatch. A transport that drains its inbound buffer with an unbounded
// `while (available())` hands the whole loop to whoever is fastest at
// writing: `Ping` is exempt from the auth gate and is answered every time,
// so a single attached client — or a LAN peer that only opened a socket —
// can keep the radio from ever being polled.
//
// This is a BYTE budget, not a frame count, and that is the point: a peer
// streaming pure garbage never completes a frame, so a frame counter would
// never advance and the read loop would still run forever. Expressed in
// maximum-size frames (a frame is at most FRAME_MAX_WIRE bytes), 16 of them
// per ~1 ms tick is far above what a radio interface actually needs.
#define FRAME_MAX_DRAIN_PER_PUMP 16
#define FRAME_MAX_DRAIN_BYTES_PER_PUMP \
  (FRAME_MAX_DRAIN_PER_PUMP * FRAME_MAX_WIRE)

size_t crc16_ccitt(uint16_t* crc_out, const uint8_t* data, size_t len,
                   uint16_t init = 0xFFFF);

/**
 * COBS-encode `src` into `dst`. `dst` must hold src_len + src_len/254 + 2
 * bytes. Returns bytes written (not including any 0x00 delimiter; caller
 * appends it). src_len == 0 encodes to a single 0x01 overhead byte.
 */
size_t cobs_encode(uint8_t* dst, const uint8_t* src, size_t src_len);

/**
 * COBS-decode `src` (without delimiter) into `dst`, which must hold at
 * most `dst_cap` bytes. Returns the decoded length, or 0 on malformed input
 * or if the decoded form would not fit.
 *
 * `dst_cap` is not advisory: a COBS body expands by up to 3 bytes over the
 * decoded length, so a hostile stream that fills FrameReader's wire buffer
 * would otherwise write past the caller's frame buffer.
 */
size_t cobs_decode(uint8_t* dst, const uint8_t* src, size_t src_len,
                   size_t dst_cap);

/** Compute the CRC16 over a decoded frame (all bytes except the last 2). */
uint16_t frame_crc(const uint8_t* frame, size_t frame_len);

/**
 * Build a decoded frame (envelope + trailing CRC) into `out`. `out` must
 * hold envelope_len + 2 bytes. Returns the total decoded length. The one
 * place a CRC is appended to an envelope, so tests and fixtures cannot
 * drift from what the transports put on the wire.
 */
size_t frame_build(uint8_t* out, const uint8_t* envelope, size_t envelope_len);

/** Encode a decoded frame (payload + trailing CRC) to wire format.
 *  `out` must hold FRAME_MAX_WIRE. */
size_t frame_encode_wire(uint8_t* out, const uint8_t* decoded, size_t decoded_len);

/**
 * Encode one serialized envelope to wire format: CRC-16 first (little-endian),
 * then COBS, then the 0x00 delimiter. This is what every transport calls
 * with the envelope it got from CommandProcessor; `out` must hold
 * FRAME_MAX_WIRE.
 */
size_t frame_encode_envelope(uint8_t* out, const uint8_t* envelope,
                             size_t envelope_len);

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
   * `frame_out` (must hold FRAME_MAX_DECODED) receives the decoded envelope
   * — the CRC is verified but not included — and the function returns its
   * length; returns 0 otherwise. Returns (size_t)-1 when a frame was
   * complete but unusable (CRC or decode failure, a body too long for
   * `frame_out`).
   *
   * A body longer than the reader's wire buffer is different: the reader
   * throws the bytes away and returns 0, because there is no complete
   * frame to report on — only the next delimiter ends the overrun.
   */
  size_t feed(uint8_t byte, uint8_t* frame_out);

 private:
  uint8_t wire_[FRAME_MAX_WIRE];
  size_t wire_len_;
  bool overflow_;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_FRAMING_H
