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
#include "mongo/db/query/optimizer/utils/abt_compare.h"


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
                                                    const IntervalRequirement& i2,
                                                    const ConstFoldFn& constFold) {
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

    const auto foldFn = [&constFold](ABT expr) {
        constFold(expr);
        return expr;
    };
    const auto minFn = [](const ABT& v1, const ABT& v2) {
        return make<If>(make<BinaryOp>(Operations::Lte, v1, v2), v1, v2);
    };
    const auto maxFn = [](const ABT& v1, const ABT& v2) {
        return make<If>(make<BinaryOp>(Operations::Gte, v1, v2), v1, v2);
    };

    // In the simplest case our bound is (max(low1, low2), min(high1, high2)) if none of the bounds
    // are inclusive.
    const ABT maxLow = foldFn(maxFn(low1, low2));
    const ABT minHigh = foldFn(minFn(high1, high2));
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
    BoundRequirement lowBoundPrimary(low1Inc && low2Inc, maxLow);
    BoundRequirement highBoundPrimary(high1Inc && high2Inc, minHigh);

    const bool boundsEqual =
        foldFn(make<BinaryOp>(Operations::Eq, maxLow, minHigh)) == Constant::boolean(true);
    if (boundsEqual) {
        if (low1Inc && high1Inc && low2Inc && high2Inc) {
            // Point interval.
            return {{std::move(lowBoundPrimary), std::move(highBoundPrimary)}};
        }
        if ((!low1Inc && !low2Inc) || (!high1Inc && !high2Inc)) {
            // Fully open on both sides.
            return {};
        }
    }
    if (low1Inc == low2Inc && high1Inc == high2Inc) {
        // Inclusion matches on both sides.
        return {{std::move(lowBoundPrimary), std::move(highBoundPrimary)}};
    }

    // At this point we have intervals without inclusion agreement, for example
    // [low1, high1) ^ (low2, high2]. We have the primary interval which in this case is the open
    // (max(low1, low2), min(high1, high2)). Then we add an extra closed interval for each side with
    // disagreement. For example for the lower sides we add: [indicator ? low1 : MaxKey, low1]. This
    // is a closed interval which would reduce to [low1, low1] if low1 > low2 and the intervals
    // intersect and are non-empty. If low2 >= low1 the interval reduces to an empty one,
    // [MaxKey, low1], which will return no results from an index scan. We do not know that in
    // general if we do not have constants (we cannot fold).
    //
    // If we can fold the aux interval, we combine the aux interval into the primary one, which
    // would yield [low1, min(high1, high2)) if we can prove that low1 > low2. Then we create a
    // similar auxiliary interval for the right side if there is disagreement on the inclusion.
    // We'll attempt to fold both intervals. Should we conclude definitively that they are
    // point intervals, we update the inclusion of the main interval for the respective side.

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

    /*
     * An auxiliary interval should resolve to a non-empty interval if the original intervals we're
     * intersecting overlap and produce something non-empty. Below we create an overlap indicator,
     * which tells us if the intervals overlap.
     *
     * For intersection, the pair [1,2) and [2, 3] does not overlap, while [1,2] and [2, 3] does. So
     * we need to adjust our comparisons depending on if the bounds are both inclusive or not.
     */
    const Operations cmpLows = low1Inc && low2Inc ? Operations::Lte : Operations::Lt;
    const Operations cmpLow1High2 = low1Inc && high2Inc ? Operations::Lte : Operations::Lt;
    const Operations cmpLow2High1 = low2Inc && high1Inc ? Operations::Lte : Operations::Lt;
    const Operations cmpHighs = high1Inc && high2Inc ? Operations::Lte : Operations::Lt;
    /*
     * Our final overlap indicator is as follows (using < or <= depending on inclusiveness)
     * (low1,high1) ^ (low2,high2) overlap if:
     * low2 < low1 < high2 || low2 < high1 < high2 || low1 < low2 < high1 || low1 < high2 < high1
     * As long as both intervals are non-empty.
     *
     * This covers the four cases:
     *      1. int1 intersects int2 from below, ex: (1,3) ^ (2,4)
     *      2. int1 intersects int2 from above, ex: (2,4) ^ (1,3)
     *      3. int1 is a subset of int2, ex: (2,3) ^ (1,4)
     *      4. int2 is a subset of int1, ex: (1,4) ^ (2,3)
     */
    ABT int1NonEmpty =
        make<BinaryOp>(low1Inc && high1Inc ? Operations::Lte : Operations::Lt, low1, high1);
    ABT int2NonEmpty =
        make<BinaryOp>(low2Inc && high2Inc ? Operations::Lte : Operations::Lt, low2, high2);
    ABT overlapCondition =
        make<BinaryOp>(Operations::Or,
                       make<BinaryOp>(Operations::Or,
                                      make<BinaryOp>(Operations::And,
                                                     make<BinaryOp>(cmpLows, low2, low1),
                                                     make<BinaryOp>(cmpLow1High2, low1, high2)),
                                      make<BinaryOp>(Operations::And,
                                                     make<BinaryOp>(cmpLow2High1, low2, high1),
                                                     make<BinaryOp>(cmpHighs, high1, high2))),
                       make<BinaryOp>(Operations::Or,
                                      make<BinaryOp>(Operations::And,
                                                     make<BinaryOp>(cmpLows, low1, low2),
                                                     make<BinaryOp>(cmpLow2High1, low2, high1)),
                                      make<BinaryOp>(Operations::And,
                                                     make<BinaryOp>(cmpLow1High2, low1, high2),
                                                     make<BinaryOp>(cmpHighs, high2, high1))));
    overlapCondition = make<BinaryOp>(
        Operations::And,
        std::move(overlapCondition),
        make<BinaryOp>(Operations::And, std::move(int1NonEmpty), std::move(int2NonEmpty)));

    /*
     * It's possible our aux indicators could be simplified. For example, a more concise indicator
     * for [low1, high1] ^ (low2, high2] might be int1_nonempty && (int2 contains low1). This
     * condition implies the intervals are non-empty and overlap, meaning the intersection is
     * non-empty. It also implies that low1 > low2, meaning the inclusive bound wins.
     */
    if (low1Inc != low2Inc) {
        ABT incBound = low1Inc ? low1 : low2;
        ABT nonIncBound = low1Inc ? low2 : low1;

        // Our aux interval should be non-empty if overlap_indicator && (incBound > nonIncBound)
        ABT auxCondition =
            make<BinaryOp>(Operations::And,
                           overlapCondition,
                           make<BinaryOp>(Operations::Gt, incBound, std::move(nonIncBound)));
        ABT low = foldFn(make<If>(std::move(auxCondition), incBound, Constant::maxKey()));
        ABT high = std::move(incBound);
        addAuxInterval(std::move(low), std::move(high), lowBoundPrimary);
    }

    if (high1Inc != high2Inc) {
        ABT incBound = high1Inc ? high1 : high2;
        ABT nonIncBound = high1Inc ? high2 : high1;

        ABT low = incBound;
        // Our aux interval should be non-empty if overlap_indicator && (incBound < nonIncBound)
        ABT auxCondition =
            make<BinaryOp>(Operations::And,
                           overlapCondition,
                           make<BinaryOp>(Operations::Lt, incBound, std::move(nonIncBound)));
        ABT high =
            foldFn(make<If>(std::move(auxCondition), std::move(incBound), Constant::minKey()));
        addAuxInterval(std::move(low), std::move(high), highBoundPrimary);
    }

    if (!boundsEqual || (lowBoundPrimary.isInclusive() && highBoundPrimary.isInclusive())) {
        // We add the main interval to the result as long as it is a valid point interval, or the
        // bounds are not equal.
        result.emplace_back(std::move(lowBoundPrimary), std::move(highBoundPrimary));
    }
    return result;
}

boost::optional<IntervalReqExpr::Node> intersectDNFIntervals(
    const IntervalReqExpr::Node& intervalDNF, const ConstFoldFn& constFold) {
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
                    auto intersectionResult =
                        intersectIntervals(intersectedInterval, interval, constFold);
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

void padCompoundIntervalsDNF(CompoundIntervalReqExpr::Node& targetIntervals,
                             const bool reverseSource) {
    CompoundIntervalReqExpr::NodeVector newDisjunction;

    for (const auto& targetConjunction :
         targetIntervals.cast<CompoundIntervalReqExpr::Disjunction>()->nodes()) {
        CompoundIntervalReqExpr::NodeVector newConjunction;

        for (const auto& targetConjunct :
             targetConjunction.cast<CompoundIntervalReqExpr::Conjunction>()->nodes()) {
            const auto& targetInterval =
                targetConjunct.cast<CompoundIntervalReqExpr::Atom>()->getExpr();

            IntervalRequirement sourceInterval;
            if (!targetInterval.empty()) {
                const auto& lastInterval = targetInterval.back();
                if (!lastInterval.getLowBound().isInclusive()) {
                    sourceInterval.getLowBound() = {false /*inclusive*/, Constant::maxKey()};
                }
                if (!lastInterval.getHighBound().isInclusive()) {
                    sourceInterval.getHighBound() = {false /*inclusive*/, Constant::minKey()};
                }
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

        newDisjunction.emplace_back(
            CompoundIntervalReqExpr::make<CompoundIntervalReqExpr::Conjunction>(
                std::move(newConjunction)));
    }

    targetIntervals = CompoundIntervalReqExpr::make<CompoundIntervalReqExpr::Disjunction>(
        std::move(newDisjunction));
}

/**
 * Transport which updates an interval to be in a normal form. Children of each conjunction and
 * disjunction node are consistently ordered. We order the Atoms first by low bound, then by high
 * bound.
 */
class IntervalNormalizer {
public:
    void transport(const IntervalReqExpr::Atom& node) {
        // Noop.
    }

    void transport(IntervalReqExpr::Conjunction& node,
                   std::vector<IntervalReqExpr::Node>& children) {
        sortChildren(children);
    }

    void transport(IntervalReqExpr::Disjunction& node,
                   std::vector<IntervalReqExpr::Node>& children) {
        sortChildren(children);
    }

    void normalize(IntervalReqExpr::Node& intervals) {
        return algebra::transport<false>(intervals, *this);
    }

private:
    void sortChildren(std::vector<IntervalReqExpr::Node>& children) {
        struct Comparator {
            bool operator()(const IntervalReqExpr::Node& i1,
                            const IntervalReqExpr::Node& i2) const {
                return compareIntervalExpr(i1, i2) < 0;
            }
        };
        std::sort(children.begin(), children.end(), Comparator{});
    }
};

void normalizeIntervals(IntervalReqExpr::Node& intervals) {
    IntervalNormalizer{}.normalize(intervals);
}

boost::optional<ABT> coerceIntervalToPathCompareEqMember(const IntervalReqExpr::Node& interval) {
    // Create the array that EqMember will use to hold the members.
    auto [eqMembersTag, eqMembersVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard guard{eqMembersTag, eqMembersVal};
    auto eqMembersArray = sbe::value::getArrayView(eqMembersVal);

    // An EqMember is a disjunction of conjunctions of atoms (point intervals). For example [1, 1] U
    // [2, 2] U [3, 3] However each conjunction should only have one atom child, so we can think of
    // it as a disjunction of point intervals instead.
    if (const auto disj = interval.cast<IntervalReqExpr::Disjunction>()) {
        // We only make an EqMember if we have 2 or more comparisons.
        if (disj->nodes().size() < 2) {
            return boost::none;
        }

        for (const auto& child : disj->nodes()) {
            if (!child.is<IntervalReqExpr::Conjunction>()) {
                return boost::none;
            }

            // Check that the conjunction has one atom child.
            const auto conjChild = child.cast<IntervalReqExpr::Conjunction>();
            if (conjChild->nodes().size() != 1 ||
                !conjChild->nodes().front().is<IntervalReqExpr::Atom>()) {
                return boost::none;
            }

            // Check that the atom is a point interval, and the bound is a constant.
            const auto atomChild = conjChild->nodes().front().cast<IntervalReqExpr::Atom>();
            if (!atomChild->getExpr().isEquality() ||
                !atomChild->getExpr().getLowBound().getBound().is<Constant>()) {
                return boost::none;
            }

            const auto constAtomChildPair =
                atomChild->getExpr().getLowBound().getBound().cast<Constant>()->get();

            // Make a copy of the point bound, insert it into our EqMember members.
            const auto newEqMember = copyValue(constAtomChildPair.first, constAtomChildPair.second);
            eqMembersArray->push_back(newEqMember.first, newEqMember.second);
        }

        // If we got to this point, we have successfully coerced the interval into an EqMember!
        // Reset the guard so the members array doesn't get deleted.
        guard.reset();
        return make<PathCompare>(Operations::EqMember, make<Constant>(eqMembersTag, eqMembersVal));
    }
    return boost::none;
}

bool areCompoundIntervalsEqualities(const CompoundIntervalRequirement& intervals) {
    for (const auto& interval : intervals) {
        if (!interval.isEquality()) {
            return false;
        }
    }
    return true;
}

bool isSimpleRange(const CompoundIntervalReqExpr::Node& interval) {
    if (const auto singularInterval = CompoundIntervalReqExpr::getSingularDNF(interval);
        singularInterval && !areCompoundIntervalsEqualities(*singularInterval)) {
        return true;
    }
    return false;
}

}  // namespace mongo::optimizer
