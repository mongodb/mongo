/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/window.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/util/string_listset.h"

/**
 * Contains a set of functions for shallow estimating the size of allocated on the heap objects
 * in a given container. The functions do not take account of the size of the given container, only
 * its values allocated on the heap.
 * They are used to calculate compile-time size of an SBE tree.
 */
namespace mongo::sbe::size_estimator {

size_t estimate(const IndexBounds& indexBounds);
size_t estimate(const OrderedIntervalList& list);
size_t estimate(const Interval& interval);
size_t estimate(const IndexSeekPoint& indexSeekPoint);

inline size_t estimate(const std::unique_ptr<PlanStage>& planStage) {
    return planStage->estimateCompileTimeSize();
}

inline size_t estimate(const std::unique_ptr<EExpression>& expr) {
    return expr->estimateSize();
}

inline size_t estimate(value::TypeTags tag, value::Value val) {
    size_t size = value::getApproximateSize(tag, val);
    return std::max(static_cast<size_t>(0), size - sizeof(tag) - sizeof(val));
}

inline size_t estimate(const std::string& str) {
    return sizeof(std::string::value_type) * str.capacity();
}

inline size_t estimate(const BSONObj& bson) {
    return bson.objsize();
}

size_t estimate(const IndexBoundsChecker& checker);

// Calculate sizes of heap-allocated values only. Therefore, sizes of scalar values are always 0.
template <typename S, std::enable_if_t<std::is_scalar_v<S>, bool> = true>
inline size_t estimate(S) {
    return 0;
}

inline size_t estimate(StringData str) {
    return 0;
}

inline size_t estimate(const AggExprPair& expr) {
    size_t size = 0;
    if (expr.init) {
        size += expr.init->estimateSize();
    }
    size += expr.acc->estimateSize();
    return size;
}

inline size_t estimate(const BlockHashAggStage::BlockRowAccumulators& acc) {
    return size_estimator::estimate(acc.blockAgg) + size_estimator::estimate(acc.rowAgg);
}

inline size_t estimate(const WindowStage::Window& window) {
    size_t size = sizeof(window);
    if (window.lowBoundExpr) {
        size += size_estimator::estimate(window.lowBoundExpr);
    }
    if (window.highBoundExpr) {
        size += size_estimator::estimate(window.highBoundExpr);
    }
    for (size_t i = 0; i < window.initExprs.size(); ++i) {
        if (window.initExprs[i]) {
            size += size_estimator::estimate(window.initExprs[i]);
        }
        if (window.addExprs[i]) {
            size += size_estimator::estimate(window.addExprs[i]);
        }
        if (window.removeExprs[i]) {
            size += size_estimator::estimate(window.removeExprs[i]);
        }
    }
    return size;
}

// Calculate the size of a SpecificStats's derived class.
// We need a template argument here rather than passing const SpecificStats&
// as we need to know the exact type to properly compute the size of the object.
template <typename S, std::enable_if_t<std::is_base_of_v<SpecificStats, S>, bool> = true>
inline size_t estimate(const S& stats) {
    return stats.estimateObjectSizeInBytes() - sizeof(S);
}

template <typename A, typename B>
inline size_t estimate(const std::pair<A, B>& pair) {
    return estimate(pair.first) + estimate(pair.second);
}

// Calculate the size of the inlined vector's elements.
template <typename T, size_t N, typename A>
size_t estimate(const absl::InlinedVector<T, N, A>& vector) {
    size_t size = 0;
    // Calculate size of the value only if the values are not inlined.
    if (vector.capacity() > N) {
        size += vector.capacity() * sizeof(T);
    }

    for (const auto& elem : vector) {
        size += estimate(elem);
    }

    return size;
}

// Calculate the size of the vector's elements.
template <typename T, typename A>
size_t estimate(const std::vector<T, A>& vector) {
    size_t size = vector.capacity() * sizeof(T);

    for (const auto& elem : vector) {
        size += estimate(elem);
    }

    return size;
}

// Overload of 'estimate' function for std::vector<bool> since
// the latter is often an optimized specialization of std::vector for bool
// and it might fail to match against ordinary 'size_t estimate(const std::vector<T, A>&)'.
inline size_t estimate(const std::vector<bool>& vec) {
    return vec.capacity() * sizeof(bool);
}

template <typename T, typename A>
size_t estimateContainerOnly(const std::vector<T, A>& vector) {
    return vector.capacity() * sizeof(T);
}

template <typename K, typename V, typename... Args>
size_t estimate(const absl::flat_hash_map<K, V, Args...>& map) {
    // The estimation is based on the memory usage of absl::flat_hash_map
    // documented in https://abseil.io/docs/cpp/guides/container:
    // The container uses O((sizeof(std::pair<const K, V>) + 1) * bucket_count()) bytes.
    // The tests with a custom allocator showed that actual memory usage was
    // (sizeof(std::pair<const K, V>) + 1) * bucket_count() + C,
    // where C was equal to 17 for non-empty containers on x64 platform
    // and 0 for empty containers.
    constexpr size_t kEstimatedConstantPayload = 17;
    size_t bucketSize = sizeof(std::pair<const K, V>) + 1;
    size_t size = map.bucket_count() * bucketSize;
    size += map.empty() ? 0 : kEstimatedConstantPayload;

    for (auto&& [key, val] : map) {
        size += estimate(key);
        size += estimate(val);
    }

    return size;
}

template <class BufferAllocator>
size_t estimate(const BasicBufBuilder<BufferAllocator>& ba) {
    return static_cast<size_t>(ba.capacity());
}

inline size_t estimate(const value::MaterializedRow& row) {
    size_t size = 0;
    for (size_t idx = 0; idx < row.size(); ++idx) {
        auto [tag, val] = row.getViewOfValue(idx);
        size += estimate(tag, val);
    }
    return size;
}

inline size_t estimate(const StringListSet& vec) {
    size_t size = size_estimator::estimate(vec.getUnderlyingVector());
    size += size_estimator::estimate(vec.getUnderlyingMap());
    return size;
}
}  // namespace mongo::sbe::size_estimator
