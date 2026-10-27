#pragma once

#include <functional>
#include <memory>
#include <string>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>

namespace press_server {

// Thread pools for one --pool_mode. A null pool means ThriftServer creates
// its default one (setNumIOWorkerThreads / setNumCPUWorkerThreads).
struct ServerPools {
  std::shared_ptr<folly::IOThreadPoolExecutor> io;
  std::shared_ptr<folly::CPUThreadPoolExecutor> cpu;
  std::function<std::string()> queueStats; // cluster mode only
};

// mode: tm | cpu | cpu_pinned | manual | cluster
//   tm         - ThriftServer defaults (fbthrift ThreadManager), unpinned
//   cpu        - folly CPUThreadPoolExecutor, unpinned
//   cpu_pinned - IO and CPU threads pinned round-robin to clusters
//   manual     - IO threads pinned to ioCpus, CPU threads to cpuCpus
//   cluster    - cpu_pinned + cluster-aware task queue
// Throws std::invalid_argument on an unknown mode or a bad cpulist.
ServerPools makeServerPools(
    const std::string& mode,
    int numIo,
    int numCpu,
    const std::string& ioCpus,
    const std::string& cpuCpus);

} // namespace press_server
