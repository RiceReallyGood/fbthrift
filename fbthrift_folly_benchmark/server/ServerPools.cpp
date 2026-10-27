#include "server/ServerPools.h"

#include "server/DirectThreadManager.h"

#include <stdexcept>
#include <utility>

#include <folly/Format.h>
#include <folly/Indestructible.h>
#include <folly/executors/ClusterAwareExecutor.h>
#include <folly/executors/ClusterTopology.h>
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
  if (mode == "folly_cpu") {
    pools.cpu = std::make_shared<folly::CPUThreadPoolExecutor>(
        cpuThreads, std::move(cpuNamed));
    return pools;
  }
  if (mode == "cpu" || mode == "cpu_pinned" || mode == "cluster") {
    // One queue group (global queue) unless cluster-aware. Never destroyed:
    // executor threads may still use it while statics are destroyed at exit.
    static const folly::Indestructible<folly::ClusterTopology> global{[&] {
      std::vector<int> cpus;
      for (const auto& c : topo.clusters()) {
        cpus.insert(cpus.end(), c.cpus.begin(), c.cpus.end());
      }
      return folly::ClusterTopology::detect("/nonexistent", std::move(cpus));
    }()};
    const auto& queueTopo = mode == "cluster" ? topo : *global;
    std::shared_ptr<folly::ThreadFactory> cpuFactory = std::move(cpuNamed);
    if (mode != "cpu") {
      pools.io = std::make_shared<folly::IOThreadPoolExecutor>(
          ioThreads.first,
          ioThreads.second,
          std::make_shared<folly::ClusterAffinityThreadFactory>(
              std::move(ioNamed), topo));
      cpuFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
          std::move(cpuFactory), topo);
    }
    auto executor = std::make_shared<folly::ClusterAwareExecutor>(
        numCpu, queueTopo, std::move(cpuFactory));
    pools.queueStats = [ex = executor.get()] {
      auto s = ex->stats();
      auto total = s.localTakes + s.stolenTakes;
      return folly::sformat(
          "local={} stolen={} steal_ratio={:.4f}",
          s.localTakes,
          s.stolenTakes,
          total ? double(s.stolenTakes) / double(total) : 0.0);
    };
    pools.threadManager = std::make_shared<DirectThreadManager>(executor);
    pools.executor = std::move(executor);
    return pools;
  }

  std::shared_ptr<folly::ThreadFactory> ioFactory;
  std::shared_ptr<folly::ThreadFactory> cpuFactory;
  if (mode == "manual") {
    ioFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(ioNamed), topo, folly::parseCpuList(ioCpus));
    cpuFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
        std::move(cpuNamed), topo, folly::parseCpuList(cpuCpus));
  } else {
    throw std::invalid_argument("unknown --pool_mode: " + mode);
  }

  pools.io = std::make_shared<folly::IOThreadPoolExecutor>(
      ioThreads.first, ioThreads.second, std::move(ioFactory));
  auto cpuPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      cpuThreads, std::move(cpuFactory));
  pools.threadManager = std::make_shared<DirectThreadManager>(cpuPool);
  pools.executor = std::move(cpuPool);
  return pools;
}

} // namespace press_server
