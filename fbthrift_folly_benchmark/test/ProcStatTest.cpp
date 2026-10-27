#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/resource.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace std;

struct ResourceSnapshot {
  double cpu_percent = 0.0;
  double mem_percent = 0.0;
  int64_t rss_mb = 0;
  int64_t virt_mb = 0;
  int64_t timestamp_us = 0;
};

std::atomic<ResourceSnapshot> g_snapshot;

class ResourceCollector {
 public:
  static constexpr int64_t COLLECT_INTERVAL_US = 500000;  // 500ms

  static ResourceCollector& instance() {
    static ResourceCollector inst;
    return inst;
  }

  void start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    running_ = true;
    worker_ = std::thread([this]() { this->run(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) return;
      running_ = false;
    }
    if (worker_.joinable()) worker_.join();
  }

  void update_global(const ResourceSnapshot& snap) {
    g_snapshot.store(snap, std::memory_order_release);
  }

  static int64_t monotonic_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000;
  }

 private:
  ResourceCollector() : running_(false) {
    baseline_cpu_time_ = get_process_cpu_time_us();
    baseline_real_time_ = monotonic_us();
  }
  ~ResourceCollector() { stop(); }

  void run() {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) break;
      }
      auto snap = collect();
      update_global(snap);
      usleep(COLLECT_INTERVAL_US);
    }
  }

  ResourceSnapshot collect() {
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

  static int64_t get_process_cpu_time_us() {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
      return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000LL +
             (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
    }
    return 0;
  }

  static bool read_proc_status(int64_t& rss_kb, int64_t& virt_kb) {
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

  static int64_t get_total_memory_kb() {
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

  std::mutex mutex_;
  bool running_;
  std::thread worker_;
  int64_t baseline_cpu_time_;
  int64_t baseline_real_time_;
};

// ============ 全部包办的初始化 ============

void init_resource_collector() {
  ResourceCollector& collector = ResourceCollector::instance();
  
  // 1. 启动后台线程
  collector.start();

  // 2. 等第一次采集完成（timestamp 有值）
  while (g_snapshot.load(std::memory_order_acquire).timestamp_us == 0) {
    usleep(1000);
  }

  // 3. 再等一个完整采集周期 + 10% 余量，确保第二次采集完成
  //    这样 delta_cpu 和 delta_real 是基于 COLLECT_INTERVAL_US 的，有效
  usleep(ResourceCollector::COLLECT_INTERVAL_US * 2);
}

// ============ Handler / main ============

int main(int argc, char const* argv[]) {
  // auto arrs = new int[100000000];

  // 全部在 init 里完成：启动 + 等有效数据
  init_resource_collector();

  auto snap = g_snapshot.load(std::memory_order_acquire);
  std::ostringstream oss;
  oss << "cpu=" << std::fixed << std::setprecision(2) << snap.cpu_percent << "%"
      << ",mem=" << std::fixed << std::setprecision(2) << snap.mem_percent << "%"
      << ",rss=" << snap.rss_mb << "MB"
      << ",virt=" << snap.virt_mb << "MB";
  std::cout << oss.str() << std::endl;

  // delete[] arrs;
  return 0;
}