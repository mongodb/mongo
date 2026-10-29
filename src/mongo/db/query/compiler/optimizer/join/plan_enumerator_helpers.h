// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/compiler/optimizer/join/join_graph.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

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
    // Value of n when calculating C(n, k). Stays constant for the life of this object.
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
 * Represent sargable predicate that can be the RHS of an indexed nested loop join.
 */
struct IndexedJoinPredicate {
    QSNJoinPredicate::ComparisonOp op;
    FieldPath field;
};

/**
 * Returns true if the given join predicates can be satisfied by an "index probe" (RHS of INLJ) on
 * the given index 'keyPattern'.
 */
bool indexSatisfiesJoinPredicates(const BSONObj& keyPattern,
                                  const std::vector<IndexedJoinPredicate>& joinPreds);

/**
 * Returns the best index that can satisfy the indexed predicates by "index probe". If there is
 * no index that can satisfy it then return boost::none. If multiple indexes can satisfy the
 * join predicates, this function selects one using deterministic heuristics:
 *  1. Prefer the index with fewer fields.
 *  2. If tied, prefer the index whose key pattern is lexicographically earlier.
 */
std::shared_ptr<const IndexCatalogEntry> bestIndexSatisfyingJoinPredicates(
    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& ies,
    const std::vector<IndexedJoinPredicate>& joinPreds);

/**
 * Same as above, but picks from indexes in 'ctx' that can be applied to the current node with id
 * 'nodeId' and the specified 'edge'.
 */
std::shared_ptr<const IndexCatalogEntry> bestIndexSatisfyingJoinPredicates(
    const JoinReorderingContext& ctx, NodeId nodeId, const JoinEdge& edge);

/**
 * Helper to convert from a logical JoinPredicate::Operator into a physical one used in constructing
 * a QSN tree.
 */
QSNJoinPredicate::ComparisonOp convertToPhysicalOperator(JoinPredicate::Operator op);

}  // namespace mongo::join_ordering
