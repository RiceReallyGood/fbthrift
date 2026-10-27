#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include "util/LatencyStats.h"

int main() {
    // 创建 LatencyStats 实例：桶宽 1000us（1ms），范围 0~100000us（100ms），桶数量 = (100000-0)/1000 = 100
    util::HistogramStats<int64_t> stats(1000, 0, 100000);  
    std::vector<int64_t> all_latencies;
    all_latencies.reserve(100000);

    // 模拟生成 10,000 个延迟样本（平均 500 微秒，标准差 100 微秒的正态分布）
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> dist(500.0, 100.0);

    const int kSamples = 100000;
    for (int i = 0; i < kSamples; ++i) {
        int64_t latency_us = static_cast<int64_t>(std::max(0.0, dist(gen)));
        stats.addValue(latency_us);
        all_latencies.push_back(latency_us);

        if ((i + 1) % 1000 == 0) {
            std::cout << "Processed " << (i + 1) << " samples" << std::endl;
        }
    }

    // 精确统计（使用 vector 存储所有样本，排序后计算分位数）
    std::sort(all_latencies.begin(), all_latencies.end());
    uint64_t exact_count = all_latencies.size();
    int64_t exact_sum = 0;
    for (auto v : all_latencies) exact_sum += v;
    double exact_avg = static_cast<double>(exact_sum) / exact_count;
    int64_t exact_min = all_latencies.front();
    int64_t exact_max = all_latencies.back();

    auto exact_percentile = [&](double p) -> int64_t {
        if (exact_count == 0) return 0;
        size_t idx = static_cast<size_t>(p * (exact_count - 1));
        return all_latencies[idx];
    };

    // 输出对比结果
    std::cout << "\n=== Latency Statistics Comparison ===" << std::endl;
    // 使用 printf 对齐输出，注意一共 9 列（Method, Samples, Avg(us), Min(us), Max(us), P50, P90, P99, P99.9）
    printf("%-10s %-8s %-10s %-8s %-8s %-8s %-8s %-8s %-8s\n",
           "Method", "Samples", "Avg(us)", "Min(us)", "Max(us)", "P50", "P90", "P99", "P99.9");
    printf("%-10s %-8lu %-10.2f %-8ld %-8ld %-8ld %-8ld %-8ld %-8ld\n",
           "Exact", exact_count, exact_avg, exact_min, exact_max,
           exact_percentile(0.50), exact_percentile(0.90), exact_percentile(0.99), exact_percentile(0.999));
    printf("%-10s %-8lu %-10.2f %-8ld %-8ld %-8ld %-8ld %-8ld %-8ld\n",
           "Histogram", stats.getCount(), stats.getAvg(), stats.getMin(), stats.getMax(),
           stats.getPercentile(0.50), stats.getPercentile(0.90), stats.getPercentile(0.99), stats.getPercentile(0.999));

    return 0;
}