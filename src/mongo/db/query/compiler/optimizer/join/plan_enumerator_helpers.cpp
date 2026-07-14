// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
                .op = convertToPhysicalOperator(pred.op),
                .field = ctx.resolvedPaths[pathId].underlyingFieldPath,
            });
        }

        return bestIndexSatisfyingJoinPredicates(ixes->second, indexedJoinPreds);
    }
    return nullptr;
}

QSNJoinPredicate::ComparisonOp convertToPhysicalOperator(JoinPredicate::Operator op) {
    switch (op) {
        case JoinPredicate::Eq:
            return QSNJoinPredicate::ComparisonOp::Eq;
        case JoinPredicate::ExprEq:
            return QSNJoinPredicate::ComparisonOp::ExprEq;
    }
    MONGO_UNREACHABLE_TASSERT(11075702);
}

}  // namespace mongo::join_ordering
