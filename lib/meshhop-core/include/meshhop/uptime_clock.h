#ifndef MESHHOP_UPTIME_CLOCK_H
#define MESHHOP_UPTIME_CLOCK_H

#include <stdint.h>

namespace meshhop {

/**
 * Free-running 32-bit uptime in milliseconds (wraps ~49.7 days). Wrap is
 * disambiguated by a boot count persisted alongside settings: every boot
 * increments it, and GET_INFO reports it so the app can anchor absolute
 * time. No RTC required (04-firmware §1.5).
 *
 * Board ports implement millis() semantics behind IMillisecondClock.
 */
class IMillisecondClock {
 public:
  virtual ~IMillisecondClock() {}
  virtual uint32_t millis() = 0;
};

class UptimeClock {
 public:
  explicit UptimeClock(IMillisecondClock& source) : source_(source) {}

  uint32_t uptime_ms() const { return source_.millis(); }
  uint32_t boot_count() const { return boot_count_; }
  void set_boot_count(uint32_t n) { boot_count_ = n; }

 private:
  IMillisecondClock& source_;
  uint32_t boot_count_ = 0;
};

/** Fixed clock for tests/simulator. */
class ManualClock : public IMillisecondClock {
 public:
  explicit ManualClock(uint32_t start = 0) : now_(start) {}
  uint32_t millis() override { return now_; }
  void advance(uint32_t ms) { now_ += ms; }

 private:
  uint32_t now_;
};

}  // namespace meshhop

#endif  // MESHHOP_UPTIME_CLOCK_H
