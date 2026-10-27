#pragma once

#include <cstdint>
#include <algorithm>
#include <limits>
#include <folly/stats/Histogram.h>

namespace util {

template <typename T>
class HistogramStats {
public:
    explicit HistogramStats(T bucketSize, T min, T max)
        : hist_(bucketSize, min, max), sum_(0), count_(0),
          actual_min_(std::numeric_limits<T>::max()),
          actual_max_(std::numeric_limits<T>::lowest()) {}

    void addValue(T val) {
        hist_.addValue(val);
        sum_ += val;
        ++count_;
        if (count_ == 1) {
            actual_min_ = val;
            actual_max_ = val;
        } else {
            actual_min_ = std::min(actual_min_, val);
            actual_max_ = std::max(actual_max_, val);
        }
    }

    void merge(const HistogramStats& other) {
        if (other.count_ == 0) return;
        hist_.merge(other.hist_);
        sum_ += other.sum_;
        count_ += other.count_;
        if (other.count_ > 0) {
            if (count_ == other.count_) {
                // 合并前自身为空，直接继承对方的 min/max
                actual_min_ = other.actual_min_;
                actual_max_ = other.actual_max_;
            } else {
                actual_min_ = std::min(actual_min_, other.actual_min_);
                actual_max_ = std::max(actual_max_, other.actual_max_);
            }
        }
    }

    uint64_t getCount() const { return count_; }
    T getSum() const { return sum_; }

    double getAvg() const {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

    // 返回实际数据中的最小值，无数据时返回 0
    T getMin() const {
        return count_ ? actual_min_ : T{};
    }

    // 返回实际数据中的最大值，无数据时返回 0
    T getMax() const {
        return count_ ? actual_max_ : T{};
    }

    T getPercentile(double p) const {
        if (count_ == 0) return T{};
        p = std::max(0.0, std::min(1.0, p));
        return hist_.getPercentileEstimate(p);
    }

    void clear() {
        hist_.clear();
        sum_ = 0;
        count_ = 0;
        actual_min_ = std::numeric_limits<T>::max();
        actual_max_ = std::numeric_limits<T>::lowest();
    }

private:
    folly::Histogram<T> hist_;
    T sum_;
    uint64_t count_;
    T actual_min_;
    T actual_max_;
};

} // namespace util