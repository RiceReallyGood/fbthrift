// paced_schedule_test: the open-loop pacer's send schedule.
#include <cstdint>
#include <initializer_list>
#include <iostream>

#include "util/PacedSchedule.h"

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                           \
  do {                                                                       \
    auto actual_ = (actual);                                                 \
    auto expected_ = (expected);                                             \
    if (actual_ != expected_) {                                              \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " #actual " = "         \
                << actual_ << ", expected " << expected_ << std::endl;       \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Request 0 is due at the start, each later one a full interval after the
// previous one (here 250 us).
static void testRequestsDueOneIntervalApartFromStart() {
  util::PacedSchedule schedule(1000, 4000);
  CHECK_EQ(schedule.dueTime(0), 1000);
  CHECK_EQ(schedule.dueTime(1), 1250);
  CHECK_EQ(schedule.dueTime(4), 2000);
  CHECK_EQ(schedule.dueCount(999), 0);
  CHECK_EQ(schedule.dueCount(1000), 1);
  CHECK_EQ(schedule.dueCount(1249), 1);
  CHECK_EQ(schedule.dueCount(1250), 2);
  CHECK_EQ(schedule.dueCount(2000), 5);
}

// dueCount(now) is exactly the number of requests whose dueTime is <= now,
// also when the interval is not a whole number of microseconds.
static void testDueCountMatchesDueTimes() {
  for (double qps : {3000.0, 3750.0, 12500.0, 100000.0 / 7}) {
    util::PacedSchedule schedule(0, qps);
    int64_t due = 0;
    for (int64_t now = 0; now < 200000; ++now) {
      while (schedule.dueTime(due) <= now) ++due;
      if (schedule.dueCount(now) != due) {
        std::cerr << "qps=" << qps << " now=" << now << ": dueCount = "
                  << schedule.dueCount(now) << ", expected " << due << std::endl;
        ++g_failures;
        break;
      }
    }
  }
}

int main() {
  testRequestsDueOneIntervalApartFromStart();
  testDueCountMatchesDueTimes();
  if (g_failures > 0) {
    std::cerr << "FAILED (" << g_failures << ")" << std::endl;
    return 1;
  }
  std::cout << "PASSED" << std::endl;
  return 0;
}
