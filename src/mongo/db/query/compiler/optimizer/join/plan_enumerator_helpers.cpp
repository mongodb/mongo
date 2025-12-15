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

#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo::join_ordering {
namespace {
std::shared_ptr<const IndexCatalogEntry> betterIndexForProbe(
    std::shared_ptr<const IndexCatalogEntry> first,
    std::shared_ptr<const IndexCatalogEntry> second) {
    const auto& firstKeyPattern = first->descriptor()->keyPattern();
    const auto& secondKeyPattern = second->descriptor()->keyPattern();
    if (firstKeyPattern.nFields() < secondKeyPattern.nFields()) {
        return first;
    } else if (firstKeyPattern.nFields() > secondKeyPattern.nFields()) {
        return second;
    }
    if (firstKeyPattern.woCompare(secondKeyPattern) > 0) {
        return second;
    }
    return first;
}
}  // namespace

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

bool indexSatisfiesJoinPredicates(const BSONObj& keyPattern,
                                  const std::vector<IndexedJoinPredicate>& joinPreds) {
    StringSet joinFields;
    for (auto&& joinPred : joinPreds) {
        joinFields.insert(joinPred.field.fullPath());
    }
    for (auto&& elem : keyPattern) {
        auto it = joinFields.find(elem.fieldName());
        if (it != joinFields.end()) {
            joinFields.erase(it);
        } else {
            break;
        }
    }
    return joinFields.empty();
}

std::shared_ptr<const IndexCatalogEntry> bestIndexSatisfyingJoinPredicates(
    const std::vector<std::shared_ptr<const IndexCatalogEntry>>& ices,
    const std::vector<IndexedJoinPredicate>& joinPreds) {
    std::shared_ptr<const IndexCatalogEntry> bestIndex;
    for (auto&& ice : ices) {
        const auto& desc = ice->descriptor();
        if (indexSatisfiesJoinPredicates(desc->keyPattern(), joinPreds)) {
            if (!bestIndex) {
                bestIndex = ice;
            } else {
                // Keep the better suited index in 'bestIndex'.
                bestIndex = betterIndexForProbe(bestIndex, ice);
            }
        }
    }
    return bestIndex;
}

std::shared_ptr<const IndexCatalogEntry> bestIndexSatisfyingJoinPredicates(
    const JoinReorderingContext& ctx, NodeId nodeId, const JoinEdge& edge) {
    tassert(11371700, "Node must be part of edge", edge.left == nodeId || edge.right == nodeId);

    const auto& node = ctx.joinGraph.getNode(nodeId);
    auto ixes = ctx.perCollIdxs.find(node.collectionName);
    if (ixes != ctx.perCollIdxs.end() && !ixes->second.empty()) {
        std::vector<IndexedJoinPredicate> indexedJoinPreds;
        for (auto&& pred : edge.predicates) {
            // We may not have re-oriented the edge if we're calling from bottom-up enumeration.
            const auto pathId = edge.left == nodeId ? pred.left : pred.right;
            indexedJoinPreds.push_back({
                .op = QSNJoinPredicate::ComparisonOp::Eq,
                .field = ctx.resolvedPaths[pathId].fieldName,
            });
        }

        return bestIndexSatisfyingJoinPredicates(ixes->second, indexedJoinPreds);
    }
    return nullptr;
}

}  // namespace mongo::join_ordering
