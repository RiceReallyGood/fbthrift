// server_pools_test: every executor-backed --pool_mode runs tasks through its
// ThreadManager, and reports the queue stats the experiment scripts parse.
#include <sched.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include "server/ServerPools.h"

static int g_failures = 0;

#define EXPECT_THAT(cond, what)                                              \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": " << what << std::endl; \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Value of `key` in a "k1=v1 k2=v2" stats line, "" if absent.
static std::string statValue(const std::string& stats, const std::string& key) {
  std::istringstream in(stats);
  std::string token;
  while (in >> token) {
    if (token.compare(0, key.size() + 1, key + "=") == 0) {
      return token.substr(key.size() + 1);
    }
  }
  return "";
}

// Distinct L3 groups (sysfs index3) among the CPUs this process may use.
static size_t distinctL3Groups() {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  std::set<std::string> groups;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &set)) {
      continue;
    }
    std::string list;
    folly::readFile(
        ("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
         "/cache/index3/shared_cpu_list")
            .c_str(),
        list);
    groups.insert(folly::rtrimWhitespace(list).str());
  }
  return groups.size();
}

static bool startsWith(const std::string& s, const std::string& prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

static void checkClusterAware2Stats(const std::string& mode, const std::string& s) {
  EXPECT_THAT(startsWith(s, "local="), mode << ": stats start with: " << s);
  EXPECT_THAT(s.find(" assign_local=") != std::string::npos, mode << ": " << s);
  std::vector<std::string> buckets;
  folly::split(',', statValue(s, "wake_remote"), buckets);
  EXPECT_THAT(buckets.size() == 11, mode << ": wake_remote has "
                                          << buckets.size() << " buckets");
  for (const auto& b : buckets) {
    EXPECT_THAT(!b.empty() && b.find_first_not_of("0123456789") == std::string::npos,
                mode << ": bucket '" << b << "'");
  }
  auto takes = std::stoull(statValue(s, "local")) + std::stoull(statValue(s, "stolen"));
  EXPECT_THAT(takes == 10000, mode << ": local + stolen = " << takes);
}

static void testModeRunsTasks(const std::string& mode) {
  std::atomic<int> done{0}; // outlives the pools, whose workers touch it
  auto pools = press_server::makeServerPools(mode, 4, 8, "", "");
  EXPECT_THAT(pools.threadManager != nullptr, mode << ": no ThreadManager");
  if (!pools.threadManager) {
    return;
  }
  for (int i = 0; i < 10000; ++i) {
    pools.threadManager->add([&] { ++done; });
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (done.load() < 10000 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_THAT(done.load() == 10000, mode << ": ran " << done.load() << " of 10000");
  std::string stats = pools.queueStats ? pools.queueStats() : "";
  if (startsWith(mode, "cluster2")) {
    checkClusterAware2Stats(mode, stats);
  }
  if (startsWith(mode, "striped")) {
    // Review focus: the stripe count comes from the process affinity, not
    // from a worker pinned to one cluster.
    auto stripes = statValue(stats, "stripes");
    EXPECT_THAT(stripes == std::to_string(distinctL3Groups()),
                mode << ": stripes=" << stripes << ", L3 groups "
                     << distinctL3Groups());
    EXPECT_THAT(!statValue(stats, "balancer_transfers").empty(), mode << ": " << stats);
  }
  std::cout << mode << ": " << stats << std::endl;
}

static void testUnknownModeThrows() {
  bool threw = false;
  try {
    press_server::makeServerPools("nope", 1, 1, "", "");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  EXPECT_THAT(threw, "unknown mode did not throw");
}

int main() {
  // As press_server: create every pool thread up front.
  FLAGS_dynamic_cputhreadpoolexecutor = false;
  for (const char* mode :
       {"cpu", "cluster", "cluster2", "cluster2_grab", "striped", "striped_pinned"}) {
    testModeRunsTasks(mode);
  }
  testUnknownModeThrows();
  if (g_failures > 0) {
    std::cerr << "FAILED (" << g_failures << ")" << std::endl;
    return 1;
  }
  std::cout << "OK" << std::endl;
  return 0;
}
