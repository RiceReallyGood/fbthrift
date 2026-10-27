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
  // folly_cpu / manual: passed to setThreadManagerFromExecutor.
  std::shared_ptr<folly::CPUThreadPoolExecutor> cpu;
  // cpu / cpu_pinned / cluster: a folly::ClusterAwareExecutor behind a
  // ThreadManagerExecutorAdapter, passed to setThreadManager (no
  // VirtualExecutor keep-alive counting per request).
  std::shared_ptr<folly::Executor> executor;
  std::shared_ptr<apache::thrift::concurrency::ThreadManager> threadManager;
  std::function<std::string()> queueStats; // cluster-aware executor modes
};

// mode: tm | folly_cpu | cpu | cpu_pinned | cluster | manual
//   tm         - ThriftServer defaults (fbthrift ThreadManager), unpinned
//   folly_cpu  - folly CPUThreadPoolExecutor via setThreadManagerFromExecutor
//   cpu        - lean executor, one global queue, unpinned
//   cpu_pinned - lean executor, one global queue, IO/CPU threads pinned
//                round-robin to clusters
//   cluster    - lean executor, per-cluster queues, threads pinned as above
//   manual     - CPUThreadPoolExecutor; IO threads pinned to ioCpus, CPU
//                threads to cpuCpus (stage 1)
// Throws std::invalid_argument on an unknown mode or a bad cpulist.
ServerPools makeServerPools(
    const std::string& mode,
    int numIo,
    int numCpu,
    const std::string& ioCpus,
    const std::string& cpuCpus);

} // namespace press_server
