#pragma once

#include <memory>
#include <string>
#include <utility>

#include <folly/Executor.h>
#include <thrift/lib/cpp/concurrency/PosixThreadFactory.h>
#include <thrift/lib/cpp/concurrency/ThreadManager.h>

namespace press_server {

// ThreadManager that hands every task straight to `executor`, whatever its
// source or priority (modelled on fbthrift's ExecutorToThreadManagerAdaptor).
// ThreadManagerExecutorAdapter instead wraps non-internal sources (all
// requests) in nested folly::MeteredExecutors, which dispatch one task per
// executor hop and cap throughput far below the default ThreadManager.
class DirectThreadManager : public apache::thrift::concurrency::ThreadManager {
 public:
  explicit DirectThreadManager(std::shared_ptr<folly::Executor> executor)
      : executor_(std::move(executor)) {}

  void add(
      std::shared_ptr<apache::thrift::concurrency::Runnable> task,
      [[maybe_unused]] int64_t timeout = 0,
      [[maybe_unused]] int64_t expiration = 0,
      [[maybe_unused]] Source source = Source::UPSTREAM) noexcept override {
    executor_->add([task = std::move(task)]() { task->run(); });
  }

  void add(folly::Func f) override { executor_->add(std::move(f)); }

  [[nodiscard]] KeepAlive<> getKeepAlive(
      ExecutionScope, Source) const override {
    return folly::getKeepAliveToken(executor_.get());
  }

  void start() override {}
  void stop() override {}
  void join() override {}
  STATE state() const override { return ThreadManager::STARTED; }
  std::shared_ptr<apache::thrift::concurrency::ThreadFactory> threadFactory()
      const override {
    return std::make_shared<apache::thrift::concurrency::PosixThreadFactory>();
  }
  void threadFactory(
      std::shared_ptr<apache::thrift::concurrency::ThreadFactory>) override {}
  std::string getNamePrefix() const override { return "press.direct"; }
  void setNamePrefix(const std::string&) override {}
  void addWorker(size_t) override {}
  void removeWorker(size_t) override {}
  size_t idleWorkerCount() const override { return 0; }
  size_t workerCount() const override { return 0; }
  size_t pendingTaskCount() const override { return 0; }
  size_t pendingUpstreamTaskCount() const override { return 0; }
  size_t totalTaskCount() const override { return 0; }
  size_t expiredTaskCount() override { return 0; }
  void remove(std::shared_ptr<apache::thrift::concurrency::Runnable>) override {}
  std::shared_ptr<apache::thrift::concurrency::Runnable> removeNextPending()
      override {
    return nullptr;
  }
  void clearPending() override {}
  void enableCodel(bool) override {}
  bool codelEnabled() const override { return false; }
  folly::Codel* getCodel() override { return nullptr; }
  void setExpireCallback(ExpireCallback) override {}
  void setCodelCallback(ExpireCallback) override {}
  void setThreadInitCallback(InitCallback) override {}
  void addTaskObserver(std::shared_ptr<Observer>) override {}
  std::chrono::nanoseconds getUsedCpuTime() const override {
    return std::chrono::nanoseconds();
  }

 private:
  std::shared_ptr<folly::Executor> executor_;
};

} // namespace press_server
