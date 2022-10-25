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

#include "mongo/db/query/cost_model/cost_estimator.h"

#include "mongo/db/query/optimizer/defs.h"

namespace mongo::cost_model {

using namespace optimizer;
using namespace optimizer::properties;
using optimizer::cascades::Memo;

namespace {
struct CostAndCEInternal {
    CostAndCEInternal(double cost, CEType ce) : _cost(cost), _ce(ce) {
        uassert(7034000, "Invalid cost.", !std::isnan(cost) && cost >= 0.0);
        uassert(7034001, "Invalid cardinality", std::isfinite(ce) && ce >= 0.0);
    }
    double _cost;
    CEType _ce;
};

class CostDerivation {
public:
    CostAndCEInternal operator()(const ABT& /*n*/, const PhysicalScanNode& /*node*/) {
        // Default estimate for scan.
        const double collectionScanCost = _coefficients.getStartupCost() +
            _coefficients.getScanIncrementalCost() * _cardinalityEstimate;
        return {collectionScanCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const CoScanNode& /*node*/) {
        // Assumed to be free.
        return {_coefficients.getStartupCost(), _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const IndexScanNode& node) {
        const double indexScanCost = _coefficients.getStartupCost() +
            _coefficients.getIndexScanIncrementalCost() * _cardinalityEstimate;
        return {indexScanCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const SeekNode& /*node*/) {
        // SeekNode should deliver one result via cardinality estimate override.
        // TODO: consider using node.getProjectionMap()._fieldProjections.size() to make the cost
        // dependent on the size of the projection
        const double seekCost =
            _coefficients.getStartupCost() + _coefficients.getSeekCost() * _cardinalityEstimate;
        return {seekCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const MemoLogicalDelegatorNode& node) {
        const LogicalProps& childLogicalProps = _memo.getLogicalProps(node.getGroupId());
        // Notice that unlike all physical nodes, this logical node takes it cardinality directly
        // from the memo group logical property, ignoring _cardinalityEstimate.
        CEType baseCE = getPropertyConst<CardinalityEstimate>(childLogicalProps).getEstimate();

        if (hasProperty<IndexingRequirement>(_physProps)) {
            const auto& indexingReq = getPropertyConst<IndexingRequirement>(_physProps);
            if (indexingReq.getIndexReqTarget() == IndexReqTarget::Seek) {
                // If we are performing a seek, normalize against the scan group cardinality.
                const GroupIdType scanGroupId =
                    getPropertyConst<IndexingAvailability>(childLogicalProps).getScanGroupId();
                if (scanGroupId == node.getGroupId()) {
                    baseCE = 1.0;
                } else {
                    const CEType scanGroupCE =
                        getPropertyConst<CardinalityEstimate>(_memo.getLogicalProps(scanGroupId))
                            .getEstimate();
                    if (scanGroupCE > 0.0) {
                        baseCE /= scanGroupCE;
                    }
                }
            }
        }

        return {0.0, getAdjustedCE(baseCE, _physProps)};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const MemoPhysicalDelegatorNode& /*node*/) {
        uasserted(7034002, "Should not be costing physical delegator nodes.");
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const FilterNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        double filterCost = childResult._cost;
        if (!isTrivialExpr<EvalFilter>(node.getFilter())) {
            // Non-trivial filter.
            filterCost += _coefficients.getStartupCost() +
                _coefficients.getFilterIncrementalCost() * childResult._ce;
        }
        return {filterCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const EvaluationNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        double evalCost = childResult._cost;
        if (!isTrivialExpr<EvalPath>(node.getProjection())) {
            // Non-trivial projection.
            evalCost += _coefficients.getStartupCost() +
                _coefficients.getEvalIncrementalCost() * _cardinalityEstimate;
        }
        return {evalCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const BinaryJoinNode& node) {
        CostAndCEInternal leftChildResult = deriveChild(node.getLeftChild(), 0);
        CostAndCEInternal rightChildResult = deriveChild(node.getRightChild(), 1);
        const double joinCost = _coefficients.getStartupCost() +
            _coefficients.getBinaryJoinIncrementalCost() *
                (leftChildResult._ce + rightChildResult._ce) +
            leftChildResult._cost + rightChildResult._cost;
        return {joinCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const HashJoinNode& node) {
        CostAndCEInternal leftChildResult = deriveChild(node.getLeftChild(), 0);
        CostAndCEInternal rightChildResult = deriveChild(node.getRightChild(), 1);

        // TODO: distinguish build side and probe side.
        const double hashJoinCost = _coefficients.getStartupCost() +
            _coefficients.getHashJoinIncrementalCost() *
                (leftChildResult._ce + rightChildResult._ce) +
            leftChildResult._cost + rightChildResult._cost;
        return {hashJoinCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const MergeJoinNode& node) {
        CostAndCEInternal leftChildResult = deriveChild(node.getLeftChild(), 0);
        CostAndCEInternal rightChildResult = deriveChild(node.getRightChild(), 1);

        const double mergeJoinCost = _coefficients.getStartupCost() +
            _coefficients.getMergeJoinIncrementalCost() *
                (leftChildResult._ce + rightChildResult._ce) +
            leftChildResult._cost + rightChildResult._cost;

        return {mergeJoinCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const UnionNode& node) {
        const ABTVector& children = node.nodes();
        // UnionNode with one child is optimized away before lowering, therefore
        // its cost is the cost of its child.
        if (children.size() == 1) {
            CostAndCEInternal childResult = deriveChild(children[0], 0);
            return {childResult._cost, _cardinalityEstimate};
        }

        double totalCost = _coefficients.getStartupCost();
        // The cost is the sum of the costs of its children and the cost to union each child.
        for (size_t childIdx = 0; childIdx < children.size(); childIdx++) {
            CostAndCEInternal childResult = deriveChild(children[childIdx], childIdx);
            const double childCost = childResult._cost +
                (childIdx > 0 ? _coefficients.getUnionIncrementalCost() * childResult._ce : 0);
            totalCost += childCost;
        }
        return {totalCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const GroupByNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        double groupByCost = _coefficients.getStartupCost();

        // TODO: for now pretend global group by is free.
        if (node.getType() == GroupNodeType::Global) {
            groupByCost += childResult._cost;
        } else {
            // TODO: consider RepetitionEstimate since this is a stateful operation.
            groupByCost +=
                _coefficients.getGroupByIncrementalCost() * childResult._ce + childResult._cost;
        }
        return {groupByCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const UnwindNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        // Unwind probably depends mostly on its output size.
        const double unwindCost =
            _coefficients.getUnwindIncrementalCost() * _cardinalityEstimate + childResult._cost;
        return {unwindCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const UniqueNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        const double uniqueCost = _coefficients.getStartupCost() +
            _coefficients.getUniqueIncrementalCost() * childResult._ce + childResult._cost;
        return {uniqueCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const CollationNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        // TODO: consider RepetitionEstimate since this is a stateful operation.

        double logFactor = childResult._ce;
        double incrConst = _coefficients.getCollationIncrementalCost();
        if (hasProperty<LimitSkipRequirement>(_physProps)) {
            if (auto limit = getPropertyConst<LimitSkipRequirement>(_physProps).getAbsoluteLimit();
                limit < logFactor) {
                logFactor = limit;
                incrConst = _coefficients.getCollationWithLimitIncrementalCost();
            }
        }

        // Notice that log2(x) < 0 for any x < 1, and log2(1) = 0. Generally it makes sense that
        // there is no cost to sort 1 document, so the only cost left is the startup cost.
        const double sortCost = _coefficients.getStartupCost() + childResult._cost +
            ((logFactor <= 1.0)
                 ? 0.0
                 // TODO: The cost formula below is based on 1 field, mix of int and str. Instead we
                 // have to take into account the number and size of sorted fields.
                 : incrConst * childResult._ce * std::log2(logFactor));
        return {sortCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const LimitSkipNode& node) {
        // Assumed to be free.
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        const double limitCost = _coefficients.getStartupCost() + childResult._cost;
        return {limitCost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const ExchangeNode& node) {
        CostAndCEInternal childResult = deriveChild(node.getChild(), 0);
        double localCost = _coefficients.getStartupCost() +
            _coefficients.getExchangeIncrementalCost() * _cardinalityEstimate;

        switch (node.getProperty().getDistributionAndProjections()._type) {
            case DistributionType::Replicated:
                localCost *= 2.0;
                break;

            case DistributionType::HashPartitioning:
            case DistributionType::RangePartitioning:
                localCost *= 1.1;
                break;

            default:
                break;
        }

        return {localCost + childResult._cost, _cardinalityEstimate};
    }

    CostAndCEInternal operator()(const ABT& /*n*/, const RootNode& node) {
        return deriveChild(node.getChild(), 0);
    }

    /**
     * Other ABT types.
     */
    template <typename T, typename... Ts>
    CostAndCEInternal operator()(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        static_assert(!canBePhysicalNode<T>(), "Physical node must implement its cost derivation.");
        return {0.0, 0.0};
    }

    static CostAndCEInternal derive(const Metadata& metadata,
                                    const Memo& memo,
                                    const PhysProps& physProps,
                                    const ABT::reference_type physNodeRef,
                                    const ChildPropsType& childProps,
                                    const NodeCEMap& nodeCEMap,
                                    const CostModelCoefficients& coefficients) {
        CostAndCEInternal result = deriveInternal(
            metadata, memo, physProps, physNodeRef, childProps, nodeCEMap, coefficients);

        switch (getPropertyConst<DistributionRequirement>(physProps)
                    .getDistributionAndProjections()
                    ._type) {
            case DistributionType::Centralized:
            case DistributionType::Replicated:
                break;

            case DistributionType::RoundRobin:
            case DistributionType::HashPartitioning:
            case DistributionType::RangePartitioning:
            case DistributionType::UnknownPartitioning:
                result._cost /= metadata._numberOfPartitions;
                break;

            default:
                MONGO_UNREACHABLE;
        }

        return result;
    }

private:
    CostDerivation(const Metadata& metadata,
                   const Memo& memo,
                   const CEType ce,
                   const PhysProps& physProps,
                   const ChildPropsType& childProps,
                   const NodeCEMap& nodeCEMap,
                   const CostModelCoefficients& coefficients)
        : _metadata(metadata),
          _memo(memo),
          _physProps(physProps),
          _cardinalityEstimate(getAdjustedCE(ce, _physProps)),
          _childProps(childProps),
          _nodeCEMap(nodeCEMap),
          _coefficients(coefficients) {}

    template <class T>
    static bool isTrivialExpr(const ABT& n) {
        if (n.is<Constant>() || n.is<Variable>()) {
            return true;
        }
        if (const auto* ptr = n.cast<T>(); ptr != nullptr &&
            ptr->getPath().template is<PathIdentity>() && isTrivialExpr<T>(ptr->getInput())) {
            return true;
        }
        return false;
    }

    static CostAndCEInternal deriveInternal(const Metadata& metadata,
                                            const Memo& memo,
                                            const PhysProps& physProps,
                                            const ABT::reference_type physNodeRef,
                                            const ChildPropsType& childProps,
                                            const NodeCEMap& nodeCEMap,
                                            const CostModelCoefficients& coefficients) {
        auto it = nodeCEMap.find(physNodeRef.cast<Node>());
        bool found = (it != nodeCEMap.cend());
        uassert(7034003,
                "Only MemoLogicalDelegatorNode can be missing from nodeCEMap.",
                found || physNodeRef.is<MemoLogicalDelegatorNode>());
        const CEType ce = (found ? it->second : 0.0);

        CostDerivation instance(metadata, memo, ce, physProps, childProps, nodeCEMap, coefficients);
        CostAndCEInternal costCEestimates = physNodeRef.visit(instance);
        return costCEestimates;
    }

    CostAndCEInternal deriveChild(const ABT& child, const size_t childIndex) {
        PhysProps physProps = _childProps.empty() ? _physProps : _childProps.at(childIndex).second;
        return deriveInternal(
            _metadata, _memo, physProps, child.ref(), {}, _nodeCEMap, _coefficients);
    }

    static CEType getAdjustedCE(CEType baseCE, const PhysProps& physProps) {
        CEType result = baseCE;

        // First: correct for un-enforced limit.
        if (hasProperty<LimitSkipRequirement>(physProps)) {
            const auto limit = getPropertyConst<LimitSkipRequirement>(physProps).getAbsoluteLimit();
            if (result > limit) {
                result = limit;
            }
        }

        // Second: correct for enforced limit.
        if (hasProperty<LimitEstimate>(physProps)) {
            const auto limit = getPropertyConst<LimitEstimate>(physProps).getEstimate();
            if (result > limit) {
                result = limit;
            }
        }

        // Third: correct for repetition.
        if (hasProperty<RepetitionEstimate>(physProps)) {
            result *= getPropertyConst<RepetitionEstimate>(physProps).getEstimate();
        }

        return result;
    }

    // We don't own this.
    const Metadata& _metadata;
    const Memo& _memo;
    const PhysProps& _physProps;
    const CEType _cardinalityEstimate;
    const ChildPropsType& _childProps;
    const NodeCEMap& _nodeCEMap;
    const CostModelCoefficients& _coefficients;
};
}  // namespace

CostAndCE CostEstimator::deriveCost(const Metadata& metadata,
                                    const Memo& memo,
                                    const PhysProps& physProps,
                                    const ABT::reference_type physNodeRef,
                                    const ChildPropsType& childProps,
                                    const NodeCEMap& nodeCEMap) const {
    const CostAndCEInternal result = CostDerivation::derive(
        metadata, memo, physProps, physNodeRef, childProps, nodeCEMap, _coefficients);
    return {CostType::fromDouble(result._cost), result._ce};
}

}  // namespace mongo::cost_model
