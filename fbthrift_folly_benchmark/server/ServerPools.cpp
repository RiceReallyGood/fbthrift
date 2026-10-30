#include "server/ServerPools.h"

#include "server/DirectThreadManager.h"

#include <stdexcept>
#include <utility>

#include <folly/Format.h>
#include <folly/Indestructible.h>
#include <folly/String.h>
#include <folly/executors/ClusterAwareExecutor.h>
#include <folly/executors/ClusterTopology.h>
#include <folly/executors/striped_port/LLCAccessSpreader.h>
#include <folly/executors/striped_port/StripedPriorityUnboundedBlockingQueue.h>
#include <folly/executors/striped_port/StripedThrottledLifoSem.h>
#include <folly/executors/thread_factory/ClusterAffinityThreadFactory.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>

namespace press_server {

namespace {

std::string formatStats(const folly::ClusterAware2Executor::Stats& s) {
  auto total = s.localTakes + s.stolenTakes;
  return folly::sformat(
      "local={} stolen={} steal_ratio={:.4f} assign_local={} assign_numa={} "
      "assign_far={} orphan_add={} orphan_take={} orphan_miss={} "
      "empty_target={} handoff={} grab_take={} grab_miss={} grab_skip={} "
      "wake_local={} wake_remote={}",
      s.localTakes,
      s.stolenTakes,
      total ? double(s.stolenTakes) / double(total) : 0.0,
      s.assignLocal,
      s.assignSameNuma,
      s.assignOtherNuma,
      s.orphanAdds,
      s.orphanTakes,
      s.orphanMisses,
      s.emptyTargetWakes,
      s.handoffs,
      s.grabTakes,
      s.grabMisses,
      s.grabSkips,
      folly::join(',', s.wakeLocal),
      folly::join(',', s.wakeRemote));
}

} // namespace

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
  const bool cluster2 = mode == "cluster2" || mode == "cluster2_grab";
  const bool striped = mode == "striped" || mode == "striped_pinned";
  if (mode == "cpu" || mode == "cpu_pinned" || mode == "cluster" || cluster2 ||
      striped) {
    std::shared_ptr<folly::ThreadFactory> cpuFactory = std::move(cpuNamed);
    if (mode != "cpu" && mode != "striped") {
      pools.io = std::make_shared<folly::IOThreadPoolExecutor>(
          ioThreads.first,
          ioThreads.second,
          std::make_shared<folly::ClusterAffinityThreadFactory>(
              std::move(ioNamed), topo));
      cpuFactory = std::make_shared<folly::ClusterAffinityThreadFactory>(
          std::move(cpuFactory), topo);
    }
    if (striped) {
      // LLCAccessSpreader counts the LLCs in the affinity of the thread that
      // first uses it, so create the queue here, on the main thread (whole
      // server cpuset), before any pinned thread exists. 3 priorities as in
      // upstream fbthrift's striped executor queue.
      auto queue = folly::striped_port::StripedPriorityUnboundedBlockingQueue<
          folly::CPUThreadPoolExecutor::CPUTask>::create(3);
      auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(
          cpuThreads, std::move(queue), std::move(cpuFactory));
      pools.queueStats = [] {
        return folly::sformat(
            "stripes={} balancer_transfers={}",
            folly::striped_port::LLCAccessSpreader::get().numStripes(),
            folly::striped_port::StripedThrottledLifoSemBalancer::
                totalTransfers());
      };
      pools.threadManager = std::make_shared<DirectThreadManager>(executor);
      pools.executor = std::move(executor);
      return pools;
    }
    if (cluster2) {
      auto executor = std::make_shared<folly::ClusterAware2Executor>(
          numCpu,
          topo,
          std::move(cpuFactory),
          folly::ClusterAware2Options{mode == "cluster2_grab"});
      pools.queueStats = [ex = executor.get()] {
        return formatStats(ex->stats());
      };
      pools.threadManager = std::make_shared<DirectThreadManager>(executor);
      pools.executor = std::move(executor);
      return pools;
    }
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
