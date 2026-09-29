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
  uint8_t* dst_start = dst;
  uint8_t* code_ptr = dst++;
  uint8_t code = 1;

  for (size_t i = 0; i < src_len; i++) {
    if (src[i] != 0) {
      *dst++ = src[i];
      code++;
    }
    if (src[i] == 0 || code == 0xFF) {
      *code_ptr = code;
      code_ptr = dst++;
      code = 1;
    }
  }
  *code_ptr = code;
  return (size_t)(dst - dst_start);
}

size_t cobs_decode(uint8_t* dst, const uint8_t* src, size_t src_len) {
  if (src_len == 0) return 0;
  uint8_t* dst_start = dst;
  const uint8_t* end = src + src_len;

  while (src < end) {
    uint8_t code = *src++;
    if (code == 0) return 0;  // interior zero: malformed
    size_t block = code - 1;
    if ((size_t)(end - src) < block) return 0;
    for (size_t i = 0; i < block; i++) *dst++ = *src++;
    if (code < 0xFF && src < end) *dst++ = 0;
  }
  return (size_t)(dst - dst_start);
}

uint16_t frame_crc(const uint8_t* frame, size_t frame_len) {
  if (frame_len < 2) return 0;
  uint16_t crc = 0;
  crc16_ccitt(&crc, frame, frame_len - 2);
  return crc;
}

size_t frame_build(uint8_t* out, const uint8_t* envelope,
                   size_t envelope_len) {
  if (envelope_len > 0 && envelope != NULL) {
    memcpy(out, envelope, envelope_len);
  }
  uint16_t crc = 0;
  crc16_ccitt(&crc, out, envelope_len);
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
  uint8_t with_crc[FRAME_MAX_DECODED];
  if (envelope_len + 2 > sizeof(with_crc)) return 0;
  return frame_encode_wire(out, with_crc,
                           frame_build(with_crc, envelope, envelope_len));
}

size_t FrameReader::feed(uint8_t byte, uint8_t* frame_out) {
  if (byte == 0x00) {  // frame delimiter
    if (overflow_ || wire_len_ == 0) {
      reset();
      return 0;
    }
    size_t n = cobs_decode(frame_out, wire_, wire_len_);
    reset();
    if (n < 2 || n > FRAME_MAX_DECODED) return (size_t)-1;  // malformed
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
