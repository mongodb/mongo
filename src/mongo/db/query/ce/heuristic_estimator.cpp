/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/ce/heuristic_estimator.h"

#include "mongo/db/query/ce/heuristic_predicate_estimation.h"
#include "mongo/db/query/ce/sel_tree_utils.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"

#include "mongo/util/assert_util.h"

namespace mongo::optimizer::ce {

/**
 * Heuristic selectivity estimation for EvalFilter nodes. Used for estimating cardinalities of
 * FilterNodes. The estimate is computed by traversing the tree bottom-up, applying default
 * selectivity functions to atomic predicates (comparisons), and combining child selectivities of
 * disjunctions and conjunctions via simple addition and multiplication.
 */
class EvalFilterSelectivityTransport {
public:
    /**
     * Helper class for holding values passed from child to parent nodes when traversing the tree.
     */
    struct EvalFilterSelectivityResult {
        // Each item represents a field in a dotted path.
        // Collected while traversing a path expression.
        // Used for deciding whether a conjunction of comparisons is an interval or not.
        FieldPathType path;
        // When handling a PathComposeM, we need to access its child comparisons which might be
        // hidden under path expressions.
        const PathCompare* compare;
        // The selectivity estimate.
        SelectivityType selectivity;
    };

    EvalFilterSelectivityResult transport(const EvalFilter& /*node*/,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult pathResult,
                                          EvalFilterSelectivityResult /*inputResult*/) {
        return pathResult;
    }

    EvalFilterSelectivityResult transport(const PathGet& node,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult childResult) {
        childResult.path.push_back(node.name());
        return childResult;
    }

    EvalFilterSelectivityResult transport(const PathTraverse& node,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult childResult) {
        return childResult;
    }

    EvalFilterSelectivityResult transport(const PathCompare& node,
                                          CEType inputCard,
                                          EvalFilterSelectivityResult /*childResult*/) {
        // Note that the result will be ignored if this operation is part of an interval.
        const SelectivityType sel = heuristicOperationSel(node.op(), inputCard);
        return {{}, &node, sel};
    }

    EvalFilterSelectivityResult transport(const PathComposeM& node,
                                          CEType inputCard,
                                          EvalFilterSelectivityResult leftChildResult,
                                          EvalFilterSelectivityResult rightChildResult) {
        const bool isInterval = leftChildResult.compare && rightChildResult.compare &&
            leftChildResult.path == rightChildResult.path;

        const SelectivityType sel = isInterval
            ? heuristicIntervalSel(*leftChildResult.compare, *rightChildResult.compare, inputCard)
            : conjunctionSel(leftChildResult.selectivity, rightChildResult.selectivity);

        return {{}, nullptr, sel};
    }

    EvalFilterSelectivityResult transport(const PathComposeA& node,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult leftChildResult,
                                          EvalFilterSelectivityResult rightChildResult) {
        const SelectivityType sel =
            disjunctionSel(leftChildResult.selectivity, rightChildResult.selectivity);

        return {{}, nullptr, sel};
    }

    EvalFilterSelectivityResult transport(const UnaryOp& node,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult childResult) {
        switch (node.op()) {
            case Operations::Not:
                childResult.selectivity = negateSel(childResult.selectivity);
                return childResult;
            case Operations::Neg:
                // If we see negation (-) in a UnaryOp, we ignore it for CE purposes.
                return childResult;
            default:
                MONGO_UNREACHABLE;
        }
    }

    EvalFilterSelectivityResult transport(const PathConstant& /*node*/,
                                          CEType /*inputCard*/,
                                          EvalFilterSelectivityResult childResult) {
        return childResult;
    }

    EvalFilterSelectivityResult transport(const PathDefault& node,
                                          CEType inputCard,
                                          EvalFilterSelectivityResult childResult) {
        if (node.getDefault() == Constant::boolean(false)) {
            // We have a {$exists: true} predicate on this path if we have a Constant[false] child
            // here. Note that ${exists: false} is handled by the presence of a negation expression
            // higher in the ABT.
            childResult.selectivity = kDefaultExistsSel;
        }
        return childResult;
    }

    template <typename T, typename... Ts>
    EvalFilterSelectivityResult transport(const T& /*node*/, Ts&&...) {
        return {{}, nullptr, kDefaultFilterSel};
    }

    static SelectivityType derive(const CEType inputCard, const ABT::reference_type ref) {
        EvalFilterSelectivityTransport instance;
        const auto result = algebra::transport<false>(ref, instance, inputCard);
        return result.selectivity;
    }

private:
    SelectivityType conjunctionSel(const SelectivityType left, const SelectivityType right) {
        return left * right;
    }

    SelectivityType disjunctionSel(const SelectivityType left, const SelectivityType right) {
        // We sum the selectivities and subtract the overlapping part so that it's only counted
        // once.
        return left + right - left * right;
    }
};

class HeuristicTransport {
public:
    CEType transport(const ScanNode& node, CEType /*bindResult*/) {
        // Default cardinality estimate.
        const CEType metadataCE = _metadata._scanDefs.at(node.getScanDefName()).getCE();
        return (metadataCE < 0.0) ? kDefaultCard : metadataCE;
    }

    CEType transport(const ValueScanNode& node, CEType /*bindResult*/) {
        return {static_cast<double>(node.getArraySize())};
    }

    CEType transport(const MemoLogicalDelegatorNode& node) {
        return properties::getPropertyConst<properties::CardinalityEstimate>(
                   _memo.getLogicalProps(node.getGroupId()))
            .getEstimate();
    }

    CEType transport(const FilterNode& node, CEType childResult, CEType /*exprResult*/) {
        if (childResult == 0.0) {
            // Early out and return 0 since we don't expect to get more results.
            return {0.0};
        }
        if (node.getFilter() == Constant::boolean(true)) {
            // Trivially true filter.
            return childResult;
        }
        if (node.getFilter() == Constant::boolean(false)) {
            // Trivially false filter.
            return {0.0};
        }

        const SelectivityType sel =
            EvalFilterSelectivityTransport::derive(childResult, node.getFilter().ref());

        return std::max(sel * childResult, kMinCard);
    }

    CEType transport(const EvaluationNode& node, CEType childResult, CEType /*exprResult*/) {
        // Evaluations do not change cardinality.
        return childResult;
    }

    CEType transport(const SargableNode& node,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        // Early out and return 0 since we don't expect to get more results.
        if (childResult == 0.0) {
            return {0.0};
        }

        EstimateIntervalSelFn estimateIntervalFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                       const IntervalRequirement& interval) {
            selTreeBuilder.atom(heuristicIntervalSel(interval, childResult));
        };

        EstimatePartialSchemaEntrySelFn estimateFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                         const PartialSchemaEntry& e) {
            const auto& [key, req] = e;
            IntervalSelectivityTreeBuilder intEstimator{selTreeBuilder, estimateIntervalFn};
            intEstimator.build(req.getIntervals());
        };

        PartialSchemaRequirementsCardinalityEstimator estimator(estimateFn, childResult);
        const CEType estimate = estimator.estimateCE(node.getReqMap().getRoot());

        const CEType card = std::max(estimate, kMinCard);
        uassert(6716602, "Invalid cardinality.", validCardinality(card));
        return card;
    }

    CEType transport(const RIDIntersectNode& node,
                     CEType /*leftChildResult*/,
                     CEType /*rightChildResult*/) {
        // CE for the group should already be derived via the underlying Filter or Evaluation
        // logical nodes.
        uasserted(6624038, "Should not be necessary to derive CE for RIDIntersectNode");
    }

    CEType transport(const RIDUnionNode& node,
                     CEType /*leftChildResult*/,
                     CEType /*rightChildResult*/) {
        // CE for the group should already be derived via the underlying Filter or Evaluation
        // logical nodes.
        uasserted(7016301, "Should not be necessary to derive CE for RIDUnionNode");
    }

    CEType transport(const BinaryJoinNode& node,
                     CEType leftChildResult,
                     CEType rightChildResult,
                     CEType /*exprResult*/) {
        const auto& filter = node.getFilter();

        SelectivityType selectivity = kDefaultFilterSel;
        if (filter == Constant::boolean(false)) {
            selectivity = {0.0};
        } else if (filter == Constant::boolean(true)) {
            selectivity = {1.0};
        }
        return computeJoinCE(leftChildResult, rightChildResult, selectivity);
    }

    CEType transport(const UnionNode& node,
                     std::vector<CEType> childResults,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        // Combine the CE of each child.
        CEType result{0.0};
        for (auto&& child : childResults) {
            result += child;
        }
        return result;
    }

    CEType transport(const GroupByNode& node,
                     CEType childResult,
                     CEType /*bindAggResult*/,
                     CEType /*refsAggResult*/,
                     CEType /*bindGbResult*/,
                     CEType /*refsGbResult*/) {
        // TODO: estimate number of groups.
        switch (node.getType()) {
            case GroupNodeType::Complete:
                return kDefaultCompleteGroupSel * childResult;

            // Global and Local selectivity should multiply to Complete selectivity.
            case GroupNodeType::Global:
                return kDefaultGlobalGroupSel * childResult;
            case GroupNodeType::Local:
                return kDefaultLocalGroupSel * childResult;

            default:
                MONGO_UNREACHABLE;
        }
    }

    CEType transport(const UnwindNode& node,
                     CEType childResult,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        return kDefaultAverageArraySize * childResult;
    }

    CEType transport(const CollationNode& node, CEType childResult, CEType /*refsResult*/) {
        // Collations do not change cardinality.
        return childResult;
    }

    CEType transport(const LimitSkipNode& node, CEType childResult) {
        const auto limit = node.getProperty().getLimit();
        const auto skip = node.getProperty().getSkip();
        const auto cardAfterSkip = (childResult > skip) ? (childResult._value - skip) : 0.0;
        if (limit < cardAfterSkip) {
            return {static_cast<double>(limit)};
        }
        return {cardAfterSkip};
    }

    CEType transport(const ExchangeNode& node, CEType childResult, CEType /*refsResult*/) {
        // Exchanges do not change cardinality.
        return childResult;
    }

    CEType transport(const RootNode& node, CEType childResult, CEType /*refsResult*/) {
        // Root node does not change cardinality.
        return childResult;
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    CEType transport(const T& /*node*/, Ts&&...) {
        static_assert(!canBeLogicalNode<T>(), "Logical node must implement its CE derivation.");
        return {0.0};
    }

    static CEType derive(const Metadata& metadata,
                         const cascades::Memo& memo,
                         const ABT::reference_type logicalNodeRef) {
        HeuristicTransport instance(metadata, memo);
        return algebra::transport<false>(logicalNodeRef, instance);
    }

private:
    HeuristicTransport(const Metadata& metadata, const cascades::Memo& memo)
        : _metadata(metadata), _memo(memo) {}

    // We don't own this.
    const Metadata& _metadata;
    const cascades::Memo& _memo;
};

CEType HeuristicEstimator::deriveCE(const Metadata& metadata,
                                    const cascades::Memo& memo,
                                    const properties::LogicalProps& /*logicalProps*/,
                                    const ABT::reference_type logicalNodeRef) const {
    return HeuristicTransport::derive(metadata, memo, logicalNodeRef);
}

}  // namespace mongo::optimizer::ce
