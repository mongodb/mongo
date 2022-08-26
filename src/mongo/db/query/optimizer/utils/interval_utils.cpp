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

#include "mongo/db/query/optimizer/utils/interval_utils.h"

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"


namespace mongo::optimizer {

void combineIntervalsDNF(const bool intersect,
                         IntervalReqExpr::Node& target,
                         const IntervalReqExpr::Node& source) {
    if (target == source) {
        // Intervals are the same. Leave target unchanged.
        return;
    }

    if (isIntervalReqFullyOpenDNF(target)) {
        // Intersecting with fully open interval is redundant.
        // Unioning with fully open interval results in a fully-open interval.
        if (intersect) {
            target = source;
        }
        return;
    }

    if (isIntervalReqFullyOpenDNF(source)) {
        // Intersecting with fully open interval is redundant.
        // Unioning with fully open interval results in a fully-open interval.
        if (!intersect) {
            target = source;
        }
        return;
    }

    IntervalReqExpr::NodeVector newDisjunction;
    // Integrate both compound bounds.
    if (intersect) {
        // Intersection is analogous to polynomial multiplication. Using '.' to denote intersection
        // and '+' to denote union. (a.b + c.d) . (e+f) = a.b.e + c.d.e + a.b.f + c.d.f
        // TODO: in certain cases we can simplify further. For example if we only have scalars, we
        // can simplify (-inf, 10) ^ (5, +inf) to (5, 10), but this does not work with arrays.

        for (const auto& sourceConjunction : source.cast<IntervalReqExpr::Disjunction>()->nodes()) {
            const auto& sourceConjunctionIntervals =
                sourceConjunction.cast<IntervalReqExpr::Conjunction>()->nodes();
            for (const auto& targetConjunction :
                 target.cast<IntervalReqExpr::Disjunction>()->nodes()) {
                // TODO: handle case with targetConjunct  fully open
                // TODO: handle case with targetConjunct half-open and sourceConjuct equality.
                // TODO: handle case with both targetConjunct and sourceConjuct equalities
                // (different consts).

                auto newConjunctionIntervals =
                    targetConjunction.cast<IntervalReqExpr::Conjunction>()->nodes();
                std::copy(sourceConjunctionIntervals.cbegin(),
                          sourceConjunctionIntervals.cend(),
                          std::back_inserter(newConjunctionIntervals));
                newDisjunction.emplace_back(IntervalReqExpr::make<IntervalReqExpr::Conjunction>(
                    std::move(newConjunctionIntervals)));
            }
        }
    } else {
        // Unioning is analogous to polynomial addition.
        // (a.b + c.d) + (e+f) = a.b + c.d + e + f
        newDisjunction = target.cast<IntervalReqExpr::Disjunction>()->nodes();
        for (const auto& sourceConjunction : source.cast<IntervalReqExpr::Disjunction>()->nodes()) {
            newDisjunction.push_back(sourceConjunction);
        }
    }
    target = IntervalReqExpr::make<IntervalReqExpr::Disjunction>(std::move(newDisjunction));
}

std::vector<IntervalRequirement> intersectIntervals(const IntervalRequirement& i1,
                                                    const IntervalRequirement& i2) {
    // Handle trivial cases of intersection.
    if (i1.isFullyOpen()) {
        return {i2};
    }
    if (i2.isFullyOpen()) {
        return {i1};
    }

    const ABT& low1 = i1.getLowBound().getBound();
    const ABT& high1 = i1.getHighBound().getBound();
    const ABT& low2 = i2.getLowBound().getBound();
    const ABT& high2 = i2.getHighBound().getBound();

    const auto foldFn = [](ABT expr) {
        // Performs constant folding.
        VariableEnvironment env = VariableEnvironment::build(expr);
        ConstEval instance(env);
        instance.optimize(expr);
        return expr;
    };
    const auto minMaxFn = [](const Operations op, const ABT& v1, const ABT& v2) {
        // Encodes max(v1, v2).
        return make<If>(make<BinaryOp>(op, v1, v2), v1, v2);
    };
    const auto minMaxFn1 = [](const Operations op, const ABT& v1, const ABT& v2, const ABT& v3) {
        // Encodes v1 op v2 ? v3 : v2
        return make<If>(make<BinaryOp>(op, v1, v2), v3, v2);
    };

    // In the simplest case our bound is (max(low1, low2), min(high1, high2)) if none of the bounds
    // are inclusive.
    const ABT maxLow = foldFn(minMaxFn(Operations::Gte, low1, low2));
    const ABT minHigh = foldFn(minMaxFn(Operations::Lte, high1, high2));
    if (foldFn(make<BinaryOp>(Operations::Gt, maxLow, minHigh)) == Constant::boolean(true)) {
        // Low bound is greater than high bound.
        return {};
    }

    const bool low1Inc = i1.getLowBound().isInclusive();
    const bool high1Inc = i1.getHighBound().isInclusive();
    const bool low2Inc = i2.getLowBound().isInclusive();
    const bool high2Inc = i2.getHighBound().isInclusive();

    // We form a "main" result interval which is closed on any side with "agreement" between the two
    // intervals. For example [low1, high1] ^ [low2, high2) -> [max(low1, low2), min(high1, high2))
    BoundRequirement lowBoundMain(low1Inc && low2Inc, maxLow);
    BoundRequirement highBoundMain(high1Inc && high2Inc, minHigh);

    const bool boundsEqual =
        foldFn(make<BinaryOp>(Operations::Eq, maxLow, minHigh)) == Constant::boolean(true);
    if (boundsEqual) {
        if (low1Inc && high1Inc && low2Inc && high2Inc) {
            // Point interval.
            return {{std::move(lowBoundMain), std::move(highBoundMain)}};
        }
        if ((!low1Inc && !low2Inc) || (!high1Inc && !high2Inc)) {
            // Fully open on both sides.
            return {};
        }
    }
    if (low1Inc == low2Inc && high1Inc == high2Inc) {
        // Inclusion matches on both sides.
        return {{std::move(lowBoundMain), std::move(highBoundMain)}};
    }

    // At this point we have intervals without inclusion agreement, for example
    // [low1, high1) ^ (low2, high2]. We have the main result which in this case is the open
    // (max(low1, low2), min(high1, high2)). Then we add an extra closed interval for each side with
    // disagreement. For example for the lower sides we add: [low2 >= low1 ? MaxKey : low1,
    // min(max(low1, low2), min(high1, high2)] This is a closed interval which would reduce to
    // [max(low1, low2), max(low1, low2)] if low2 < low1. If low2 >= low1 the interval reduces to an
    // empty one [MaxKey, min(max(low1, low2), min(high1, high2)] which will return no results from
    // an index scan. We do not know that in general if we do not have constants (we cannot fold).
    //
    // If we can fold the extra interval, we exploit the fact that (max(low1, low2),
    // min(high1, high2)) U [max(low1, low2), max(low1, low2)] is [max(low1, low2), min(high1,
    // high2)) (observe left side is now closed). Then we create a similar auxiliary interval for
    // the right side if there is disagreement on the inclusion. Finally, we attempt to fold both
    // intervals. Should we conclude definitively that they are point intervals, we update the
    // inclusion of the main interval for the respective side.

    std::vector<IntervalRequirement> result;
    const auto addAuxInterval = [&](ABT low, ABT high, BoundRequirement& bound) {
        IntervalRequirement interval{{true, low}, {true, high}};

        const ABT comparison = foldFn(make<BinaryOp>(Operations::Lte, low, high));
        if (comparison == Constant::boolean(true)) {
            if (interval.isEquality()) {
                // We can determine the two bounds are equal.
                bound = {true /*inclusive*/, bound.getBound()};
            } else {
                result.push_back(std::move(interval));
            }
        } else if (!comparison.is<Constant>()) {
            // We cannot determine statically how the two bounds compare.
            result.push_back(std::move(interval));
        }
    };

    if (low1Inc != low2Inc) {
        const ABT low = foldFn(minMaxFn1(
            Operations::Gte, low1Inc ? low2 : low1, low1Inc ? low1 : low2, Constant::maxKey()));
        const ABT high = foldFn(minMaxFn(Operations::Lte, maxLow, minHigh));
        addAuxInterval(std::move(low), std::move(high), lowBoundMain);
    }

    if (high1Inc != high2Inc) {
        const ABT low = foldFn(minMaxFn(Operations::Gte, maxLow, minHigh));
        const ABT high = foldFn(minMaxFn1(Operations::Lte,
                                          high1Inc ? high2 : high1,
                                          high1Inc ? high1 : high2,
                                          Constant::minKey()));
        addAuxInterval(std::move(low), std::move(high), highBoundMain);
    }

    if (!boundsEqual || (lowBoundMain.isInclusive() && highBoundMain.isInclusive())) {
        // We add the main interval to the result as long as it is a valid point interval, or the
        // bounds are not equal.
        result.emplace_back(std::move(lowBoundMain), std::move(highBoundMain));
    }
    return result;
}

boost::optional<IntervalReqExpr::Node> intersectDNFIntervals(
    const IntervalReqExpr::Node& intervalDNF) {
    IntervalReqExpr::NodeVector disjuncts;

    for (const auto& disjunct : intervalDNF.cast<IntervalReqExpr::Disjunction>()->nodes()) {
        const auto& conjuncts = disjunct.cast<IntervalReqExpr::Conjunction>()->nodes();
        uassert(6624149, "Empty disjunct in interval DNF.", !conjuncts.empty());

        std::vector<IntervalRequirement> intersectedIntervalDisjunction;
        bool isEmpty = false;
        bool isFirst = true;

        for (const auto& conjunct : conjuncts) {
            const auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
            if (isFirst) {
                isFirst = false;
                intersectedIntervalDisjunction = {interval};
            } else {
                std::vector<IntervalRequirement> newResult;
                for (const auto& intersectedInterval : intersectedIntervalDisjunction) {
                    auto intersectionResult = intersectIntervals(intersectedInterval, interval);
                    newResult.insert(
                        newResult.end(), intersectionResult.cbegin(), intersectionResult.cend());
                }
                if (newResult.empty()) {
                    // The intersection is empty, there is no need to process the remaining
                    // conjuncts
                    isEmpty = true;
                    break;
                }
                std::swap(intersectedIntervalDisjunction, newResult);
            }
        }
        if (isEmpty) {
            continue;  // The whole conjunct is false (empty interval), skip it.
        }

        for (const auto& interval : intersectedIntervalDisjunction) {
            auto conjunction =
                IntervalReqExpr::make<IntervalReqExpr::Conjunction>(IntervalReqExpr::makeSeq(
                    IntervalReqExpr::make<IntervalReqExpr::Atom>(std::move(interval))));

            // Remove redundant conjunctions.
            if (std::find(disjuncts.cbegin(), disjuncts.cend(), conjunction) == disjuncts.cend()) {
                disjuncts.emplace_back(conjunction);
            }
        }
    }

    if (disjuncts.empty()) {
        return {};
    }
    return IntervalReqExpr::make<IntervalReqExpr::Disjunction>(std::move(disjuncts));
}

bool combineCompoundIntervalsDNF(CompoundIntervalReqExpr::Node& targetIntervals,
                                 const IntervalReqExpr::Node& sourceIntervals,
                                 bool reverseSource) {
    CompoundIntervalReqExpr::NodeVector newDisjunction;

    for (const auto& sourceConjunction :
         sourceIntervals.cast<IntervalReqExpr::Disjunction>()->nodes()) {
        for (const auto& targetConjunction :
             targetIntervals.cast<CompoundIntervalReqExpr::Disjunction>()->nodes()) {
            CompoundIntervalReqExpr::NodeVector newConjunction;

            for (const auto& sourceConjunct :
                 sourceConjunction.cast<IntervalReqExpr::Conjunction>()->nodes()) {
                const auto& sourceInterval =
                    sourceConjunct.cast<IntervalReqExpr::Atom>()->getExpr();
                for (const auto& targetConjunct :
                     targetConjunction.cast<CompoundIntervalReqExpr::Conjunction>()->nodes()) {
                    const auto& targetInterval =
                        targetConjunct.cast<CompoundIntervalReqExpr::Atom>()->getExpr();
                    if (!targetInterval.empty() && !targetInterval.back().isEquality() &&
                        !sourceInterval.isFullyOpen()) {
                        // We do not have an equality prefix. Reject.
                        return false;
                    }

                    auto newInterval = targetInterval;
                    if (reverseSource) {
                        auto newSource = sourceInterval;
                        newSource.reverse();
                        newInterval.push_back(std::move(newSource));
                    } else {
                        newInterval.push_back(sourceInterval);
                    }
                    newConjunction.emplace_back(
                        CompoundIntervalReqExpr::make<CompoundIntervalReqExpr::Atom>(
                            std::move(newInterval)));
                }
            }

            newDisjunction.emplace_back(
                CompoundIntervalReqExpr::make<CompoundIntervalReqExpr::Conjunction>(
                    std::move(newConjunction)));
        }
    }

    targetIntervals = CompoundIntervalReqExpr::make<CompoundIntervalReqExpr::Disjunction>(
        std::move(newDisjunction));
    return true;
}

}  // namespace mongo::optimizer
