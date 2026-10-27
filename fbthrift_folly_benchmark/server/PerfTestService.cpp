#include "server/PerfTestService.h"
#include "util/ResourceMonitor.h"
#include <atomic>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>

namespace press_test {

std::atomic<uint64_t> PerfTestServiceHandler::g_last_resource_update_time{0};

// ============ Handler 实现 ============

void PerfTestServiceHandler::test(PerfTestResponse &response,
                                  std::unique_ptr<Request> req)
{
    fillResponse(response, *req);
}

void PerfTestServiceHandler::async_eb_testEb(
    std::unique_ptr<apache::thrift::HandlerCallback<
        std::unique_ptr<PerfTestResponse>>> callback,
    std::unique_ptr<Request> req)
{
    auto response = std::make_unique<PerfTestResponse>();
    fillResponse(*response, *req);
    callback->result(std::move(response));
}

void PerfTestServiceHandler::fillResponse(PerfTestResponse &response,
                                          const Request &req)
{
    if (*req.echo_attachment()) {
        *response.payload() = *req.payload();
    }

    uint64_t last = g_last_resource_update_time.load(std::memory_order_relaxed);
    uint64_t now = util::ResourceCollector::monotonic_us();

    if (now > last && now - last > CPU_UPDATE_INTERVAL_US) {
        if (g_last_resource_update_time.compare_exchange_weak(
                last, now, std::memory_order_relaxed)) {
            auto snap = util::g_snapshot.load(std::memory_order_acquire);
            *response.cpu_usage() = snap.cpu_percent;
            *response.mem_usage() = snap.mem_percent;
        } else {
            *response.cpu_usage() = -1.0;
            *response.mem_usage() = -1.0;
        }
    } else {
        *response.cpu_usage() = -1.0;
        *response.mem_usage() = -1.0;
    }
}

} // namespace press_test