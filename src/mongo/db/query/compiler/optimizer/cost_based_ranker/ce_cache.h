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

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_hasher.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"

#include <absl/container/flat_hash_map.h>

namespace mongo::cost_based_ranker {

template <bool EnableLogging = true>
class CECache {
public:
    CECache();
    ~CECache();

    /**
     * Call getOrComputeInternal() and optionally take over ownership of node if it was stored in
     * the cache.
     */
    template <typename CEFunc>
    CardinalityEstimate getOrCompute(std::unique_ptr<MatchExpression> node, CEFunc&& ceFn) {
        auto [ce, takeOwnership] = getOrComputeInternal(node.get(), ceFn);
        if (takeOwnership) {
            _ownedExpressions.push_back(std::move(node));
        }
        return ce;
    }

    /**
     * Call getOrComputeInternal() and do not assume ownership of node.
     */
    template <typename CEFunc>
    CardinalityEstimate getOrCompute(const MatchExpression* node, CEFunc&& ceFn) {
        return getOrComputeInternal(node, ceFn).first;
    }

    /**
     * Call getOrComputeInternal() and optionally take over ownership of node if it was stored in
     * the cache.
     */
    template <typename CEFunc>
    CardinalityEstimate getOrCompute(std::unique_ptr<IndexBounds> node, CEFunc&& ceFn) {
        auto [ce, takeOwnership] = getOrComputeInternal(node.get(), ceFn);
        if (takeOwnership) {
            _ownedIntervals.push_back(std::move(node));
        }
        return ce;
    }

    /**
     * Call getOrComputeInternal() and do not assume ownership of node.
     */
    template <typename CEFunc>
    CardinalityEstimate getOrCompute(const IndexBounds* node, CEFunc&& ceFn) {
        return getOrComputeInternal(node, ceFn).first;
    }

private:
    /**
     * Get the CE of a MatchExpression 'node' from the cache or compute it via 'ceFn'.
     *
     * Return a pair of the CE and a boolean indicating whether ownership of 'node' should be taken
     * by the caller.
     */
    template <typename CEFunc>
    std::pair<CardinalityEstimate, bool> getOrComputeInternal(const MatchExpression* node,
                                                              CEFunc&& ceFn) {
        if (auto it = _exprCache.find(node); it != _exprCache.end()) {
            ++_meHits;
            log("Cache hit for node:  ", node->toString(), ", CE: ", it->second.toString());
            return {it->second, false};
        }

        ++_meMisses;
        auto ce = ceFn();
        log("Cache miss for node: ", node->toString(), ", CE: ", ce.toString());
        _exprCache.emplace(node, ce);
        return {ce, true};
    }

    /**
     * Get the CE of an IndexBounds 'node' from the cache or compute it via 'ceFn'.
     *
     * Return a pair of the CE and a boolean indicating whether ownership of 'node' should be taken
     * by the caller.
     */
    template <typename CEFunc>
    std::pair<CardinalityEstimate, bool> getOrComputeInternal(const IndexBounds* node,
                                                              CEFunc&& ceFn) {
        auto it = std::find_if(_intervalCache.begin(), _intervalCache.end(), [&](const auto& pair) {
            return *pair.first == *node;
        });

        if (it != _intervalCache.end()) {
            ++_ibHits;
            log("Cache hit for node:  ", node->toString(false), ", CE: ", it->second.toString());
            return {it->second, false};
        }

        ++_ibMisses;
        auto ce = ceFn();
        log("Cache miss for node: ", node->toString(false), ", CE: ", ce.toString());
        _intervalCache.emplace_back(node, ce);
        return {ce, true};
    }

    // Cache CEs of equivalent MatchExpressions.
    absl::flat_hash_map<const MatchExpression*,
                        CardinalityEstimate,
                        MatchExpressionHasher,
                        MatchExpressionEq>
        _exprCache;

    // Cache the number of index key CEs of IndexBounds. Currently this cache is used only for
    // estimates of number of keys and not for RID estimates. This is so because (a) in most cases
    // RID estimates of intervals are done by converting the intervals to MatchExpressions, so those
    // intervals do not end up here, and (b) if we are to store here also estimates of RIDs, this
    // would make the cache more complicated because we would have to distinguish between the two
    // (number of keys vs RIDs estimate).
    std::vector<std::pair<const IndexBounds*, CardinalityEstimate>> _intervalCache;

    // Some expressions and intervals (jointly nodes) passed to the cache are not owned long enough
    // by the caller to be used by the cache during the optimization of a query. Such nodes are the
    // ones created during CBR's operation. They are not part of any query plan, and normally are
    // freed when their scope goes away. If the cache decides to store any of these nodes, it takes
    // over their ownership.
    std::vector<std::unique_ptr<MatchExpression>> _ownedExpressions;
    std::vector<std::unique_ptr<IndexBounds>> _ownedIntervals;

    // Counters for cache hits/misses for MatchExpressions (me) and IndexBounds (ib)
    size_t _meHits = 0;
    size_t _meMisses = 0;
    size_t _ibHits = 0;
    size_t _ibMisses = 0;

    // Compile-time optimized logging depending on the EnableLogging template parameter.
    template <typename... Args>
    void log(Args&&... args) {
        if constexpr (EnableLogging) {
            (_logStream << ... << args) << std::endl;
        }
    }

    std::conditional_t<EnableLogging, std::ostringstream, int> _logStream{};
};

// Explicit instantiation declarations
extern template class CECache<true>;
extern template class CECache<false>;

}  // namespace mongo::cost_based_ranker
