#ifndef MESHPIGEON_UPTIME_CLOCK_H
#define MESHPIGEON_UPTIME_CLOCK_H

#include <stdint.h>

namespace meshpigeon {

/**
 * Free-running 32-bit uptime in milliseconds. Board ports implement millis()
 * semantics behind IMillisecondClock.
 */
class IMillisecondClock {
 public:
  virtual ~IMillisecondClock() {}
  virtual uint32_t millis() = 0;
};

/**
 * Monotonic 64-bit uptime in milliseconds (docs/radio-protocol.md §5.1): the 32-bit millis()
 * counter plus a RAM rollover count this class increments whenever it
 * observes the source wrap. Clients pair this with app time once per
 * connection; packets carry the same clock, so every timestamp is plain
 * arithmetic — no rollover/boot reasoning on the client side. uint64 ms
 * cannot wrap in practice (~584 million years).
 *
 * The rollover counter is RAM-only — no flash writes for time, ever. A
 * reboot legitimately restarts the clock (and empties the RAM packet store);
 * clients re-anchor trivially.
 *
 * The wrap is only observed if this clock is polled across it, which is what
 * every caller does (the board loop / sim tick runs continuously); poll()
 * is still there so tests can force it. boot_count is a persisted, purely
 * informational counter of real boots — it plays no role in timestamping.
 */
class UptimeClock {
 public:
  explicit UptimeClock(IMillisecondClock& source) : source_(source) {}

  /** Board loop tick: refresh the 64-bit view, catching a wrap. */
  void poll() {
    uint32_t now = source_.millis();
    // A monotonic millis() counter only ever goes backwards when it wraps
    // (a real restart re-creates this object), so a smaller reading means
    // one more rollover — as long as poll() runs at least once per 49.7
    // days, which any board loop does thousands of times over.
    if (now < last_ms_) rollovers_++;
    last_ms_ = now;
  }

  uint64_t uptime_ms64() const { return (uint64_t)rollovers_ << 32 | last_ms_; }
  uint32_t boot_count() const { return boot_count_; }
  void set_boot_count(uint32_t n) { boot_count_ = n; }

 private:
  IMillisecondClock& source_;
  uint32_t last_ms_ = 0;
  uint32_t rollovers_ = 0;
  uint32_t boot_count_ = 0;
};

/** Fixed clock for tests/simulator. */
class ManualClock : public IMillisecondClock {
 public:
  explicit ManualClock(uint32_t start = 0) : now_(start) {}
  uint32_t millis() override { return now_; }
  void advance(uint32_t ms) { now_ += ms; }
  void set(uint32_t ms) { now_ = ms; }

 private:
  uint32_t now_;
};

}  // namespace meshpigeon

#endif  // MESHPIGEON_UPTIME_CLOCK_H
