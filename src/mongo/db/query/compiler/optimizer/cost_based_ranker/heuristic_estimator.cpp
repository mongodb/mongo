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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/heuristic_estimator.h"

#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/ce_utils.h"

#include <span>

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

bool heuristicIsEstimable(const MatchExpression* expr) {
    switch (expr->matchType()) {
        // These are the complement set of match types to the switch statement in
        // heuristicLeafMatchExpressionSel. The union of both sets are all MatchExpression types.
        case MatchExpression::MatchType::AND:
        case MatchExpression::MatchType::OR:
        case MatchExpression::MatchType::NOT:
        case MatchExpression::MatchType::NOR:
        case MatchExpression::MatchType::ELEM_MATCH_OBJECT:
        case MatchExpression::MatchType::ELEM_MATCH_VALUE:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_XOR:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_COND:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_OBJECT_MATCH:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
            return false;
        default:
            return true;
    }
}

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

/**
 * Compute the selectivity of an arbitrary predicate. The selectivity can be scaled depending on the
 * input cardinality - the bigger the cardinality, the better (smaller) the selectivity.
 * The idea being that typically queries in OLTP workloads against large datasets are designed to
 * select a small portion of the data.
 * Since different kinds of predicates are generally less/more selective, this function allows to
 * control the rate at which selectivity varies, so that some predicates can be more selective than
 * others.
 */
SelectivityEstimate heuristicScaledPredSel(CardinalityEstimate inputCard, double scalingFactor) {
    return SelectivityEstimate{SelectivityType{1 / std::pow(inputCard.toDouble(), scalingFactor)},
                               EstimationSource::Heuristics};
}

SelectivityEstimate heuristicLeafMatchExpressionSel(const MatchExpression* expr,
                                                    CardinalityEstimate inputCard) {
    tassert(9844001,
            str::stream{} << "heuristicLeafMatchExpressionSel got non-leaf expression: "
                          << expr->toString(),
            expr->numChildren() == 0);

    switch (expr->matchType()) {
        case MatchExpression::MatchType::ALWAYS_FALSE:
            return zeroSelHeuristic;
        case MatchExpression::MatchType::ALWAYS_TRUE:
            return oneSelHeuristic;
        case MatchExpression::MatchType::EQ:
        case MatchExpression::MatchType::INTERNAL_EXPR_EQ:
        case MatchExpression::MatchType::INTERNAL_EQ_HASHED_KEY:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_EQ:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_ROOT_DOC_EQ: {
            // Equality predicate is equalivent to a point interval
            return heuristicScaledPredSel(inputCard, kEqualityScalingFactor);
        }
        case MatchExpression::MatchType::LT:
        case MatchExpression::MatchType::GT:
        case MatchExpression::MatchType::INTERNAL_EXPR_LT:
        case MatchExpression::MatchType::INTERNAL_EXPR_GT: {
            return heuristicOpenRangeSel(inputCard);
        }
        case MatchExpression::MatchType::LTE:
        case MatchExpression::MatchType::GTE:
        case MatchExpression::MatchType::INTERNAL_EXPR_LTE:
        case MatchExpression::MatchType::INTERNAL_EXPR_GTE: {
            return heuristicClosedRangeSel(inputCard);
        }
        case MatchExpression::MatchType::MOD: {
            // Assume that the results of mod are equally likely.
            auto modExpr = static_cast<const ModMatchExpression*>(expr);
            return {SelectivityType{1.0 / std::abs(modExpr->getDivisor())},
                    EstimationSource::Heuristics};
        }
        case MatchExpression::MatchType::EXISTS: {
            return kExistsSel;
        }
        case MatchExpression::MatchType::MATCH_IN: {
            // Construct vector of selectivities for each element in the $in list and perform
            // disjunction estimation.
            auto inExpr = static_cast<const InMatchExpression*>(expr);
            // The elements of an IN list are unique, therefore the sets of documents matching
            // each IN-list element are disjunct (similar to the elements of an OIL). Therefore
            // the selectivities of all equalities should be summed instead of applying
            // exponential backoff.
            double eqSel = heuristicScaledPredSel(inputCard, kEqualityScalingFactor).toDouble();
            double totalSelDbl = std::min(inExpr->getEqualities().size() * eqSel, 1.0);
            SelectivityEstimate totalEqSel{SelectivityType{totalSelDbl},
                                           EstimationSource::Heuristics};
            std::vector<SelectivityEstimate> sels;
            sels.push_back(std::move(totalEqSel));
            auto regexSel = heuristicScaledPredSel(inputCard, kTextSearchScalingFactor);
            sels.insert(sels.end(), inExpr->getRegexes().size(), regexSel);
            return disjExponentialBackoff(sels);
        }
        case MatchExpression::MatchType::TYPE_OPERATOR: {
            // Treat each operand in a $type operator as a closed interval. Estimate it by
            // summing the estimates similar to $in because documents of different types are
            // disjunct.
            auto typeExpr = static_cast<const TypeMatchExpression*>(expr);
            double rangeSel = heuristicClosedRangeSel(inputCard).toDouble();
            double totalSelDbl = std::min(typeExpr->typeSet().bsonTypes.size() * rangeSel, 1.0);
            return SelectivityEstimate{SelectivityType{totalSelDbl}, EstimationSource::Heuristics};
        }
        case MatchExpression::MatchType::REGEX:
        case MatchExpression::MatchType::TEXT:
            return heuristicScaledPredSel(inputCard, kTextSearchScalingFactor);
        case MatchExpression::MatchType::WHERE:
        case MatchExpression::MatchType::SIZE:
        case MatchExpression::MatchType::EXPRESSION:
        case MatchExpression::MatchType::GEO:
        case MatchExpression::MatchType::GEO_NEAR:
        case MatchExpression::MatchType::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::MatchType::INTERNAL_2D_POINT_IN_ANNULUS:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_FMOD:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MAX_LENGTH:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MIN_LENGTH:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_TYPE:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MAX_ITEMS:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MIN_ITEMS:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MAX_PROPERTIES:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_MIN_PROPERTIES:
        case MatchExpression::MatchType::INTERNAL_SCHEMA_UNIQUE_ITEMS:
        case MatchExpression::MatchType::BITS_ALL_SET:
        case MatchExpression::MatchType::BITS_ALL_CLEAR:
        case MatchExpression::MatchType::BITS_ANY_SET:
        case MatchExpression::MatchType::BITS_ANY_CLEAR: {
            return heuristicScaledPredSel(inputCard, kDefaultScalingFactor);
        }
        default:
            // Non-leaf expressions that should never appear in this function.
            tassert(9902900, "Unknown inestimable expression.", !heuristicIsEstimable(expr));
    }
    MONGO_UNREACHABLE_TASSERT(9695000);
}

SelectivityEstimate estimateInterval(const Interval& interval, CardinalityEstimate inputCard) {
    if (interval.isEmpty() || interval.isNull()) {
        return zeroSelHeuristic;
    }
    if (interval.isFullyOpen()) {
        return oneSelHeuristic;
    }
    if (interval.isPoint()) {
        if (inputCard <= oneCE) {
            return oneSelHeuristic;
        }
        return heuristicScaledPredSel(inputCard, kEqualityScalingFactor);
    }
    // At this point, we know this interval is a range.

    // We use different heuristic based on whether this range is open or closed.
    if (interval.start.type() == BSONType::minKey || interval.end.type() == BSONType::maxKey ||
        !interval.startInclusive || !interval.endInclusive) {
        return heuristicOpenRangeSel(inputCard);
    }
    return heuristicClosedRangeSel(inputCard);
}

}  // namespace mongo::cost_based_ranker
