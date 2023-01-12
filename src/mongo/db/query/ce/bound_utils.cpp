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

#include "mongo/db/query/ce/bound_utils.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"

namespace mongo::optimizer::ce {

boost::optional<std::pair<sbe::value::TypeTags, sbe::value::Value>> getConstTypeVal(
    const ABT& abt) {
    if (const auto* constant = abt.cast<Constant>(); constant) {
        return constant->get();
    }
    return boost::none;
};

boost::optional<sbe::value::TypeTags> getConstTypeTag(const ABT& abt) {
    if (auto maybeConstVal = getConstTypeVal(abt); maybeConstVal) {
        return maybeConstVal->first;
    }
    return boost::none;
};

boost::optional<std::pair<sbe::value::TypeTags, sbe::value::Value>> getBound(
    const BoundRequirement& boundReq) {
    const ABT& bound = boundReq.getBound();
    if (bound.is<Constant>()) {
        return getConstTypeVal(bound);
    }
    return boost::none;
};

boost::optional<sbe::value::TypeTags> getBoundReqTypeTag(const BoundRequirement& boundReq) {
    if (const auto bound = getBound(boundReq); bound) {
        return bound->first;
    }
    return boost::none;
}

IntervalRequirement getMinMaxIntervalForType(sbe::value::TypeTags type) {
    // Note: This function works based on the assumption that there are no intervals that include
    // values from more than one type. That is why the MinMax interval of a type will include all
    // possible intervals over that type.

    auto&& [min, minInclusive] = getMinMaxBoundForType(true /*isMin*/, type);
    tassert(7051103, str::stream() << "Type " << type << " has no minimum", min);

    auto&& [max, maxInclusive] = getMinMaxBoundForType(false /*isMin*/, type);
    tassert(7051104, str::stream() << "Type " << type << " has no maximum", max);

    return IntervalRequirement{BoundRequirement(minInclusive, *min),
                               BoundRequirement(maxInclusive, *max)};
}

bool isIntervalSubsetOfType(const IntervalRequirement& interval, sbe::value::TypeTags type) {
    // Create a conjunction of the interval and the min-max interval for the type as input for the
    // intersection function.
    auto intervals =
        IntervalReqExpr::make<IntervalReqExpr::Disjunction>(IntervalReqExpr::NodeVector{
            IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::NodeVector{
                IntervalReqExpr::make<IntervalReqExpr::Atom>(interval),
                IntervalReqExpr::make<IntervalReqExpr::Atom>(getMinMaxIntervalForType(type))})});

    return intersectDNFIntervals(intervals, ConstEval::constFold).has_value();
}

}  // namespace mongo::optimizer::ce
