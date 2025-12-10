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

#include "mongo/db/query/compiler/optimizer/join/plan_enumerator_helpers.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::join_ordering {

CombinationSequence::CombinationSequence(int n) : _n(n), _k(0), _accum(1) {
    tassert(10986302,
            str::stream{} << "illegal combination " << _n << " choose " << _k,
            _k <= _n && _k >= 0 && _n > 0);
}

uint64_t CombinationSequence::next() {
    tassert(
        10986301, str::stream{} << "attempted to calculate " << _n << " choose " << _k, _k <= _n);
    // Base case: C(n, 0) = 1
    if (_k == 0) {
        ++_k;
        return _accum;
    }
    // Inductive case: C(n, k) = C(n, k-1) * (n - k + 1) / k.
    // This formula can be derived from the definition of binomial coefficient:
    // C(n, k) = n! / (k! * (n-k)!)
    _accum = (_accum * (_n - _k + 1)) / _k;
    ++_k;
    return _accum;
}

uint64_t combinations(int n, int k) {
    if (n < 0 || k < 0 || k > n) {
        return 0;
    }
    if (n == k || k == 0) {
        return 1;
    }
    // Optimization C(n, k) = C(n, n-k)
    k = std::min(k, n - k);
    CombinationSequence cs(n);
    uint64_t res = 1;
    for (int i = 0; i <= k; ++i) {
        res = cs.next();
    }
    return res;
}

JoinPredicateEstimator::JoinPredicateEstimator(const JoinGraph& graph,
                                               const std::vector<ResolvedPath>& resolvedPaths,
                                               const SamplingEstimatorMap& samplingEstimators)
    : _graph(graph), _resolvedPaths(resolvedPaths), _samplingEstimators(samplingEstimators) {}

// This function makes a number of assumptions:
// * Join predicate are independent from single table predicates. This allows us to estimate them
// separately, which can be seen by our use of NDV(join key) over the entire collection, as opposed
// to considering values after selections.
// * While MongoDB does not implement referential data integrity constraints like typical relational
// systems, we assume that joins are logically either primary key - foreign key (PK-FK) joins or
// foreign key - foreign key (FK-FK) joins. These types of joins satisfy the "Principle of
// Inclusion" which states that every foreign key value must exist as a primary key value in the
// primary table. We also assume that there is a uniform distribution of foreign key values within
// foreign tables over the set of primary key values in the primary table.
//
// The algorithm this function performs is rather simple, we look at the node which has a smaller
// CE (before single-table selections), calculate the NDV of the join key of that node and return
// 1/NDV(PK). To explain why this works, we should examine the two possible cases we assumed we are
// in.
//
// Case 1: This join represents a PK-FK join. Recall that a primary key must be unique and due to
// the principle of inclusion, we know that the cardinality of the join is card(F). The selectivity
// of the join is defined as the cardinality of the join over the cardinality of the cross product.
// Join sel = Card(F) / (Card(F) * Card(P)). Therefore, the selectivity is 1 / Card(P).

// Case 2: This join represents a FK-FK join. Here, we make an additional assumption that the two
// join keys are foreign keys to the same underlying primary table, P. In this case, the join
// cardinality is a little more complex. We can estimate it as:
// (Card(F1) / Card(P)) * (Card(F2) / Card(P)) * Card(P) = (Card(F1) * Card(F2)) / Card(P)
// Here we make use of the uniform distribution of foreign keys assumption: Every row in F1 has a
// foreign key value chosen uniformly from the (|P|) possible PK values. So for any particular row
// in P, the number of rows in F1 that reference it is Card(F1) / Card(P). The same logic applies to
// F2. We multiply by Card(P) at the end since that is the number of distinct PK values that can be
// referenced. Simplifying the above equation, we get:
// Join card = (Card(F1) * Card(F2)) / Card(P)
// We divide this by the cross product cardinality to get the selectivity:
// Join sel = (Card(F1) * Card(F2)) / (Card(F1) * Card(F2) * Card(P)) = 1 / Card(P)
//
// Regardless of whether we are in case (1) or (2), our estimate of join selectivity is 1 / Card(P).
// If we are in case (1), for simplicity we assume that the node with the smaller CE is the primary
// key side. We estimate Card(P) by estimating NDV(PK), though we easily could have done NDV(FK) as
// based on our assumptions, we'd get a similar result.
//
// If we are in case (2), we again can estimate Card(P) via NDV(FK) on either side, since we assume
// both sides reference the primary key. Again, we use the side with the smaller CE for simplicity.
cost_based_ranker::SelectivityEstimate JoinPredicateEstimator::joinPredicateSel(
    const JoinEdge& edge) {

    auto& leftNode = _graph.getNode(edge.left);
    auto& rightNode = _graph.getNode(edge.right);

    // Extract the cardinality estimates for left and right nodes before single table predicates are
    // applied.
    auto leftCard = _samplingEstimators.at(leftNode.collectionName)->getCollCard();
    auto rightCard = _samplingEstimators.at(rightNode.collectionName)->getCollCard();

    // For the purposes of estimation, we assume that this edge represents a "primary key" to
    // "foreign key" join, despite these concepts not existing in MongoDB. We also assume that the
    // node with the small CE is the primary key side.
    bool smallerCardIsLeft = leftCard <= rightCard;
    auto& primaryKeyNode = smallerCardIsLeft ? leftNode : rightNode;

    // Accumulate the field names of the "primary key" of the join edge.
    std::vector<FieldPath> fields;
    for (auto&& joinPred : edge.predicates) {
        tassert(11352502,
                "join predicate selectivity estimatation only supported for equality",
                joinPred.op == JoinPredicate::Eq);
        auto pathId = smallerCardIsLeft ? joinPred.left : joinPred.right;
        fields.push_back(_resolvedPaths[pathId].fieldName);
    }

    // Get sampling estimator for the "primary key" collection
    auto& samplingEstimator = _samplingEstimators.at(primaryKeyNode.collectionName);
    // Invoke NDV estimation for the "primary key"
    auto ndv = samplingEstimator->estimateNDV(fields);

    cost_based_ranker::SelectivityEstimate res{cost_based_ranker::oneSel};
    // Ensure we don't accidentally produce a selectivity > 1
    if (ndv.toDouble() > 1) {
        res = cost_based_ranker::oneCE / ndv;
    }
    LOGV2_DEBUG(11352504,
                5,
                "Performed estimation of selectivity of join edge",
                "leftNss"_attr = leftNode.collectionName,
                "rightNs"_attr = rightNode.collectionName,
                "smallerColl"_attr =
                    smallerCardIsLeft ? leftNode.collectionName : rightNode.collectionName,
                "fields"_attr = fields,
                "ndvEstimate"_attr = ndv,
                "selectivityEstimate"_attr = res);
    return res;
}

}  // namespace mongo::join_ordering
