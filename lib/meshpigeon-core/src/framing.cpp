#include "meshpigeon/framing.h"

#include <string.h>

namespace meshpigeon {

size_t crc16_ccitt(uint16_t* crc_out, const uint8_t* data, size_t len,
                   uint16_t init) {
  uint16_t crc = init;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
    }
  }
  *crc_out = crc;
  return len;
}

size_t cobs_encode(uint8_t* dst, const uint8_t* src, size_t src_len) {
  CobsEncoder enc(dst);
  for (size_t i = 0; i < src_len; i++) enc.put(src[i]);
  return (size_t)(enc.finish() - dst);
}

size_t cobs_decode(uint8_t* dst, const uint8_t* src, size_t src_len,
                   size_t dst_cap) {
  if (src_len == 0) return 0;
  size_t written = 0;
  const uint8_t* end = src + src_len;

  while (src < end) {
    uint8_t code = *src++;
    if (code == 0) return 0;  // interior zero: malformed
    size_t block = code - 1;
    if ((size_t)(end - src) < block) return 0;  // truncated run
    // Every write is checked against the cap, so no input can overrun dst.
    if (written + block > dst_cap) return 0;
    for (size_t i = 0; i < block; i++) dst[written++] = *src++;
    if (code < 0xFF && src < end) {
      if (written >= dst_cap) return 0;
      dst[written++] = 0;
    }
  }
  return written;
}

uint16_t frame_crc16(const uint8_t* envelope, size_t envelope_len) {
  uint16_t crc = 0;
  crc16_ccitt(&crc, envelope, envelope_len);
  return crc;
}

uint16_t frame_crc(const uint8_t* frame, size_t frame_len) {
  if (frame_len < 2) return 0;
  return frame_crc16(frame, frame_len - 2);
}

size_t frame_build(uint8_t* out, const uint8_t* envelope,
                   size_t envelope_len) {
  if (envelope_len > 0 && envelope != NULL) {
    memcpy(out, envelope, envelope_len);
  }
  uint16_t crc = frame_crc16(envelope, envelope_len);
  out[envelope_len] = (uint8_t)(crc & 0xFF);
  out[envelope_len + 1] = (uint8_t)(crc >> 8);
  return envelope_len + 2;
}

size_t frame_encode_wire(uint8_t* out, const uint8_t* decoded,
                         size_t decoded_len) {
  size_t n = cobs_encode(out, decoded, decoded_len);
  out[n++] = 0x00;  // frame delimiter
  return n;
}

size_t frame_encode_envelope(uint8_t* out, const uint8_t* envelope,
                             size_t envelope_len) {
  if (envelope_len + 2 > FRAME_MAX_DECODED) return 0;
  // The envelope and its trailing CRC go into one COBS encoder, straight
  // into `out`: no scratch frame, so the deepest call chain pays ~40 bytes
  // of stack here instead of a FRAME_MAX_DECODED buffer.
  const uint16_t crc = frame_crc16(envelope, envelope_len);
  CobsEncoder enc(out);
  for (size_t i = 0; i < envelope_len; i++) enc.put(envelope[i]);
  enc.put((uint8_t)(crc & 0xFF));  // little-endian on the wire
  enc.put((uint8_t)(crc >> 8));
  size_t n = (size_t)(enc.finish() - out);
  out[n++] = 0x00;  // frame delimiter
  return n;
}

size_t FrameReader::feed(uint8_t byte, uint8_t* frame_out) {
  if (byte == 0x00) {  // frame delimiter
    if (overflow_ || wire_len_ == 0) {
      reset();
      return 0;
    }
    // The cap is what makes this safe: a wire body long enough to decode
    // past frame_out is rejected here, never written.
    size_t n = cobs_decode(frame_out, wire_, wire_len_, FRAME_MAX_DECODED);
    reset();
    if (n < 2) return (size_t)-1;  // malformed, or longer than any frame
    if (frame_crc(frame_out, n) !=
        (uint16_t)(frame_out[n - 2] | (frame_out[n - 1] << 8))) {
      return (size_t)-1;  // CRC failure
    }
    return n - 2;  // payload only: the CRC is verified, not handed on
  }
  if (wire_len_ >= sizeof(wire_)) {
    overflow_ = true;  // wait for the next delimiter, then drop
    return 0;
  }
  wire_[wire_len_++] = byte;
  return 0;
}

}  // namespace meshpigeon
