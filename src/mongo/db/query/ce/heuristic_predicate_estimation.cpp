/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/ce/heuristic_predicate_estimation.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/bound_utils.h"

namespace mongo::optimizer::ce {

using TypeTags = mongo::sbe::value::TypeTags;

SelectivityType heuristicEqualitySel(const CEType inputCard) {
    uassert(6716604, "Zero cardinality must be handled by the caller.", inputCard > 0.0);
    if (inputCard <= 1.0) {
        // If the input has < 1 values, it cannot be reduced any further by a condition.
        return {1.0};
    }
    return {1.0 / std::sqrt(inputCard._value)};
}

SelectivityType heuristicClosedRangeSel(const CEType inputCard) {
    SelectivityType sel = kInvalidSel;
    if (inputCard < kSmallLimit) {
        sel = kSmallCardClosedRangeSel;
    } else if (inputCard < kMediumLimit) {
        sel = kMediumCardClosedRangeSel;
    } else {
        sel = kLargeCardClosedRangeSel;
    }
    return sel;
}

SelectivityType heuristicOpenRangeSel(const CEType inputCard) {
    SelectivityType sel = kInvalidSel;
    if (inputCard < kSmallLimit) {
        sel = kSmallCardOpenRangeSel;
    } else if (inputCard < kMediumLimit) {
        sel = kMediumCardOpenRangeSel;
    } else {
        sel = kLargeCardOpenRangeSel;
    }
    return sel;
}

SelectivityType heuristicIntervalSel(const IntervalRequirement& interval, const CEType inputCard) {
    SelectivityType sel = kInvalidSel;
    if (interval.isFullyOpen()) {
        sel = {1.0};
    } else if (interval.isEquality()) {
        sel = heuristicEqualitySel(inputCard);
    } else if (interval.getHighBound().isPlusInf() || interval.getLowBound().isMinusInf() ||
               getBoundReqTypeTag(interval.getLowBound()) !=
                   getBoundReqTypeTag(interval.getHighBound())) {
        // The interval has an actual bound only on one of it ends if:
        // - one of the bounds is infinite, or
        // - both bounds are of a different type - this is the case when due to type bracketing
        //   one of the bounds is the lowest/highest value of the previous/next type.
        // TODO: Notice that sometimes type bracketing uses a min/max value from the same type,
        // so sometimes we may not detect an open-ended interval.
        sel = heuristicOpenRangeSel(inputCard);
    } else {
        sel = heuristicClosedRangeSel(inputCard);
    }
    uassert(6716603, "Invalid selectivity.", validSelectivity(sel));
    return sel;
}

CEType heuristicIntervalCard(const IntervalRequirement& interval, const CEType inputCard) {
    return inputCard * heuristicIntervalSel(interval, inputCard);
}

SelectivityType heuristicOperationSel(const Operations op, const CEType inputCard) {
    switch (op) {
        case Operations::Eq:
            return heuristicEqualitySel(inputCard);
        case Operations::Neq:
            return negateSel(heuristicEqualitySel(inputCard));
        case Operations::EqMember:
            // Reached when the query has $in. We don't handle it yet.
            return kDefaultFilterSel;
        case Operations::Gt:
        case Operations::Gte:
        case Operations::Lt:
        case Operations::Lte:
            return heuristicOpenRangeSel(inputCard);
        default:
            MONGO_UNREACHABLE;
    }
}

SelectivityType heuristicIntervalSel(const PathCompare& left,
                                     const PathCompare& right,
                                     const CEType inputCard) {
    if (left.op() == Operations::EqMember || right.op() == Operations::EqMember) {
        // Reached when the query has $in. We don't handle it yet.
        return kDefaultFilterSel;
    }

    bool lowBoundUnknown = false;
    bool highBoundUnknown = false;
    boost::optional<TypeTags> lowBoundReqTypeTag;
    boost::optional<TypeTags> highBoundReqTypeTag;

    for (const auto& compare : {left, right}) {
        switch (compare.op()) {
            case Operations::Eq: {
                // This branch is reached when we have a conjunction of equalities on the same path.
                uassert(6777601,
                        "Expected conjunction of equalities.",
                        left.op() == Operations::Eq && right.op() == Operations::Eq);

                const auto leftConst = left.getVal().cast<Constant>();
                const auto rightConst = right.getVal().cast<Constant>();
                if (leftConst && rightConst && !(*leftConst == *rightConst)) {
                    // Equality comparison on different constants is a contradiction.
                    return {0.0};
                }
                // We can't tell if the equalities result in a contradiction or not, so we use the
                // default equality selectivity.
                return heuristicEqualitySel(inputCard);
            }
            case Operations::Gt:
            case Operations::Gte:
                lowBoundUnknown = lowBoundUnknown || compare.getVal().is<Variable>();
                lowBoundReqTypeTag =
                    getConstTypeTag(compare.getVal()).get_value_or(TypeTags::Nothing);
                break;
            case Operations::Lt:
            case Operations::Lte:
                highBoundUnknown = highBoundUnknown || compare.getVal().is<Variable>();
                highBoundReqTypeTag =
                    getConstTypeTag(compare.getVal()).get_value_or(TypeTags::Nothing);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    if (lowBoundReqTypeTag && highBoundReqTypeTag &&
        (lowBoundReqTypeTag == highBoundReqTypeTag || lowBoundUnknown || highBoundUnknown)) {
        // Interval is closed only if:
        // - it has low and high bounds
        // - bounds are of the same type
        //
        // If bounds are of a different type, it implies that one bound is the
        // lowest/highest value of the previous/next type and has been added for type bracketing
        // purposes. We treat such bounds as infinity.
        //
        // If there are unknown boundaries (Variables), we assume that they are of the same type
        // as the other bound.
        //
        // TODO: Notice that sometimes type bracketing uses a min/max value from the same type,
        // so sometimes we may not detect an open-ended interval.
        return heuristicClosedRangeSel(inputCard);
    }

    if (lowBoundReqTypeTag || highBoundReqTypeTag) {
        return heuristicOpenRangeSel(inputCard);
    }

    MONGO_UNREACHABLE;
}
}  // namespace mongo::optimizer::ce
