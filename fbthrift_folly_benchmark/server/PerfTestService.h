#pragma once

#include <atomic>
#include <cstdint>
#include "gen-cpp2/PerfTestService.h"
#include "util/LatencyStats.h"

namespace press_test {

class PerfTestServiceHandler
    : virtual public apache::thrift::ServiceHandler<PerfTestService> {
 public:
  void test(
      PerfTestResponse& response,
      std::unique_ptr<Request> req) override;

  // thread='eb': runs on the IO thread, replies inline.
  void async_eb_testEb(
      std::unique_ptr<apache::thrift::HandlerCallback<
          std::unique_ptr<PerfTestResponse>>> callback,
      std::unique_ptr<Request> req) override;

 private:
  static void fillResponse(PerfTestResponse& response, const Request& req);

  static std::atomic<uint64_t> g_last_resource_update_time;
  static constexpr uint64_t CPU_UPDATE_INTERVAL_US = 100000;  // 100ms
};

} // namespace press_test