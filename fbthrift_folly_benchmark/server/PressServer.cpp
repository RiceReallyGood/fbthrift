#include <signal.h>
#include <atomic>
#include <iostream>

#include <glog/logging.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>
#include <folly/experimental/io/IoUringBackend.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/executors/ClusterTopology.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/transport/rocket/server/RocketRoutingHandler.h>

#include "server/PerfTestService.h"
#include "server/ServerPools.h"
#include "util/ResourceMonitor.h"
#include <thrift/lib/cpp2/gen/client_cpp.h>
DEFINE_int32(port, 8002, "TCP Port of this server");
DEFINE_int32(num_io_threads, 1, "Number of IO worker threads");
DEFINE_int32(num_cpu_threads, 1, "Number of CPU worker threads");
DEFINE_bool(use_io_uring, false, "Use IoUringBackend for IO threads");
DEFINE_string(pool_mode, "tm", "Thread pools: tm | folly_cpu | cpu | cpu_pinned | cluster | manual");
DEFINE_string(io_cpus, "", "manual mode: cpulist for IO threads");
DEFINE_string(cpu_cpus, "", "manual mode: cpulist for CPU threads");

FOLLY_GFLAGS_DECLARE_bool(dynamic_iothreadpoolexecutor);

using apache::thrift::RocketRoutingHandler;
using apache::thrift::ThriftServer;
using press_test::PerfTestServiceHandler;

struct ServerHolder {
  std::shared_ptr<folly::EventBaseManager> ebm;
  press_server::ServerPools pools;
  std::shared_ptr<ThriftServer> server;
};
ServerHolder g_server;
std::atomic<bool> g_stop_requested{false};

void signalHandler(int sig) {
    if (g_server.server && !g_stop_requested.exchange(true)) {
        LOG(INFO) << "Received signal " << sig << ", stopping server...";
        g_server.server->stop();
    }
}

std::unique_ptr<RocketRoutingHandler> createRocketRoutingHandler(
    std::shared_ptr<ThriftServer> server) {
  return std::make_unique<RocketRoutingHandler>(*server);
}

template <typename ServiceHandler>
ServerHolder newServer(int32_t port, int32_t num_io_threads, int32_t num_cpu_threads) {
  auto handler = std::make_shared<ServiceHandler>();
  auto server = std::make_shared<ThriftServer>();
  ServerHolder holder;

  server->setInterface(handler);
  holder.pools = press_server::makeServerPools(
      FLAGS_pool_mode, num_io_threads, num_cpu_threads, FLAGS_io_cpus, FLAGS_cpu_cpus);
  if (FLAGS_use_io_uring) {
    CHECK(FLAGS_pool_mode == "tm") << "--use_io_uring only works with --pool_mode=tm";
    folly::EventBaseBackendBase::FactoryFunc bf = []() {
      folly::IoUringBackend::Options opts;
      opts.setCapacity(65536).setMaxSubmit(4096).setInitialProvidedBuffers(4096, 1024);
      return std::unique_ptr<folly::EventBaseBackendBase>(new folly::IoUringBackend(opts));
    };
    holder.ebm = std::make_shared<folly::EventBaseManager>(bf);
    auto io_pool = std::make_shared<folly::IOThreadPoolExecutor>(
        num_io_threads, std::make_shared<folly::NamedThreadFactory>("ServerIOThreadPool"), holder.ebm.get());
    server->setIOThreadPool(io_pool);
    server->setPreferIoUring(true);
  } else if (holder.pools.io) {
    server->setIOThreadPool(holder.pools.io);
  } else {
    server->setNumIOWorkerThreads(num_io_threads);
  }
  server->setPort(port);
  if (holder.pools.threadManager) {
    server->setThreadManager(holder.pools.threadManager);
  } else if (holder.pools.cpu) {
    server->setThreadManagerFromExecutor(holder.pools.cpu.get(), "press");
  } else {
    server->setNumCPUWorkerThreads(num_cpu_threads);
  }
  server->addRoutingHandler(createRocketRoutingHandler(server));
  holder.server = server;
  return holder;
}

int main(int argc, char** argv) {
  FLAGS_logtostderr = 1;
  folly::init(&argc, &argv);
  // Create every pool thread up front (even round-robin cluster assignment).
  FLAGS_dynamic_cputhreadpoolexecutor = false;
  FLAGS_dynamic_iothreadpoolexecutor = false;

  util::init_resource_collector();

  LOG(INFO) << "pool_mode=" << FLAGS_pool_mode << " num_io=" << FLAGS_num_io_threads
            << " num_cpu=" << FLAGS_num_cpu_threads << " topology:\n"
            << folly::ClusterTopology::instance().describe();

  g_server = newServer<PerfTestServiceHandler>(FLAGS_port, FLAGS_num_io_threads, FLAGS_num_cpu_threads);

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  LOG(INFO) << "PerfTest Server running on port: " << FLAGS_port
            << " with " << FLAGS_num_io_threads << " IO threads and "
            << FLAGS_num_cpu_threads << " CPU threads (Rocket transport)";

  g_server.server->serve();

  if (g_server.pools.queueStats) {
    std::cout << "QUEUE_STATS " << g_server.pools.queueStats() << std::endl;
  }
  LOG(INFO) << "Server exited gracefully.";
  return 0;
}