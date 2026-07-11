// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_percentile.h"

#include <cmath>

namespace mongo {

void WindowFunctionPercentileCommon::add(Value value) {
    // Only add numeric values.
    if (!value.numeric()) {
        return;
    }
    const double d = value.coerceToDouble();
    // NaN violates the strict-weak-ordering required by boost::container::flat_multiset
    // and would corrupt _values, so skip it (matching AccuratePercentile::incorporate).
    if (std::isnan(d)) {
        return;
    }
    _values.insert(d);
    _memUsageTracker.add(sizeof(double));
}

void WindowFunctionPercentileCommon::remove(Value value) {
    // Only numeric values were added, so only numeric values need to be removed.
    if (!value.numeric()) {
        return;
    }
    const double d = value.coerceToDouble();
    if (std::isnan(d)) {
        return;
    }

    auto iter = _values.find(d);
    tassert(7455904,
            "Cannot remove a value not tracked by WindowFunctionPercentile",
            iter != _values.end());
    _memUsageTracker.add(-static_cast<int64_t>(sizeof(double)));
    _values.erase(iter);
}

void WindowFunctionPercentileCommon::reset() {
    _values.clear();
    // resetting _memUsageTracker is the responsibility of the derived classes.
}

Value WindowFunctionPercentileCommon::computePercentile(double p) const {
    const double n = _values.size();

    // boost::container::flat_multiset stores the values in ascending order, so we don't need to
    // sort them before finding the value at index 'rank'.
    // boost::container::flat_multiset has random-access iterators, so std::advance has an
    // expected runtime of O(1).
    if (_method != PercentileMethodEnum::kContinuous) {
        const double rank = DiscretePercentile::computeTrueRank(n, p);
        auto it = _values.begin();
        std::advance(it, static_cast<int>(rank));
        return Value(*it);
    } else {
        const double rank = ContinuousPercentile::computeTrueRank(n, p);
        const int ceil_rank = std::ceil(rank);
        const int floor_rank = std::floor(rank);
        if (ceil_rank == rank && rank == floor_rank) {
            auto it = _values.begin();
            std::advance(it, static_cast<int>(rank));
            return Value(*it);
        } else {
            auto it_ceil_rank = _values.begin();
            std::advance(it_ceil_rank, ceil_rank);
            auto it_floor_rank = _values.begin();
            std::advance(it_floor_rank, floor_rank);

            return Value(ContinuousPercentile::linearInterpolate(
                rank, ceil_rank, floor_rank, *it_ceil_rank, *it_floor_rank));
        }
    }
}

Value WindowFunctionPercentile::getValue(boost::optional<Value> current) const {
    if (_values.empty()) {
        std::vector<Value> nulls;
        nulls.insert(nulls.end(), _ps.size(), Value(BSONNULL));
        return Value(std::move(nulls));
    }
    std::vector<Value> pctls;
    pctls.reserve(_ps.size());
    for (double p : _ps) {
        auto result = WindowFunctionPercentileCommon::computePercentile(p);
        pctls.push_back(result);
    }

    return Value(std::move(pctls));
};

void WindowFunctionPercentile::reset() {
    WindowFunctionPercentileCommon::reset();
    _memUsageTracker.set(sizeof(*this) + _ps.capacity() * sizeof(double));
}

Value WindowFunctionMedian::getValue(boost::optional<Value> current) const {
    if (_values.empty())
        return Value{BSONNULL};

    return WindowFunctionPercentileCommon::computePercentile(0.5 /* p */);
}

void WindowFunctionMedian::reset() {
    WindowFunctionPercentileCommon::reset();
    _memUsageTracker.set(sizeof(*this));
}

}  // namespace mongo
