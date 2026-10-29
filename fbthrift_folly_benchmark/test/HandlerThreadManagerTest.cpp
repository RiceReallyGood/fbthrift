// handler_tm_test: a handler callback's getThreadManager_deprecated() (called
// for every request by the generated async_tm_ code) is the server's
// ThreadManager, with ThriftServer's default ThreadManager and with a pool
// mode's DirectThreadManager.
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <folly/Portability.h>
#include <folly/SocketAddress.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "gen-cpp2/PerfTestService.h"
#include "gen-cpp2/PerfTestServiceAsyncClient.h"
#include "server/PerfTestService.h"
#include "server/ServerPools.h"

FOLLY_GFLAGS_DECLARE_uint32(folly_global_io_executor_threads);

using apache::thrift::ScopedServerInterfaceThread;
using apache::thrift::concurrency::ThreadManager;

static int g_failures = 0;

#define EXPECT_THAT(cond, what)                                              \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " << what << std::endl; \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Records the ThreadManager each request's callback reports.
class TmProbeHandler : public press_test::PerfTestServiceHandler {
 public:
  void async_tm_test(
      std::unique_ptr<apache::thrift::HandlerCallback<
          std::unique_ptr<press_test::PerfTestResponse>>> callback,
      std::unique_ptr<press_test::Request> /* req */) override {
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
    ThreadManager* tm = callback->getThreadManager_deprecated();
    FOLLY_POP_WARNING
    {
      std::lock_guard<std::mutex> g(mutex_);
      seen_.push_back(tm);
    }
    callback->result(std::make_unique<press_test::PerfTestResponse>());
  }

  std::vector<ThreadManager*> seen() {
    std::lock_guard<std::mutex> g(mutex_);
    return seen_;
  }

 private:
  std::mutex mutex_;
  std::vector<ThreadManager*> seen_;
};

static void checkScenario(
    const std::string& name, ScopedServerInterfaceThread::ServerConfigCb configCb) {
  constexpr int kRequests = 100;
  auto handler = std::make_shared<TmProbeHandler>();
  ScopedServerInterfaceThread runner(
      handler, folly::SocketAddress("127.0.0.1", 0), std::move(configCb));
  auto client = runner.newClient<press_test::PerfTestServiceAsyncClient>();
  for (int i = 0; i < kRequests; ++i) {
    press_test::PerfTestResponse response;
    client->sync_test(response, press_test::Request());
  }
  ThreadManager* expected =
      runner.getThriftServer().getThreadManager_deprecated().get();
  auto seen = handler->seen();
  EXPECT_THAT(expected != nullptr, name << ": server has no ThreadManager");
  EXPECT_THAT(seen.size() == kRequests, name << ": " << seen.size() << " requests seen");
  int mismatches = 0;
  for (auto* tm : seen) {
    mismatches += tm != expected;
  }
  EXPECT_THAT(mismatches == 0, name << ": " << mismatches << " of " << seen.size()
                                    << " callbacks report another ThreadManager");
  std::cout << name << ": " << seen.size() << " requests checked" << std::endl;
}

int main(int argc, char** argv) {
  folly::init(&argc, &argv); // ThriftServer uses folly singletons
  FLAGS_dynamic_cputhreadpoolexecutor = false;
  // The clients' channels run on the global IO executor, one EventBase per
  // CPU by default: on a 384-CPU machine that exceeds the default fd limit.
  FLAGS_folly_global_io_executor_threads = 4;
  checkScenario("default ThreadManager", {});
  // The pool must outlive the server that runs on it.
  auto pools = press_server::makeServerPools("cluster2", 2, 4, "", "");
  checkScenario("cluster2 DirectThreadManager", [tm = pools.threadManager](
                                                    apache::thrift::ThriftServer& server) {
    server.setThreadManager(tm);
  });
  if (g_failures > 0) {
    std::cerr << "FAILED (" << g_failures << ")" << std::endl;
    return 1;
  }
  std::cout << "OK" << std::endl;
  return 0;
}
