#include "server/ServerPools.h"

#include <stdexcept>
#include <utility>

#include <folly/Format.h>
#include <folly/executors/ClusterTopology.h>
#include <folly/executors/task_queue/ClusterAwareBlockingQueue.h>
#include <folly/executors/thread_factory/ClusterAffinityThreadFactory.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>

namespace press_server {

ServerPools makeServerPools(
    const std::string& mode,
    int numIo,
    int numCpu,
    const std::string& ioCpus,
    const std::string& cpuCpus) {
  const auto& topo = folly::ClusterTopology::instance();
  auto ioNamed = std::make_shared<folly::NamedThreadFactory>("SrvIO");
  auto cpuNamed = std::make_shared<folly::NamedThreadFactory>("SrvCPU");
  // min == max: create every thread up front so the round-robin cluster
  // assignment is even.
  const std::pair<size_t, size_t> ioThreads(numIo, numIo);
  const std::pair<size_t, size_t> cpuThreads(numCpu, numCpu);

  ServerPools pools;
  if (mode == "tm") {
    return pools;
  }
  if (mode == "cpu") {
    pools.cpu = std::make_shared<folly::CPUThreadPoolExecutor>(
        cpuThreads, std::move(cpuNamed));
    return pools;
  }

  std::shared_ptr<folly::ThreadFactory> ioFactory;
  std::shared_ptr<folly::ThreadFactory> cpuFactory;
  if (mode == "cpu_pinned" || mode == "cluster") {
    ioFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(ioNamed), topo);
    cpuFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(cpuNamed), topo);
  } else if (mode == "manual") {
    ioFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(ioNamed), topo, folly::parseCpuList(ioCpus));
    cpuFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(cpuNamed), topo, folly::parseCpuList(cpuCpus));
  } else {
    throw std::invalid_argument("unknown --pool_mode: " + mode);
  }

  pools.io = std::make_shared<folly::IOThreadPoolExecutor>(
      ioThreads.first, ioThreads.second, std::move(ioFactory));
  if (mode == "cluster") {
    using Queue = folly::ClusterAwareBlockingQueue<
        folly::CPUThreadPoolExecutor::CPUTask>;
    auto queue = std::make_unique<Queue>(topo);
    Queue* q = queue.get(); // owned by pools.cpu, which outlives queueStats
    pools.cpu = std::make_shared<folly::CPUThreadPoolExecutor>(
        cpuThreads, std::move(queue), std::move(cpuFactory));
    pools.queueStats = [q] {
      auto s = q->stats();
      auto total = s.localTakes + s.stolenTakes;
      return folly::sformat(
          "local={} stolen={} steal_ratio={:.4f}",
          s.localTakes,
          s.stolenTakes,
          total ? double(s.stolenTakes) / double(total) : 0.0);
    };
    return pools;
  }
  pools.cpu = std::make_shared<folly::CPUThreadPoolExecutor>(
      cpuThreads, std::move(cpuFactory));
  return pools;
}

} // namespace press_server
