#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>


namespace util {    
struct ResourceSnapshot {
  double cpu_percent = 0.0;
  double mem_percent = 0.0;
  int64_t rss_mb = 0;
  int64_t virt_mb = 0;
  int64_t timestamp_us = 0;
};

// 声明（定义放在 .cpp 里，防止多文件 include 冲突）
extern std::atomic<ResourceSnapshot> g_snapshot;

class ResourceCollector {
 public:
  static ResourceCollector& instance();

  void start();
  void stop();

  void update_global(const ResourceSnapshot& snap);

  static int64_t monotonic_us();

 private:
  ResourceCollector();
  ~ResourceCollector();

  void run();
  ResourceSnapshot collect();

  static int64_t get_process_cpu_time_us();
  static bool read_proc_status(int64_t& rss_kb, int64_t& virt_kb);
  static int64_t get_total_memory_kb();

  std::mutex mutex_;
  bool running_;
  std::thread worker_;
  int64_t baseline_cpu_time_;
  int64_t baseline_real_time_;
};


void init_resource_collector();
} // namespace util
