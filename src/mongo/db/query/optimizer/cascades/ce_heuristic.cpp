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

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"

#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/util/assert_util.h"

namespace mongo::optimizer::cascades {

using namespace properties;
using namespace mongo::ce;

// Invalid estimate - an arbitrary negative value used for initialization.
constexpr SelectivityType kInvalidSel = -1.0;
constexpr SelectivityType kDefaultFilterSel = 0.1;

class CEHeuristicTransport {
public:
    CEType transport(const ScanNode& node, CEType /*bindResult*/) {
        // Default cardinality estimate.
        const CEType metadataCE = _memo.getMetadata()._scanDefs.at(node.getScanDefName()).getCE();
        return (metadataCE < 0.0) ? kDefaultCard : metadataCE;
    }

    CEType transport(const ValueScanNode& node, CEType /*bindResult*/) {
        return node.getArraySize();
    }

    CEType transport(const MemoLogicalDelegatorNode& node) {
        return getPropertyConst<CardinalityEstimate>(
                   _memo.getGroup(node.getGroupId())._logicalProperties)
            .getEstimate();
    }

    CEType transport(const FilterNode& node, CEType childResult, CEType /*exprResult*/) {
        if (childResult == 0.0) {
            return 0.0;
        }
        if (node.getFilter() == Constant::boolean(true)) {
            // Trivially true filter.
            return childResult;
        } else if (node.getFilter() == Constant::boolean(false)) {
            // Trivially false filter.
            return 0.0;
        } else {
            // Estimate filter selectivity at 0.1.
            return std::max(kDefaultFilterSel * childResult, kMinCard);
        }
    }

    CEType transport(const EvaluationNode& node, CEType childResult, CEType /*exprResult*/) {
        // Evaluations do not change cardinality.
        return childResult;
    }

    /**
     * Default selectivity of equalities. To avoid super small selectivities for small
     * cardinalities, that would result in 0 cardinality for many small inputs, the
     * estimate is scaled as inputCard grows. The bigger inputCard, the smaller the
     * selectivity.
     */
    SelectivityType equalitySel(const CEType inputCard) {
        uassert(6716604, "Zero cardinality must be handled by the caller.", inputCard > 0.0);
        return std::sqrt(inputCard) / inputCard;
    }

    /**
     * Default selectivity of intervals with bounds on both ends. These intervals are
     * considered less selective than equalities.
     * Examples: (a > 'abc' AND a < 'hta'), (0 < b <= 13)
     */
    SelectivityType closedRangeSel(const CEType inputCard) {
        CEType sel = kInvalidSel;
        if (inputCard < 20.0) {
            sel = 0.50;
        } else if (inputCard < 100.0) {
            sel = 0.33;
        } else {
            sel = 0.20;
        }
        return sel;
    }

    /**
     * Default selectivity of intervals open on one end. These intervals are
     * considered less selective than those with both ends specified by the user query.
     * Examples: (a > 'xyz'), (b <= 13)
     */
    SelectivityType openRangeSel(const CEType inputCard) {
        SelectivityType sel = kInvalidSel;
        if (inputCard < 20.0) {
            sel = 0.70;
        } else if (inputCard < 100.0) {
            sel = 0.45;
        } else {
            sel = 0.33;
        }
        return sel;
    }

    mongo::sbe::value::TypeTags boundType(const BoundRequirement& bound) {
        const auto constBoundPtr = bound.getBound().cast<Constant>();
        if (constBoundPtr == nullptr) {
            return mongo::sbe::value::TypeTags::Nothing;
        }
        const auto [tag, val] = constBoundPtr->get();
        return tag;
    }

    SelectivityType intervalSel(const IntervalRequirement& interval, const CEType inputCard) {
        SelectivityType sel = kInvalidSel;
        if (interval.isFullyOpen()) {
            sel = 1.0;
        } else if (interval.isEquality()) {
            sel = equalitySel(inputCard);
        } else if (interval.getHighBound().isPlusInf() || interval.getLowBound().isMinusInf() ||
                   boundType(interval.getLowBound()) != boundType(interval.getHighBound())) {
            // The interval has an actual bound only on one of it ends if:
            // - one of the bounds is infinite, or
            // - both bounds are of a different type - this is the case when due to type bracketing
            //   one of the bounds is the lowest/highest value of the previous/next type.
            // TODO: Notice that sometimes type bracketing uses a min/max value from the same type,
            // so sometimes we may not detect an open-ended interval.
            sel = openRangeSel(inputCard);
        } else {
            sel = closedRangeSel(inputCard);
        }
        uassert(6716603, "Invalid selectivity.", validSelectivity(sel));
        return sel;
    }

    CEType transport(const SargableNode& node,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        // Early out and return 0 since we don't expect to get more results.
        if (childResult == 0.0) {
            return 0.0;
        }

        SelectivityType topLevelSel = 1.0;
        std::vector<SelectivityType> topLevelSelectivities;
        for (const auto& [key, req] : node.getReqMap()) {
            SelectivityType disjSel = 1.0;
            std::vector<SelectivityType> disjSelectivities;
            // Intervals are in DNF.
            const auto intervalDNF = req.getIntervals();
            const auto disjuncts = intervalDNF.cast<IntervalReqExpr::Disjunction>()->nodes();
            for (const auto& disjunct : disjuncts) {
                const auto& conjuncts = disjunct.cast<IntervalReqExpr::Conjunction>()->nodes();
                SelectivityType conjSel = 1.0;
                std::vector<SelectivityType> conjSelectivities;
                for (const auto& conjunct : conjuncts) {
                    const auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
                    const SelectivityType sel = intervalSel(interval, childResult);
                    conjSelectivities.push_back(sel);
                }
                conjSel = ce::conjExponentialBackoff(std::move(conjSelectivities));
                disjSelectivities.push_back(conjSel);
            }
            disjSel = ce::disjExponentialBackoff(std::move(disjSelectivities));
            topLevelSelectivities.push_back(disjSel);
        }

        // The elements of the PartialSchemaRequirements map represent an implicit conjunction.
        topLevelSel = ce::conjExponentialBackoff(std::move(topLevelSelectivities));
        CEType card = std::max(topLevelSel * childResult, kMinCard);
        uassert(6716602, "Invalid cardinality.", mongo::ce::validCardinality(card));
        return card;
    }

    CEType transport(const RIDIntersectNode& node,
                     CEType /*leftChildResult*/,
                     CEType /*rightChildResult*/) {
        // CE for the group should already be derived via the underlying Filter or Evaluation
        // logical nodes.
        uasserted(6624038, "Should not be necessary to derive CE for RIDIntersectNode");
    }

    CEType transport(const BinaryJoinNode& node,
                     CEType leftChildResult,
                     CEType rightChildResult,
                     CEType /*exprResult*/) {
        const auto& filter = node.getFilter();

        SelectivityType selectivity = kDefaultFilterSel;
        if (filter == Constant::boolean(false)) {
            selectivity = 0.0;
        } else if (filter == Constant::boolean(true)) {
            selectivity = 1.0;
        }
        return leftChildResult * rightChildResult * selectivity;
    }

    CEType transport(const UnionNode& node,
                     std::vector<CEType> childResults,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        // Combine the CE of each child.
        CEType result = 0;
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
                return 0.01 * childResult;

            // Global and Local selectivity should multiply to Complete selectivity.
            case GroupNodeType::Global:
                return 0.5 * childResult;
            case GroupNodeType::Local:
                return 0.02 * childResult;

            default:
                MONGO_UNREACHABLE;
        }
    }

    CEType transport(const UnwindNode& node,
                     CEType childResult,
                     CEType /*bindResult*/,
                     CEType /*refsResult*/) {
        // Estimate unwind selectivity at 10.0
        return 10.0 * childResult;
    }

    CEType transport(const CollationNode& node, CEType childResult, CEType /*refsResult*/) {
        // Collations do not change cardinality.
        return childResult;
    }

    CEType transport(const LimitSkipNode& node, CEType childResult) {
        const auto limit = node.getProperty().getLimit();
        if (limit < childResult) {
            return limit;
        }
        return childResult;
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
        return 0.0;
    }

    static CEType derive(const Memo& memo, const ABT::reference_type logicalNodeRef) {
        CEHeuristicTransport instance(memo);
        return algebra::transport<false>(logicalNodeRef, instance);
    }

private:
    CEHeuristicTransport(const Memo& memo) : _memo(memo) {}

    // We don't own this.
    const Memo& _memo;
};

CEType HeuristicCE::deriveCE(const Memo& memo,
                             const LogicalProps& /*logicalProps*/,
                             const ABT::reference_type logicalNodeRef) const {
    CEType card = CEHeuristicTransport::derive(memo, logicalNodeRef);
    return card;
}

}  // namespace mongo::optimizer::cascades
