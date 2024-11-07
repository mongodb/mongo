/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/cost_based_ranker/heuristic_estimator.h"

#include <fmt/format.h>
#include <span>

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/query/cost_based_ranker/ce_utils.h"

namespace mongo::cost_based_ranker {

const CardinalityEstimate kSmallLimit{CardinalityType{20.0}, EstimationSource::Heuristics};
const CardinalityEstimate kMediumLimit{CardinalityType{100.0}, EstimationSource::Heuristics};

// The selectivities used in the piece-wise function for open-range intervals.
// Note that we assume a smaller input cardinality will result in a less selective range.
const SelectivityEstimate kSmallCardOpenRangeSel{SelectivityType{0.70},
                                                 EstimationSource::Heuristics};
const SelectivityEstimate kMediumCardOpenRangeSel{SelectivityType{0.45},
                                                  EstimationSource::Heuristics};
const SelectivityEstimate kLargeCardOpenRangeSel{SelectivityType{0.33},
                                                 EstimationSource::Heuristics};

// The selectivities used in the piece-wise function for closed-range intervals.
// Note that we assume a smaller input cardinality will result in a less selective range.
const SelectivityEstimate kSmallCardClosedRangeSel{SelectivityType{0.50},
                                                   EstimationSource::Heuristics};
const SelectivityEstimate kMediumCardClosedRangeSel{SelectivityType{0.33},
                                                    EstimationSource::Heuristics};
const SelectivityEstimate kLargeCardClosedRangeSel{SelectivityType{0.20},
                                                   EstimationSource::Heuristics};

SelectivityEstimate heuristicClosedRangeSel(CardinalityEstimate inputCard) {
    if (inputCard < kSmallLimit) {
        return kSmallCardClosedRangeSel;
    } else if (inputCard < kMediumLimit) {
        return kMediumCardClosedRangeSel;
    }
    return kLargeCardClosedRangeSel;
}

SelectivityEstimate heuristicOpenRangeSel(CardinalityEstimate inputCard) {
    if (inputCard < kSmallLimit) {
        return kSmallCardOpenRangeSel;
    } else if (inputCard < kMediumLimit) {
        return kMediumCardOpenRangeSel;
    }
    return kLargeCardOpenRangeSel;
}

SelectivityEstimate heuristicPointIntervalSel(CardinalityEstimate inputCard) {
    return SelectivityEstimate{SelectivityType{1 / std::sqrt(inputCard.toDouble())},
                               EstimationSource::Heuristics};
}

SelectivityEstimate estimateLeafMatchExpression(const MatchExpression* expr,
                                                CardinalityEstimate inputCard) {
    return [&]() -> SelectivityEstimate {
        switch (expr->matchType()) {
            case MatchExpression::MatchType::ALWAYS_FALSE:
                return zeroSel;
            case MatchExpression::MatchType::ALWAYS_TRUE:
                return oneSel;
            case MatchExpression::MatchType::EQ:
            case MatchExpression::MatchType::INTERNAL_EXPR_EQ: {
                // Equality predicate is equalivent to a point interval
                return heuristicPointIntervalSel(inputCard);
            }
            case MatchExpression::MatchType::LT:
            case MatchExpression::MatchType::GT: {
                return heuristicOpenRangeSel(inputCard);
            }
            case MatchExpression::MatchType::LTE:
            case MatchExpression::MatchType::GTE: {
                return heuristicClosedRangeSel(inputCard);
            }
            case MatchExpression::MatchType::REGEX:
                return kRegexSel;
            case MatchExpression::MatchType::MOD: {
                // Assume that the results of mod are equally likely.
                auto modExpr = static_cast<const ModMatchExpression*>(expr);
                return {SelectivityType{1.0 / modExpr->getDivisor()}, EstimationSource::Heuristics};
            }
            case MatchExpression::MatchType::EXISTS: {
                return kExistsSel;
            }
            case MatchExpression::MatchType::MATCH_IN: {
                // Construct vector of selectivities for each element in the $in list and perform
                // disjunction estimation.
                auto inExpr = static_cast<const InMatchExpression*>(expr);
                std::vector<SelectivityEstimate> sels(inExpr->getEqualities().size(),
                                                      heuristicPointIntervalSel(inputCard));
                sels.insert(sels.end(), inExpr->getRegexes().size(), kRegexSel);
                return disjExponentialBackoff(sels);
            }
            case MatchExpression::MatchType::TYPE_OPERATOR: {
                // Treat each operand in a $type operator as a closed interval. Estimate it by
                // constructing a vector a selecitvies (one per type specified) and perform
                // disjunction estimation, similar to $in.
                auto typeExpr = static_cast<const TypeMatchExpression*>(expr);
                std::vector<SelectivityEstimate> sels(typeExpr->typeSet().bsonTypes.size(),
                                                      heuristicClosedRangeSel(inputCard));
                return disjExponentialBackoff(sels);
            }
            case MatchExpression::MatchType::BITS_ALL_SET:
            case MatchExpression::MatchType::BITS_ALL_CLEAR:
            case MatchExpression::MatchType::BITS_ANY_SET:
            case MatchExpression::MatchType::BITS_ANY_CLEAR: {
                return kBitsSel;
            }
            default:
                tasserted(9608701,
                          fmt::format("invalid MatchExpression passed to heuristic estimate: {}",
                                      expr->matchType()));
        }
    }();
}

SelectivityEstimate estimateInterval(const Interval& interval, CardinalityEstimate inputCard) {
    if (interval.isEmpty() || interval.isNull()) {
        return zeroSel;
    }
    if (interval.isFullyOpen()) {
        return oneSel;
    }
    if (interval.isPoint()) {
        if (inputCard <= oneCE) {
            return oneSel;
        }
        return heuristicPointIntervalSel(inputCard);
    }
    // At this point, we know this interval is a range.

    // We use different heuristic based on whether this range is open or closed.
    if (interval.start.type() == MinKey || interval.end.type() == MaxKey ||
        !interval.startInclusive || !interval.endInclusive) {
        return heuristicOpenRangeSel(inputCard);
    }
    return heuristicClosedRangeSel(inputCard);
}

SelectivityEstimate estimateOil(const OrderedIntervalList& oil, CardinalityEstimate inputCard) {
    std::vector<SelectivityEstimate> sels;
    for (size_t j = 0; j < oil.intervals.size(); ++j) {
        sels.push_back(estimateInterval(oil.intervals[j], inputCard));
    }
    return disjExponentialBackoff(sels);
}

SelectivityEstimate estimateIndexBounds(const IndexBounds& bounds, CardinalityEstimate inputCard) {
    if (bounds.isSimpleRange) {
        MONGO_UNIMPLEMENTED;
    }
    if (bounds.isUnbounded()) {
        return oneSel;
    }
    std::vector<SelectivityEstimate> sels;
    for (size_t i = 0; i < bounds.fields.size(); ++i) {
        sels.push_back(estimateOil(bounds.fields[i], inputCard));
    }
    return conjExponentialBackoff(sels);
}

}  // namespace mongo::cost_based_ranker
