/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/pipeline/window_function/window_function_percentile.h"

#include "mongo/db/query/util/represent_as_util.h"

#include <cmath>

namespace mongo {
namespace {
// Defense-in-depth bounds check: asserts 'index' is a valid 0-based position into a collection of
// 'size' values. Both paths through computePercentile() guarantee this by construction (see call
// sites), so this tassert should never fire.
int assertRankIndex(int index, size_t size) {
    tassert(12961700,
            "percentile rank is not a valid in-bounds index into the window's values",
            index >= 0 && static_cast<size_t>(index) < size);
    return index;
}
}  // namespace

void WindowFunctionPercentileCommon::add(Value value) {
    // Only add numeric values.
    if (!value.numeric()) {
        return;
    }
    _values.insert(value.coerceToDouble());
    _memUsageTracker.add(sizeof(double));
}

void WindowFunctionPercentileCommon::remove(Value value) {
    // Only numeric values were added, so only numeric values need to be removed.
    if (!value.numeric()) {
        return;
    }

    auto iter = _values.find(value.coerceToDouble());
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
        const int rank = DiscretePercentile::computeTrueRank(n, p);
        // computeTrueRank() guarantees rank is in [0, n-1]: when p >= 1.0 it returns n-1,
        // otherwise max(0, ceil(n*p)-1). Since p < 1.0 implies n*p < n and n is an integer,
        // the result always fits in int and is a valid index.
        assertRankIndex(rank, _values.size());
        auto it = _values.begin();
        std::advance(it, rank);
        return Value(*it);
    } else {
        const double rank = ContinuousPercentile::computeTrueRank(n, p);
        // representAsChecked<int>() tasserts if rank is non-finite or out of int range.
        const int ceil_rank =
            assertRankIndex(representAsChecked<int>(std::ceil(rank)), _values.size());
        const int floor_rank =
            assertRankIndex(representAsChecked<int>(std::floor(rank)), _values.size());
        if (ceil_rank == floor_rank) {  // rank is an exact integer; no interpolation needed
            auto it = _values.begin();
            std::advance(it, ceil_rank);
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
