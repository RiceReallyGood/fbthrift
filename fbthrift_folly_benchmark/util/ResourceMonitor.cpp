#include "util/ResourceMonitor.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

namespace util {
// 全局定义（仅此一份）
std::atomic<ResourceSnapshot> g_snapshot;
static constexpr int64_t COLLECT_INTERVAL_US = 100000;  // 100ms

ResourceCollector& ResourceCollector::instance() {
  static ResourceCollector inst;
  return inst;
}

ResourceCollector::ResourceCollector() : running_(false) {
  baseline_cpu_time_ = get_process_cpu_time_us();
  baseline_real_time_ = monotonic_us();
}

ResourceCollector::~ResourceCollector() { stop(); }

void ResourceCollector::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) return;
  running_ = true;
  worker_ = std::thread([this]() { this->run(); });
}

void ResourceCollector::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    running_ = false;
  }
  if (worker_.joinable()) worker_.join();
}

void ResourceCollector::update_global(const ResourceSnapshot& snap) {
  g_snapshot.store(snap, std::memory_order_release);
}

int64_t ResourceCollector::monotonic_us() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
}

void ResourceCollector::run() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) break;
    }
    auto snap = collect();
    update_global(snap);
    std::this_thread::sleep_for(std::chrono::microseconds(COLLECT_INTERVAL_US));
  }
}

ResourceSnapshot ResourceCollector::collect() {
  ResourceSnapshot snap{0.0, 0.0, 0, 0, 0};

  int64_t now_cpu = get_process_cpu_time_us();
  int64_t now_real = monotonic_us();

  int64_t delta_cpu = now_cpu - baseline_cpu_time_;
  int64_t delta_real = now_real - baseline_real_time_;

  if (delta_real > 0) {
    snap.cpu_percent = static_cast<double>(delta_cpu) / delta_real * 100.0;
  }

  baseline_cpu_time_ = now_cpu;
  baseline_real_time_ = now_real;
  snap.timestamp_us = now_real;

  int64_t rss_kb = 0, virt_kb = 0;
  if (read_proc_status(rss_kb, virt_kb)) {
    snap.rss_mb = rss_kb / 1024;
    snap.virt_mb = virt_kb / 1024;

    int64_t total_kb = get_total_memory_kb();
    if (total_kb > 0) {
      snap.mem_percent = static_cast<double>(rss_kb) / total_kb * 100.0;
    }
  }

  return snap;
}

int64_t ResourceCollector::get_process_cpu_time_us() {
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
  }
  return 0;
}

bool ResourceCollector::read_proc_status(int64_t& rss_kb, int64_t& virt_kb) {
  int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;

  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return false;
  buf[n] = '\0';

  rss_kb = 0;
  virt_kb = 0;
  char* line = buf;
  while (line) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      rss_kb = strtoull(line + 6, nullptr, 10);
    } else if (strncmp(line, "VmSize:", 7) == 0) {
      virt_kb = strtoull(line + 7, nullptr, 10);
    }
    line = strchr(line, '\n');
    if (line) line++;
  }
  return rss_kb > 0;
}

int64_t ResourceCollector::get_total_memory_kb() {
  int64_t total = 0;
  int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;

  char buf[1024];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n > 0) {
    buf[n] = '\0';
    char* p = strstr(buf, "MemTotal:");
    if (p) total = strtoull(p + 9, nullptr, 10);
  }
  return total;
}

void init_resource_collector() {
  ResourceCollector& collector = ResourceCollector::instance();
  collector.start();

  // 等第一次采集完成
  while (g_snapshot.load(std::memory_order_acquire).timestamp_us == 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(COLLECT_INTERVAL_US)); // 50ms 轮询，比 500ms 更灵敏
  }

  // 再等一个完整周期，确保第二次采集完成，delta 有效
  std::this_thread::sleep_for(std::chrono::microseconds(COLLECT_INTERVAL_US * 2)); // 1s 足够覆盖 500ms 间隔
}
} // namespace util
