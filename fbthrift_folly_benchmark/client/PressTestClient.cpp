// PressTestClient.cpp — clean version (no stats functions)
#include <sys/prctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <random>
#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/io/AsyncIoUringSocket.h>
#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <folly/experimental/io/IoUringBackend.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/synchronization/Baton.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/async/HeaderClientChannel.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>
#include "gen-cpp2/PerfTestService.h"
#include "util/LatencyStats.h"
#include "util/PacedSchedule.h"
#include "util/ResourceMonitor.h"
#include <thrift/lib/cpp2/gen/client_cpp.h>

DEFINE_string(host, "127.0.0.1", "Server host");
DEFINE_int32(port, 8002, "Server port");
DEFINE_string(transport, "rocket", "Transport protocol: rocket, header, http2");
DEFINE_int32(queue_depth, 1, "In-flight requests per connection");
DEFINE_int32(expected_qps, 0, "Expected QPS (0 = unlimited)");
DEFINE_int32(payload_size, -1, "Payload size (<0 = scan 1~1024)");
DEFINE_bool(echo_attachment, false, "Echo payload back");
DEFINE_int32(rpc_timeout_ms, 2000, "RPC timeout in ms");
DEFINE_int32(test_seconds, 20, "Test duration");
DEFINE_string(method, "test", "RPC method: test | testEb (thread='eb' on the server)");
DEFINE_int32(warmup_seconds, 0, "Requests sent in the first N seconds are not recorded in latency stats");
DEFINE_int32(test_iterations, 0, "Test iterations (0 = time-based)");
DEFINE_string(compression, "none", "Compression algorithm: none, zlib, zstd");
DEFINE_int32(connections, 100, "Total number of connections");

DEFINE_string(mode, "close_loop", "Test mode: close_loop or open_loop");
DEFINE_int32(open_loop_inflight, 1000, "Max in-flight requests for open_loop mode (global limit)");
DEFINE_int32(io_threads, 10, "Number of IO threads (EventBase count)");
DEFINE_int32(cpu_threads, 0, "Number of CPU worker threads (0 = use hardware_concurrency)");
DEFINE_bool(verbose_rpc_error, false, "Print RPC error details on failure");
DEFINE_string(fields, "all", "Request fields: all | none | comma-list");
DEFINE_bool(random_fields, false, "Use random values for test fields");
DEFINE_int64(random_seed, 0, "Random seed (0=truly random, non-zero=reproducible)");
DEFINE_int32(str_val_size, 32, "Random string size in bytes");
DEFINE_int32(int_list_size, 5, "Random int_list element count");
DEFINE_int32(str_list_size, 5, "Random str_list element count");
DEFINE_int32(kv_map_size, 5, "Random kv_map pair count");
DEFINE_bool(use_io_uring, false, "Use IoUringBackend + AsyncIoUringSocket for client IO");

using PerfTestClient = apache::thrift::Client<press_test::PerfTestService>;
static std::atomic<uint64_t> g_client_last_update_time{0};

// Latency histogram: 1us buckets up to 100ms (larger values land in the overflow bucket).
constexpr int64_t kLatencyBucketUs = 1;
constexpr int64_t kLatencyMaxUs = 100000;

// Per-CPU-core stats (no lock needed, each callback thread writes to its own)
struct alignas(64) ThreadStats {
    util::HistogramStats<int64_t> latency{kLatencyBucketUs, 0, kLatencyMaxUs};
    util::HistogramStats<double>   server_cpu_usage{1, 0.0, 30000.0};
    util::HistogramStats<double>   client_cpu_usage{1, 0.0, 30000.0};
    util::HistogramStats<double>   server_mem_usage{1, 0.0, 100.0};
    util::HistogramStats<double>   client_mem_usage{1, 0.0, 100.0};
    std::atomic<uint64_t> success{0};
    std::atomic<uint64_t> fail{0};
};

static std::vector<std::unique_ptr<ThreadStats>> g_stats_per_cpu;
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_total_bytes{0};
std::atomic<uint64_t> g_total_cnt{0};

static std::atomic<int> g_open_loop_inflight{0};
static std::atomic<int64_t> g_request_budget{0};

std::mutex g_token_mtx;
std::condition_variable g_token_cv;
int64_t g_token{0};
// Largest send backlog a paced open-loop sender had to catch up at once.
static std::atomic<int64_t> g_max_burst{0};

int64_t monotonic_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static bool after_warmup(int64_t start_time, int64_t send_time) {
  return send_time - start_time >= FLAGS_warmup_seconds * 1000000LL;
}

static folly::SemiFuture<press_test::PerfTestResponse> send_rpc(
    PerfTestClient& client, const press_test::Request& req) {
  return FLAGS_method == "testEb" ? client.semifuture_testEb(req)
                                  : client.semifuture_test(req);
}

std::string generate_payload(int size) {
    std::string payload;
    payload.resize(size);
    thread_local std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 0; i < size; ++i) {
        payload[i] = static_cast<char>(dist(rng));
    }
    return payload;
}

void token_thread(int expected_qps) {
  if (expected_qps <= 0) return;
  auto start = std::chrono::steady_clock::now();
  int64_t accumulative = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    int64_t expected = static_cast<int64_t>(expected_qps) * elapsed_us / 1000000;
    if (accumulative < expected) {
      int64_t delta = expected - accumulative;
      {
        std::lock_guard<std::mutex> lock(g_token_mtx);
        g_token += delta;
        accumulative += delta;
      }
      g_token_cv.notify_all();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::optional<apache::thrift::CompressionConfig> getCompressionConfig(
    const std::string& algo) {
  if (algo == "none" || algo.empty()) return std::nullopt;
  apache::thrift::CompressionConfig config;
  auto& codec = config.codecConfig().ensure();
  if (algo == "zstd") { codec.set_zstdConfig(); }
  else if (algo == "zlib") { codec.set_zlibConfig(); }
  else { LOG(FATAL) << "Unsupported compression algorithm: " << algo; }
  return config;
}

// ============ Protocol Factory ============
std::unique_ptr<PerfTestClient> newPerfTestClient(
    folly::EventBase* evb,
    const folly::SocketAddress& addr,
    const std::string& transport) {
  auto socket = folly::AsyncSocket::newSocket(evb, addr);
  std::unique_ptr<folly::AsyncTransport, folly::DelayedDestruction::Destructor> tsock;
  if (FLAGS_use_io_uring) {
    tsock = folly::AsyncIoUringSocket::UniquePtr(new folly::AsyncIoUringSocket(std::move(socket)));
  } else {
    tsock = std::move(socket);
  }
  if (transport == "rocket") {
    auto channel = apache::thrift::RocketClientChannel::newChannel(std::move(tsock));
    if (FLAGS_rpc_timeout_ms > 0) channel->setTimeout(FLAGS_rpc_timeout_ms);
    if (auto config = getCompressionConfig(FLAGS_compression))
      channel->setDesiredCompressionConfig(*config);
    return std::make_unique<PerfTestClient>(std::move(channel));
  } else if (transport == "header") {
    auto channel = apache::thrift::HeaderClientChannel::newChannel(std::move(tsock));
    if (FLAGS_rpc_timeout_ms > 0) channel->setTimeout(FLAGS_rpc_timeout_ms);
    if (auto config = getCompressionConfig(FLAGS_compression))
      channel->setDesiredCompressionConfig(*config);
    return std::make_unique<PerfTestClient>(std::move(channel));
  }
  LOG(FATAL) << "Unknown transport: " << transport;
  return nullptr;
}

static thread_local int g_stats_idx = -1;
static int get_stats_idx() {
    if (g_stats_idx < 0) {
        static std::atomic<int> next_idx{0};
        g_stats_idx = next_idx.fetch_add(1) % static_cast<int>(g_stats_per_cpu.size());
    }
    return g_stats_idx;
}

struct ConnContext {
    folly::EventBase* evb;
    std::unique_ptr<PerfTestClient> client;
    int evb_idx;
};

struct BenchmarkState {
    std::vector<std::unique_ptr<ConnContext>> ctxs;
    press_test::Request request;
    uint64_t start_time{0};
    std::atomic<int> total_inflight{0};
    std::atomic<uint64_t> total_processed{0};
    std::vector<std::vector<int>> conns_per_evb;
    std::atomic<int> active_senders{0};
};

// ============ Close-Loop Mode ============
// Thread-local RNG for random fields
static thread_local std::mt19937_64 rng_ = []() {
    if (FLAGS_random_seed != 0) return std::mt19937_64(static_cast<uint64_t>(FLAGS_random_seed));
    std::random_device rd; return std::mt19937_64(rd());
}();
// Generate i32 whose CompactProtocol size is exactly bytes (1..5)
static int32_t randI32ByBytes(int bytes) {
    uint64_t lo = (bytes == 1) ? 0 : (1ULL << (7 * (bytes - 1)));
    uint64_t hi = (bytes == 5) ? 0xFFFFFFFFULL : ((1ULL << (7 * bytes)) - 1);
    uint64_t u = lo + (rng_() % (hi - lo + 1));
    return (u & 1) ? -(int32_t)((u + 1) >> 1) : (int32_t)(u >> 1);
}
// Generate i64 whose CompactProtocol size is exactly bytes (1..10)
static int64_t randI64ByBytes(int bytes) {
    uint64_t lo = (bytes == 1) ? 0 : (1ULL << (7 * (bytes - 1)));
    uint64_t hi = (bytes == 10) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (7 * bytes)) - 1);
    uint64_t u = lo + (rng_() % (hi - lo + 1));
    return (u & 1) ? -(int64_t)((u + 1) >> 1) : (int64_t)(u >> 1);
}
static std::string randomStr(size_t len) {
    static const char cs[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string s(len, 0);
    for (size_t i = 0; i < len; ++i) s[i] = cs[std::uniform_int_distribution<size_t>(0, 61)(rng_)];
    return s;
}
void fillTestFields(press_test::Request& req) {
    auto& f = FLAGS_fields; if (f == "none") return;
    bool all = (f == "all"), rnd = FLAGS_random_fields;
    auto on = [&](const char* n) { return all || f.find(n) != std::string::npos; };
    if (on("int_val"))     req.int_val()    = rnd ? randI32ByBytes(std::uniform_int_distribution<int>(1, 5)(rng_)) : 42;
    if (on("long_val"))    req.long_val()   = rnd ? randI64ByBytes(std::uniform_int_distribution<int>(1, 10)(rng_)) : 12345678901234LL;
    if (on("double_val"))  req.double_val() = rnd ? std::uniform_real_distribution<double>(-1000,1000)(rng_) : 3.14159;
    if (on("str_val"))     req.str_val()    = rnd ? randomStr(FLAGS_str_val_size) : "hello_thrift";
    if (on("int_list")) {
        if (rnd) { std::vector<int32_t> v(FLAGS_int_list_size); for (auto& x:v) x=randI32ByBytes(std::uniform_int_distribution<int>(1, 5)(rng_)); req.int_list() = v; }
        else req.int_list() = {1,2,3,4,5};
    }
    if (on("str_list")) {
        if (rnd) { std::vector<std::string> v(FLAGS_str_list_size); for (auto& x:v) x=randomStr(std::uniform_int_distribution<size_t>(4,32)(rng_)); req.str_list() = v; }
        else req.str_list() = {"a","bb","ccc"};
    }
    if (on("str_set")) {
        if (rnd) { std::set<std::string> s; for (int i=0;i<FLAGS_str_list_size;++i) s.insert(randomStr(std::uniform_int_distribution<size_t>(4,32)(rng_))); req.str_set() = s; }
        else req.str_set() = {"alpha","beta","gamma"};
    }
    if (on("kv_map")) {
        if (rnd) { std::map<std::string,int64_t> m; for (int i=0;i<FLAGS_kv_map_size;++i) m[randomStr(std::uniform_int_distribution<size_t>(4,16)(rng_))]=std::uniform_int_distribution<int64_t>()(rng_); req.kv_map() = m; }
        else req.kv_map() = {{"key1",100},{"key2",200}};
    }
}

// Adjust payload to keep total serialized size close to target
void adjustPayloadToTarget(press_test::Request& req, int targetSize) {
    // Measure overhead from non-payload fields
    press_test::Request probe = req;
    probe.payload() = std::string(0, ' ');
    folly::IOBufQueue q(folly::IOBufQueue::cacheChainLength());
    apache::thrift::CompactProtocolWriter w;
    w.setOutput(&q);
    probe.write(&w);
    int overhead = q.chainLength();
    int newPayload = targetSize - overhead;
    if (newPayload > 0 && newPayload < targetSize) {
        req.payload() = std::string(newPayload, 'x');
    }
}


std::shared_ptr<BenchmarkState> run_benchmark_close_loop(
    const folly::SocketAddress& addr,
    const std::string& payload,
    const std::vector<folly::EventBase*>& evb_list,
    folly::Executor* cpu_executor) {

    int num_connections = FLAGS_connections;
    int num_evbs = static_cast<int>(evb_list.size());
    auto state = std::make_shared<BenchmarkState>();

    for (int i = 0; i < num_connections; ++i) {
        auto ctx = std::make_unique<ConnContext>();
        ctx->evb_idx = i % num_evbs;
        ctx->evb = evb_list[ctx->evb_idx];
        folly::Baton<> baton;
        std::unique_ptr<PerfTestClient> client;
        ctx->evb->runInEventBaseThread([&]() {
            client = newPerfTestClient(ctx->evb, addr, FLAGS_transport);
            baton.post();
        });
        baton.wait();
        ctx->client = std::move(client);
        state->ctxs.push_back(std::move(ctx));
    }

    state->request.echo_attachment() = FLAGS_echo_attachment;
    state->request.payload() = payload;
    fillTestFields(state->request);
    state->start_time = monotonic_us();

    auto send_request = std::make_shared<std::function<void(int)>>();
    *send_request = [state, send_request, cpu_executor](int idx) {
        auto& ctx = *state->ctxs[idx];
        if (g_stop.load(std::memory_order_acquire)) return;

        if (FLAGS_expected_qps > 0) {
            std::unique_lock<std::mutex> lock(g_token_mtx);
            if (g_token <= 0) {
                lock.unlock();
                ctx.evb->runInEventBaseThread([idx, send_request, evb = ctx.evb]() {
                    evb->runAfterDelay([idx, send_request]() { (*send_request)(idx); }, 1);
                });
                return;
            }
            g_token--;
        }

        if (g_stop.load(std::memory_order_acquire)) return;

        state->total_inflight.fetch_add(1, std::memory_order_relaxed);

        // The client channel is not thread-safe: always send from its EventBase.
        ctx.evb->runInEventBaseThread([state, idx, send_request, cpu_executor]() {
        auto& ctx = *state->ctxs[idx];
        int64_t send_time = monotonic_us();
        send_rpc(*ctx.client, state->request)
            .via(cpu_executor)
            .thenTry([state, idx, send_time, send_request](folly::Try<press_test::PerfTestResponse>&& t) {
                int64_t recv_time = monotonic_us();
                int64_t latency_us = recv_time - send_time;

                int stats_idx = get_stats_idx();
                ThreadStats& stats = *g_stats_per_cpu[stats_idx];

                if (t.hasValue()) {
                    if (after_warmup(state->start_time, send_time)) stats.latency.addValue(latency_us);
                    stats.success.fetch_add(1, std::memory_order_relaxed);
                    if (t.value().cpu_usage().value() > 0)
                        stats.server_cpu_usage.addValue(t.value().cpu_usage().value());
                    if (t.value().mem_usage().value() > 0)
                        stats.server_mem_usage.addValue(t.value().mem_usage().value());
                    g_total_bytes.fetch_add(state->request.payload()->size(), std::memory_order_relaxed);
                } else {
                    stats.fail.fetch_add(1, std::memory_order_relaxed);
                    if (FLAGS_verbose_rpc_error)
                        LOG(ERROR) << "RPC failed: " << t.exception().what();
                }

                uint64_t last = g_client_last_update_time.load(std::memory_order_relaxed);
                if (recv_time > last && recv_time - last > 100000) {
                    if (g_client_last_update_time.compare_exchange_weak(last, recv_time, std::memory_order_relaxed)) {
                        auto snap = util::g_snapshot.load(std::memory_order_acquire);
                        stats.client_cpu_usage.addValue(snap.cpu_percent);
                        stats.client_mem_usage.addValue(snap.mem_percent);
                    }
                }

                g_total_cnt.fetch_add(1, std::memory_order_relaxed);
                state->total_inflight.fetch_sub(1, std::memory_order_relaxed);
                auto processed = state->total_processed.fetch_add(1, std::memory_order_relaxed) + 1;

                if (!g_stop.load(std::memory_order_acquire)) {
                    if (FLAGS_test_iterations > 0 && processed >= static_cast<uint64_t>(FLAGS_test_iterations)) {
                        g_stop.store(true, std::memory_order_release);
                    } else if (FLAGS_test_iterations == 0 && recv_time - state->start_time > FLAGS_test_seconds * 1000000LL) {
                        g_stop.store(true, std::memory_order_release);
                    } else {
                        // bypass EventBase: next send stays on CPU thread
                        (*send_request)(idx);
                    }
                }
            });
        });
    };

    for (int i = 0; i < num_connections; ++i)
        for (int j = 0; j < FLAGS_queue_depth; ++j)
            cpu_executor->add([i, send_request]() { (*send_request)(i); });

    while (!g_stop.load(std::memory_order_acquire) ||
           state->total_inflight.load(std::memory_order_acquire) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return state;
}

// Logs the first few very slow RPCs to help locate tail outliers.
static void maybe_log_slow_rpc(const BenchmarkState& state, int idx, int64_t send_time, int64_t latency_us) {
    static std::atomic<int> logged{0};
    if (latency_us > 100000 && logged.fetch_add(1, std::memory_order_relaxed) < 20) {
        LOG(WARNING) << "slow rpc: latency_us=" << latency_us
                     << " sent_at_ms=" << (send_time - static_cast<int64_t>(state.start_time)) / 1000
                     << " conn=" << idx;
    }
}

// Sends one open-loop request on connection `idx`; must run on that
// connection's EventBase. Returns false when the in-flight cap, the request
// budget or the stop flag prevents sending.
static bool open_loop_send(const std::shared_ptr<BenchmarkState>& state, int idx, folly::Executor* cpu_executor) {
    if (g_open_loop_inflight.load(std::memory_order_relaxed) >= FLAGS_open_loop_inflight) return false;
    int current = g_open_loop_inflight.fetch_add(1, std::memory_order_relaxed);
    if (current >= FLAGS_open_loop_inflight) { g_open_loop_inflight.fetch_sub(1, std::memory_order_relaxed); return false; }

    if (FLAGS_test_iterations > 0) {
        int64_t budget = g_request_budget.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (budget < 0) { g_request_budget.fetch_add(1, std::memory_order_relaxed); g_open_loop_inflight.fetch_sub(1, std::memory_order_relaxed); return false; }
    }

    if (g_stop.load(std::memory_order_acquire)) { g_open_loop_inflight.fetch_sub(1, std::memory_order_relaxed); if (FLAGS_test_iterations > 0) g_request_budget.fetch_add(1, std::memory_order_relaxed); return false; }

    state->total_inflight.fetch_add(1, std::memory_order_relaxed);
    int64_t send_time = monotonic_us();
    auto& ctx = *state->ctxs[idx];
    send_rpc(*ctx.client, state->request)
        .via(cpu_executor)
        .thenTry([state, idx, send_time](folly::Try<press_test::PerfTestResponse>&& t) {
            int64_t recv_time = monotonic_us();
            int64_t latency_us = recv_time - send_time;
            int stats_idx = get_stats_idx();
            ThreadStats& stats = *g_stats_per_cpu[stats_idx];

            if (t.hasValue()) {
                if (after_warmup(state->start_time, send_time)) {
                    stats.latency.addValue(latency_us);
                    maybe_log_slow_rpc(*state, idx, send_time, latency_us);
                }
                stats.success.fetch_add(1, std::memory_order_relaxed);
                if (t.value().cpu_usage().value() > 0) stats.server_cpu_usage.addValue(t.value().cpu_usage().value());
                if (t.value().mem_usage().value() > 0) stats.server_mem_usage.addValue(t.value().mem_usage().value());
                g_total_bytes.fetch_add(state->request.payload()->size(), std::memory_order_relaxed);
            } else {
                stats.fail.fetch_add(1, std::memory_order_relaxed);
                if (FLAGS_verbose_rpc_error) LOG(ERROR) << "RPC failed: " << t.exception().what();
            }

            uint64_t last = g_client_last_update_time.load(std::memory_order_relaxed);
            if (recv_time > last && recv_time - last > 100000) {
                if (g_client_last_update_time.compare_exchange_weak(last, recv_time, std::memory_order_relaxed)) {
                    auto snap = util::g_snapshot.load(std::memory_order_acquire);
                    stats.client_cpu_usage.addValue(snap.cpu_percent);
                    stats.client_mem_usage.addValue(snap.mem_percent);
                }
            }

            g_total_cnt.fetch_add(1, std::memory_order_relaxed);
            g_open_loop_inflight.fetch_sub(1, std::memory_order_relaxed);
            state->total_inflight.fetch_sub(1, std::memory_order_relaxed);
            state->total_processed.fetch_add(1, std::memory_order_relaxed);
        });
    return true;
}

// Paced open-loop sender: posts sends for `conns` to their EventBases so that
// `qps` requests/s are issued evenly from state->start_time. The EventBases
// keep blocking in epoll between events; no timer-tick bursts.
static void pacer_loop(std::shared_ptr<BenchmarkState> state, std::vector<int> conns, double qps, folly::Executor* cpu_executor) {
    prctl(PR_SET_TIMERSLACK, 1UL);
    const int64_t start = static_cast<int64_t>(state->start_time);
    const int64_t end = start + FLAGS_test_seconds * 1000000LL;
    const util::PacedSchedule schedule(start, qps);
    int64_t posted = 0;
    size_t next = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
        int64_t now = monotonic_us();
        if (FLAGS_test_iterations == 0 && now >= end) { g_stop.store(true, std::memory_order_release); break; }
        if (FLAGS_test_iterations > 0 && g_request_budget.load(std::memory_order_relaxed) <= 0) break;
        int64_t due = schedule.dueCount(now);
        if (due > posted) {
            int64_t backlog = due - posted;
            int64_t prev = g_max_burst.load(std::memory_order_relaxed);
            while (backlog > prev && !g_max_burst.compare_exchange_weak(prev, backlog, std::memory_order_relaxed)) {}
            for (; posted < due; ++posted) {
                int idx = conns[next++ % conns.size()];
                state->ctxs[idx]->evb->runInEventBaseThread([state, idx, cpu_executor]() { open_loop_send(state, idx, cpu_executor); });
            }
            continue;
        }
        int64_t wait = schedule.dueTime(posted) - monotonic_us();
        if (wait > 60) std::this_thread::sleep_for(std::chrono::microseconds(wait - 50));
        else std::this_thread::yield();
    }
    state->active_senders.fetch_sub(1, std::memory_order_relaxed);
}

// ============ Open-Loop Mode ============
std::shared_ptr<BenchmarkState> run_benchmark_open_loop(
    const folly::SocketAddress& addr,
    const std::string& payload,
    const std::vector<folly::EventBase*>& evb_list,
    folly::Executor* cpu_executor) {

    int num_connections = FLAGS_connections;
    int num_evbs = static_cast<int>(evb_list.size());
    auto state = std::make_shared<BenchmarkState>();

    for (int i = 0; i < num_connections; ++i) {
        auto ctx = std::make_unique<ConnContext>();
        ctx->evb_idx = i % num_evbs;
        ctx->evb = evb_list[ctx->evb_idx];
        folly::Baton<> baton;
        std::unique_ptr<PerfTestClient> client;
        ctx->evb->runInEventBaseThread([&]() {
            client = newPerfTestClient(ctx->evb, addr, FLAGS_transport);
            baton.post();
        });
        baton.wait();
        ctx->client = std::move(client);
        state->ctxs.push_back(std::move(ctx));
    }

    state->request.echo_attachment() = FLAGS_echo_attachment;
    state->request.payload() = payload;
    fillTestFields(state->request);
    state->start_time = monotonic_us();

    if (FLAGS_test_iterations > 0)
        g_request_budget.store(FLAGS_test_iterations, std::memory_order_relaxed);

    state->conns_per_evb.resize(num_evbs);
    for (int i = 0; i < num_connections; ++i)
        state->conns_per_evb[state->ctxs[i]->evb_idx].push_back(i);

    std::vector<std::thread> pacers;
    if (FLAGS_expected_qps > 0) {
        // Paced mode: connections are split round-robin over up to 8 pacer
        // threads; each pacer owns the matching share of the target rate.
        int num_pacers = std::min(num_connections, 8);
        state->active_senders.store(num_pacers, std::memory_order_relaxed);
        for (int p = 0; p < num_pacers; ++p) {
            std::vector<int> conns;
            for (int i = p; i < num_connections; i += num_pacers) conns.push_back(i);
            double qps = static_cast<double>(FLAGS_expected_qps) * conns.size() / num_connections;
            pacers.emplace_back(pacer_loop, state, std::move(conns), qps, cpu_executor);
        }
    } else {
        // Unlimited mode: each EventBase that owns connections keeps sending
        // until the in-flight cap is reached.
        int active_evbs = 0;
        for (int e = 0; e < num_evbs; ++e)
            if (!state->conns_per_evb[e].empty()) ++active_evbs;
        state->active_senders.store(active_evbs, std::memory_order_relaxed);
        for (int e = 0; e < num_evbs; ++e) {
            if (state->conns_per_evb[e].empty()) continue;
            auto* evb = evb_list[e];
            evb->runInEventBaseThread([state, evb, evb_idx = e, cpu_executor]() {
                auto sender_loop_ptr = std::make_shared<std::function<void()>>();
                *sender_loop_ptr = [state, evb, evb_idx, cpu_executor, sender_loop_ptr, next_conn_pos = size_t(0)]() mutable {
                    if (g_stop.load(std::memory_order_acquire)) { state->active_senders.fetch_sub(1); return; }
                    if (FLAGS_test_iterations == 0 && monotonic_us() - state->start_time > FLAGS_test_seconds * 1000000LL) {
                        g_stop.store(true, std::memory_order_release);
                        state->active_senders.fetch_sub(1); return;
                    }
                    if (FLAGS_test_iterations > 0 && g_request_budget.load(std::memory_order_relaxed) <= 0) {
                        state->active_senders.fetch_sub(1); return;
                    }
                    auto& my_conns = state->conns_per_evb[evb_idx];
                    bool sent_any = false;
                    for (int b = 0; b < 10; ++b) {
                        if (!open_loop_send(state, my_conns[next_conn_pos % my_conns.size()], cpu_executor)) break;
                        ++next_conn_pos;
                        sent_any = true;
                    }
                    if (sent_any) evb->runInEventBaseThread([sender_loop_ptr]() { (*sender_loop_ptr)(); });
                    else evb->runAfterDelay([sender_loop_ptr]() { (*sender_loop_ptr)(); }, 1);
                };
                (*sender_loop_ptr)();
            });
        }
    }

    while (!g_stop.load(std::memory_order_acquire) ||
           state->total_inflight.load(std::memory_order_acquire) > 0 ||
           state->active_senders.load(std::memory_order_acquire) > 0) {
        if (FLAGS_test_iterations > 0 && g_request_budget.load(std::memory_order_relaxed) <= 0 &&
            state->total_inflight.load(std::memory_order_acquire) == 0) {
            g_stop.store(true, std::memory_order_release); break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (auto& t : pacers) t.join();
    return state;
}

std::shared_ptr<BenchmarkState> run_benchmark(
    const folly::SocketAddress& addr, const std::string& payload,
    const std::vector<folly::EventBase*>& evb_list, folly::Executor* cpu_executor) {
    if (FLAGS_mode == "open_loop")
        return run_benchmark_open_loop(addr, payload, evb_list, cpu_executor);
    return run_benchmark_close_loop(addr, payload, evb_list, cpu_executor);
}

// ============ Single Test Run ============
void test_once(int payload_size) {
  g_open_loop_inflight.store(0, std::memory_order_relaxed);
  g_request_budget.store(0, std::memory_order_relaxed);

  std::string payload = generate_payload(payload_size);

  int io_thread_num = FLAGS_io_threads > 0 ? FLAGS_io_threads : 10;
  int cpu_thread_num = FLAGS_cpu_threads > 0 ? FLAGS_cpu_threads : static_cast<int>(std::thread::hardware_concurrency());

  LOG(INFO) << "[Mode: " << FLAGS_mode
            << ", IOThreads: " << io_thread_num
            << ", CPUThreads: " << cpu_thread_num
            << ", Connections: " << FLAGS_connections
            << ", Depth: " << (FLAGS_mode == "open_loop" ? FLAGS_open_loop_inflight : FLAGS_queue_depth)
            << ", Payload: " << payload_size << "B"
            << ", Transport: " << FLAGS_transport
            << ", QPS: " << (FLAGS_expected_qps > 0 ? std::to_string(FLAGS_expected_qps) : "unlimited")
            << ", Duration: " << FLAGS_test_seconds << "s"
            << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
            << "]" ;

  g_stop.store(false, std::memory_order_release);
  g_total_bytes.store(0, std::memory_order_relaxed);
  g_total_cnt.store(0, std::memory_order_relaxed);

  { std::lock_guard<std::mutex> lock(g_token_mtx); g_token = 0; }
  g_max_burst.store(0, std::memory_order_relaxed);

  g_stats_per_cpu.clear();
  for (int i = 0; i < cpu_thread_num; ++i)
      g_stats_per_cpu.push_back(std::make_unique<ThreadStats>());

  folly::SocketAddress addr(FLAGS_host, FLAGS_port);
  std::vector<folly::EventBase*> evbs;
  std::unique_ptr<folly::EventBaseManager> ebm;
  if (FLAGS_use_io_uring) {
    folly::EventBaseBackendBase::FactoryFunc bf = []() {
      folly::IoUringBackend::Options opts;
      opts.setCapacity(65536).setMaxSubmit(4096).setInitialProvidedBuffers(4096, 1024);
      return std::unique_ptr<folly::EventBaseBackendBase>(new folly::IoUringBackend(opts));
    };
    ebm = std::make_unique<folly::EventBaseManager>(bf);
  } else {
    ebm = std::make_unique<folly::EventBaseManager>();
  }
  auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(io_thread_num, std::make_shared<folly::NamedThreadFactory>("IOThreadPool"), ebm.get());
  for (int i = 0; i < io_thread_num; ++i)
      evbs.push_back(io_executor->getEventBase());
  auto cpu_executor = std::make_shared<folly::CPUThreadPoolExecutor>(cpu_thread_num);

  std::thread token_td;
  if (FLAGS_expected_qps > 0)
    token_td = std::thread(token_thread, FLAGS_expected_qps);

  auto test_start = std::chrono::steady_clock::now();
  auto state = run_benchmark(addr, payload, evbs, cpu_executor.get());

  g_stop.store(true, std::memory_order_release);
  g_token_cv.notify_all();
  if (token_td.joinable()) token_td.join();

  cpu_executor->stop();
  cpu_executor->join();

  for (auto& ctx : state->ctxs) {
      folly::Baton<> baton;
      auto* evb = ctx->evb;
      auto client_ptr = ctx->client.release();
      evb->runInEventBaseThread([client_ptr, &baton]() { delete client_ptr; baton.post(); });
      baton.wait();
  }
  state->ctxs.clear();
  evbs.clear();
  io_executor->stop();
  io_executor->join();

  auto test_end = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(test_end - test_start).count();
  // double elapsed_sec = FLAGS_test_seconds;
  double elapsed_sec = elapsed_ms / 1000.0;

  util::HistogramStats<int64_t> total_latency(kLatencyBucketUs, 0, kLatencyMaxUs);
  util::HistogramStats<double> total_server_cpu(1, 0.0, 30000.0);
  util::HistogramStats<double> total_client_cpu(1, 0.0, 30000.0);
  util::HistogramStats<double> total_server_mem(1, 0.0, 100.0);
  util::HistogramStats<double> total_client_mem(1, 0.0, 100.0);
  uint64_t total_success = 0;
  uint64_t total_fail = 0;

  for (auto& stats_ptr : g_stats_per_cpu) {
      ThreadStats& stats = *stats_ptr;
      total_latency.merge(stats.latency);
      total_server_cpu.merge(stats.server_cpu_usage);
      total_client_cpu.merge(stats.client_cpu_usage);
      total_server_mem.merge(stats.server_mem_usage);
      total_client_mem.merge(stats.client_mem_usage);
      total_success += stats.success.load();
      total_fail += stats.fail.load();
  }

  uint64_t total_cnt = total_success + total_fail;
  double throughput_mbps = (g_total_bytes.load() / 1048576.0) / elapsed_sec;

  LOG(INFO) << "====== Benchmark Results =======";
  LOG(INFO) << "Mode: " << FLAGS_mode;
  LOG(INFO) << "Total duration: " << std::fixed << std::setprecision(3) << elapsed_sec << "s";
  LOG(INFO) << "Total requests: " << total_cnt;
  LOG(INFO) << "Success: " << total_success;
  LOG(INFO) << "Fail: " << total_fail;
  LOG(INFO) << "QPS: " << std::fixed << std::setprecision(2) << (total_success / elapsed_sec);
  LOG(INFO) << "Throughput: " << std::fixed << std::setprecision(2) << throughput_mbps << "MB/s";

  if (total_success > 0) {
    LOG(INFO) << "Latency (us):"
              << " avg=" << std::fixed << std::setprecision(2) << total_latency.getAvg()
              << " min=" << total_latency.getMin()
              << " max=" << total_latency.getMax()
              << " p50=" << total_latency.getPercentile(0.50)
              << " p90=" << total_latency.getPercentile(0.90)
              << " p99=" << total_latency.getPercentile(0.99)
              << " p999=" << total_latency.getPercentile(0.999);

    LOG(INFO) << "Server CPU (%):"
              << " avg=" << std::fixed << std::setprecision(2) << total_server_cpu.getAvg()
              << " min=" << total_server_cpu.getMin()
              << " max=" << total_server_cpu.getMax()
              << " p50=" << total_server_cpu.getPercentile(0.50)
              << " p90=" << total_server_cpu.getPercentile(0.90)
              << " p99=" << total_server_cpu.getPercentile(0.99)
              << " p999=" << total_server_cpu.getPercentile(0.999);

    LOG(INFO) << "Client CPU (%):"
              << " avg=" << std::fixed << std::setprecision(2) << total_client_cpu.getAvg()
              << " min=" << total_client_cpu.getMin()
              << " max=" << total_client_cpu.getMax()
              << " p50=" << total_client_cpu.getPercentile(0.50)
              << " p90=" << total_client_cpu.getPercentile(0.90)
              << " p99=" << total_client_cpu.getPercentile(0.99)
              << " p999=" << total_client_cpu.getPercentile(0.999);

    LOG(INFO) << "Server Memory (%):"
              << " avg=" << std::fixed << std::setprecision(2) << total_server_mem.getAvg()
              << " min=" << total_server_mem.getMin()
              << " max=" << total_server_mem.getMax()
              << " p50=" << total_server_mem.getPercentile(0.50)
              << " p90=" << total_server_mem.getPercentile(0.90)
              << " p99=" << total_server_mem.getPercentile(0.99)
              << " p999=" << total_server_mem.getPercentile(0.999);

    LOG(INFO) << "Client Memory (%):"
              << " avg=" << std::fixed << std::setprecision(2) << total_client_mem.getAvg()
              << " min=" << total_client_mem.getMin()
              << " max=" << total_client_mem.getMax()
              << " p50=" << total_client_mem.getPercentile(0.50)
              << " p90=" << total_client_mem.getPercentile(0.90)
              << " p99=" << total_client_mem.getPercentile(0.99)
              << " p999=" << total_client_mem.getPercentile(0.999);
  }

  LOG(INFO) << "================================";

  // One machine-readable line for the experiment scripts (key=value).
  const double window_sec = FLAGS_test_iterations == 0 ? FLAGS_test_seconds : elapsed_sec;
  std::cout << "RESULT qps=" << std::fixed << std::setprecision(2) << (total_success / window_sec)
            << " p50=" << total_latency.getPercentile(0.50)
            << " p90=" << total_latency.getPercentile(0.90)
            << " p99=" << total_latency.getPercentile(0.99)
            << " p999=" << total_latency.getPercentile(0.999)
            << " max=" << total_latency.getMax()
            << " fail=" << total_fail
            << " max_burst=" << g_max_burst.load(std::memory_order_relaxed)
            << " client_cpu_avg=" << total_client_cpu.getAvg() << std::endl;
}

// ============ main ============
int main(int argc, char* argv[]) {
  FLAGS_logtostderr = true;
  const folly::Init init(&argc, &argv);
  util::init_resource_collector();

  if (FLAGS_payload_size >= 0) {
    test_once(FLAGS_payload_size);
  } else {
    for (int i = 1; i <= 1024; i *= 4) {
      test_once(i);
    }
  }
  return 0;
}
