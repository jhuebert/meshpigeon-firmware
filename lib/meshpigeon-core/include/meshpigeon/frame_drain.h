#ifndef MESHPIGEON_FRAME_DRAIN_H
#define MESHPIGEON_FRAME_DRAIN_H

#include <stddef.h>
#include <stdint.h>

#include "meshpigeon/command_processor.h"
#include "meshpigeon/framing.h"

namespace meshpigeon {

/**
 * Drain one transport's inbound stream into frames and hand them to the
 * core, bounded per board-loop tick.
 *
 * Every transport that reads from a socket or a FIFO calls this, and that
 * is the whole point of it. The budget is a safety property, not a
 * throughput tweak: `Ping` is auth-exempt, so a client — or a LAN peer that
 * only opened a socket — that keeps its inbound buffer fed would otherwise
 * own the board loop and starve the radio. It is in BYTES rather than
 * frames because a peer streaming pure garbage never completes a frame, so
 * a frame counter would never advance (docs/radio-protocol.md §10).
 *
 * A hand-written copy of this loop per transport is how that rule gets
 * forgotten on the fourth transport, so there is one of them here instead.
 * `src` only has to offer `int available()` and `int read()`; `frame_out`
 * must hold FRAME_MAX_DECODED and belongs to the caller (so one FrameReader
 * can be shared by a stream that delivers bytes out of order — a BLE link's
 * per-central readers, say).
 */
template <typename Src>
void drain_frames(CommandProcessor& proc, FrameReader& reader,
                  uint8_t* frame_out, IFrameSink* to, Src& src) {
  size_t budget = FRAME_MAX_DRAIN_BYTES_PER_PUMP;
  while (budget-- > 0 && src.available() > 0) {
    size_t res = reader.feed((uint8_t)src.read(), frame_out);
    if (res != 0 && res != (size_t)-1) {
      proc.on_envelope(frame_out, res, to);
    }
  }
}

}  // namespace meshpigeon

#endif  // MESHPIGEON_FRAME_DRAIN_H
