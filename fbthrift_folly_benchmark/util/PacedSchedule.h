#pragma once

#include <cstdint>

namespace util {

// Even send schedule of one open-loop pacer: request k (k = 0, 1, ...) is due
// at start_us + k * 1e6 / qps.
class PacedSchedule {
 public:
  PacedSchedule(int64_t start_us, double qps) : start_us_(start_us), qps_(qps) {}

  // When request k is due.
  int64_t dueTime(int64_t k) const {
    return start_us_ + static_cast<int64_t>(k * 1e6 / qps_);
  }

  // How many requests are due by now_us, i.e. those with dueTime(k) <= now_us.
  int64_t dueCount(int64_t now_us) const {
    if (now_us < start_us_) return 0;
    // Estimate, then step past floating-point rounding so that the count
    // agrees with dueTime exactly.
    int64_t count = static_cast<int64_t>(qps_ * (now_us - start_us_) / 1e6) + 1;
    while (dueTime(count) <= now_us) ++count;
    while (count > 0 && dueTime(count - 1) > now_us) --count;
    return count;
  }

 private:
  int64_t start_us_;
  double qps_;
};

} // namespace util
