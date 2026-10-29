#pragma once

#include <functional>
#include <memory>
#include <string>

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <thrift/lib/cpp/concurrency/ThreadManager.h>

namespace press_server {

// Thread pools for one --pool_mode. A null pool means ThriftServer creates
// its default one (setNumIOWorkerThreads / setNumCPUWorkerThreads).
struct ServerPools {
  std::shared_ptr<folly::IOThreadPoolExecutor> io;
  // folly_cpu: passed to setThreadManagerFromExecutor (fbthrift wraps it in
  // VirtualExecutor + MeteredExecutors; kept as the original reference).
  std::shared_ptr<folly::CPUThreadPoolExecutor> cpu;
  // Every other mode: the executor behind a DirectThreadManager, passed to
  // setThreadManager.
  std::shared_ptr<folly::Executor> executor;
  std::shared_ptr<apache::thrift::concurrency::ThreadManager> threadManager;
  // One line of key=value pairs: cluster-aware and striped modes.
  std::function<std::string()> queueStats;
};

// mode: tm | folly_cpu | cpu | cpu_pinned | cluster | cluster2 |
//       cluster2_grab | striped | striped_pinned | manual
//   tm             - ThriftServer defaults (fbthrift ThreadManager), unpinned
//   folly_cpu      - folly CPUThreadPoolExecutor via
//                    setThreadManagerFromExecutor
//   cpu            - lean executor, one global queue, unpinned
//   cpu_pinned     - lean executor, one global queue, IO/CPU threads pinned
//                    round-robin to clusters
//   cluster        - lean executor, per-cluster queues
//                    (ClusterAwareBlockingQueue), threads pinned as above
//   cluster2       - as cluster with ClusterAware2BlockingQueue
//   cluster2_grab  - as cluster2 with its grab policy
//   striped        - CPUThreadPoolExecutor over upstream's LLC-striped queue
//                    (folly::striped_port), IO pool default, unpinned
//   striped_pinned - as striped, IO/CPU threads pinned as cpu_pinned
//   manual         - CPUThreadPoolExecutor (direct); IO threads pinned to
//                    ioCpus, CPU threads to cpuCpus (stage 1)
// Throws std::invalid_argument on an unknown mode or a bad cpulist.
ServerPools makeServerPools(
    const std::string& mode,
    int numIo,
    int numCpu,
    const std::string& ioCpus,
    const std::string& cpuCpus);

} // namespace press_server
