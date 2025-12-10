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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::join_ordering {

/**
 * Helper to calculate binomial coefficient (n choose k, notated C(n, k)) for all valid values of k
 * for the given value of n. This object provides an iterator-like interface to expose a sequence of
 * values of C(n, 0), C(n, 1), ... C(n, n). The purpose is to provide a fast way to calculate
 * binomial coefficient in order without performing any duplicate work.
 */
class CombinationSequence {
public:
    CombinationSequence(int n);

    /**
     * Returns C(n, k) and increments k. The initial value of k is 0. This function can only be
     * invoked at most n+1 times (C(n,n) is the final legal invocation).
     */
    uint64_t next();

private:
    // Value of n when calculating C(n, k). Stays constants for the life of this object.
    int _n;
    // Current k in the sequence. The subsequent call to next() will return C(n, k).
    int _k;
    // The number of combinations for the previous value of k. In other words, holds C(n, k-1).
    uint64_t _accum;
};

/**
 * Calculates the binomial coefficient "n choose k", i.e., the number of distinct
 * subsets of size k that can be chosen from a set of size n.
 */
uint64_t combinations(int n, int k);

/**
 * Container for all objects necessary to estimate the selectivity of join predicates.
 */
class JoinPredicateEstimator {
public:
    JoinPredicateEstimator(const JoinGraph& graph,
                           const std::vector<ResolvedPath>& resolvedPaths,
                           const SamplingEstimatorMap& samplingEstimators);

    /**
     * Returns an estimate of the selectivity of the given 'JoinEdge' using sampling.
     */
    cost_based_ranker::SelectivityEstimate joinPredicateSel(const JoinEdge& edge);

private:
    const JoinGraph& _graph;
    const std::vector<ResolvedPath>& _resolvedPaths;
    const SamplingEstimatorMap& _samplingEstimators;
};

}  // namespace mongo::join_ordering
