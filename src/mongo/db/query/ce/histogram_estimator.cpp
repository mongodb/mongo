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

#include "mongo/db/query/ce/histogram_estimator.h"

#include "mongo/db/pipeline/abt/utils.h"

#include "mongo/db/query/ce/bound_utils.h"
#include "mongo/db/query/ce/heuristic_predicate_estimation.h"
#include "mongo/db/query/ce/histogram_predicate_estimation.h"
#include "mongo/db/query/ce/sel_tree_utils.h"

#include "mongo/db/query/cqf_command_utils.h"

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::ce {

namespace {
/**
 * Returns the selectivity of the given type according to the array histogram type counts, and may
 * apply heuristics to adjust the count estimate for the case where the counters don't have enough
 * information for us to accurately estimate the given interval.
 */
SelectivityType estimateIntervalByTypeSel(const stats::ArrayHistogram& ah,
                                          const IntervalRequirement& interval,
                                          const BoundRequirement& bound,
                                          CEType childResult,
                                          bool includeScalar) {
    const auto [tag, val] = *getBound(bound);
    CEType count{0.0};

    if (includeScalar) {
        // Include scalar type count estimate.
        switch (tag) {
            case sbe::value::TypeTags::Boolean: {
                // In the case of booleans, we have separate true/false counters we can use.
                const bool estTrue = sbe::value::bitcastTo<bool>(val);
                if (estTrue) {
                    count = {ah.getTrueCount()};
                } else {
                    count = {ah.getFalseCount()};
                }
                break;
            }
            case sbe::value::TypeTags::Array: {
                // Note that if we are asked by the optimizer to estimate an interval whose bounds
                // are arrays, this means we are trying to estimate equality on nested arrays. In
                // this case, we do not want to include the "scalar" type counter for the array
                // type, because this will cause us to estimate the nested array case as counting
                // all arrays, regardless of whether or not they are nested.
                break;
            }
            case sbe::value::TypeTags::Null: {
                // The predicate {$eq: null} matches both missing and null values.
                count = {ah.getTypeCount(sbe::value::TypeTags::Nothing)};
                count += {ah.getTypeCount(sbe::value::TypeTags::Null)};
                break;
            }
            default: {
                // We know the total count of values for this type; however, the interval given will
                // likely not include all values. We therefore heuristically apply a default
                // selectivity to the count of values of that type.
                if (const CEType tc{ah.getTypeCount(tag)}; tc > 0.0) {
                    count = heuristicIntervalCard(interval, tc);
                }
            }
        }
    }

    if (ah.isArray()) {
        // If this histogram includes an array counter for this type, add its value to the estimate.
        const CEType tagArrCount{ah.getArrayTypeCount(tag)};
        if (tagArrCount > 0.0) {
            switch (tag) {
                case sbe::value::TypeTags::Boolean: {
                    // We have a count of all arrays that contain any booleans and we assume that
                    // half of them will match for true and the other half will match for false. As
                    // a note, we could have arrays which contain both values and match in both
                    // cases.
                    count += kDefaultArrayBoolSel * tagArrCount;
                    break;
                }
                case sbe::value::TypeTags::Null: {
                    // We have an exact count of arrays that contain nulls, so we want to return it.
                    count += tagArrCount;
                    break;
                }
                default: {
                    // We heuristically assume a default selectivity for the given tag.
                    count += heuristicIntervalCard(interval, tagArrCount);
                }
            }
        }
    }

    return getSelectivity(ah, count);
}

/**
 * Estimates the selectivity of a given 'interval' according to one of the possible estimation
 * modes. Note that if 'histogram' is null or if the 'interval' is incompatible with histogram
 * estimation, we fall back to heuristic estimation.
 */
SelectivityType estimateInterval(const stats::ArrayHistogram* histogram,
                                 const IntervalRequirement& interval,
                                 bool includeScalar,
                                 CEType childResult) {
    if (interval.isFullyOpen()) {
        // No need to estimate, as we're just going to return all inputs.
        return {1.0};
    } else {
        // Determine how this interval will be estimated.
        const auto [mode, lowBound, highBound] = analyzeIntervalEstimationMode(histogram, interval);
        switch (mode) {
            case IntervalEstimationMode::kUseHistogram: {
                const auto [lowTag, lowVal] = *getBound(lowBound->get());

                if (interval.isEquality()) {
                    return estimateSelEq(*histogram, lowTag, lowVal, includeScalar);
                }

                // This is a range predicate.
                const auto [highTag, highVal] = *getBound(highBound->get());
                return estimateSelRange(*histogram,
                                        lowBound->get().isInclusive(),
                                        lowTag,
                                        lowVal,
                                        highBound->get().isInclusive(),
                                        highTag,
                                        highVal,
                                        includeScalar);
            }
            case IntervalEstimationMode::kUseTypeCounts: {
                const auto bound = lowBound ? *lowBound : *highBound;
                return estimateIntervalByTypeSel(
                    *histogram, interval, bound, childResult, includeScalar);
            }
            case IntervalEstimationMode::kFallback: {
                // Fall back to heuristic estimation for this interval.
                // TODO SERVER-67498: We want to use a per-interval fallback which depends on
                // _fallbackCE, rather than an explicit call to heuristic estimation here, in order
                // to parametrize the fallback logic and allow it to be set at the time when the
                // estimator is constructed. For now, we use this because we need more refactoring
                // to enable this behavior.
                return heuristicIntervalSel(interval, childResult);
            }
            default: {
                MONGO_UNREACHABLE;
            }
        }
    }
}

/**
 * This transport combines chains of PathGets and PathTraverses into an MQL-like string path.
 */
class PathDescribeTransport {
public:
    std::string transport(const PathTraverse& /*node*/, std::string childResult) {
        return childResult;
    }

    std::string transport(const PathGet& node, std::string childResult) {
        return str::stream() << node.name() << (childResult.length() > 0 ? "." : "") << childResult;
    }

    std::string transport(const EvalFilter& node, std::string pathResult, std::string inputResult) {
        return pathResult;
    }

    std::string transport(const PathIdentity& node) {
        return "";
    }

    template <typename T, typename... Ts>
    std::string transport(const T& node, Ts&&... /* args */) {
        uasserted(6903900, "Unexpected node in path serialization.");
    }
};

std::string serializePath(const ABT& path) {
    PathDescribeTransport pdt;
    auto str = algebra::transport<false>(path, pdt);
    return str;
}
}  // namespace

IntervalEstimation analyzeIntervalEstimationMode(const stats::ArrayHistogram* histogram,
                                                 const IntervalRequirement& interval) {
    if (!histogram) {
        return {kFallback, boost::none, boost::none};
    }

    const auto& lowBound = interval.getLowBound();
    const auto lowTag = getBoundReqTypeTag(lowBound);
    if (!lowTag) {
        return {kFallback, boost::none, boost::none};
    }

    const auto& highBound = interval.getHighBound();
    const auto highTag = getBoundReqTypeTag(highBound);
    if (!highTag) {
        return {kFallback, boost::none, boost::none};
    }

    // Check if this interval deals with a type that should be estimated via histograms. We may get
    // an interval where one bound is histogrammable but the other is not, as in the case where we
    // have an upper or lower bound which is exclusive and is the first value in the next
    // type-bracket which is not histogrammable.
    if (stats::canEstimateTypeViaHistogram(*lowTag) ||
        stats::canEstimateTypeViaHistogram(*highTag)) {
        return {kUseHistogram,
                boost::make_optional(std::reference_wrapper(lowBound)),
                boost::make_optional(std::reference_wrapper(highBound))};
    }

    // If neither type is histogrammable, we may still be able to estimate this interval using type
    // counts; check if this interval includes only values of one type.
    if (lowTag == highTag || isIntervalSubsetOfType(interval, *lowTag)) {
        return {
            kUseTypeCounts, boost::make_optional(std::reference_wrapper(lowBound)), boost::none};
    } else if (isIntervalSubsetOfType(interval, *highTag)) {
        return {
            kUseTypeCounts, boost::none, boost::make_optional(std::reference_wrapper(highBound))};
    }

    return {kFallback, boost::none, boost::none};
}

class HistogramTransport {
public:
    HistogramTransport(std::shared_ptr<stats::CollectionStatistics> stats,
                       std::unique_ptr<cascades::CardinalityEstimator> fallbackCE)
        : _stats(stats),
          _fallbackCE(std::move(fallbackCE)),
          _arrayOnlyInterval(*defaultConvertPathToInterval(make<PathArr>())) {}

    CEType transport(const ABT& n,
                     const ScanNode& node,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     CEType /*bindResult*/) {
        return {_stats->getCardinality()};
    }

    CEType transport(const ABT& n,
                     const SargableNode& node,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        // Early out and return 0 since we don't expect to get more results.
        if (childResult == 0.0) {
            return {0.0};
        }

        SelectivityTreeBuilder selTreeBuilder;
        selTreeBuilder.pushDisj();
        PSRExpr::visitDisjuncts(node.getReqMap().getRoot(),
                                [&](const PSRExpr::Node& n, const size_t) {
                                    estimateConjunct(n, selTreeBuilder, childResult);
                                });

        if (auto selTree = selTreeBuilder.finish()) {
            const SelectivityType topLevelSel = estimateSelectivityTree(*selTree);
            childResult *= topLevelSel;
        }

        OPTIMIZER_DEBUG_LOG(7151304,
                            5,
                            "Final estimate for SargableNode using histograms.",
                            "node"_attr = ExplainGenerator::explainV2(n),
                            "cardinality"_attr = childResult._value);
        return childResult;
    }

    CEType transport(const ABT& n,
                     const RootNode& node,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*refsResult*/) {
        // Root node does not change cardinality.
        return childResult;
    }

    /**
     * Use fallback for other ABT types.
     */
    template <typename T, typename... Ts>
    CEType transport(const ABT& n,
                     const T& /*node*/,
                     const Metadata& metadata,
                     const cascades::Memo& memo,
                     const properties::LogicalProps& logicalProps,
                     Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n.ref());
        }
        return {0.0};
    }

private:
    /**
     * This struct is used to track an intermediate representation of the intervals in the
     * requirements map. In particular, grouping intervals along each path in the map allows us to
     * determine which paths should be estimated as $elemMatches without relying on a particular
     * order of entries in the requirements map.
     */
    struct SargableConjunct {
        bool includeScalar;
        const stats::ArrayHistogram* histogram;
        std::vector<std::reference_wrapper<const IntervalReqExpr::Node>> intervals;

        bool isPathArr() const {
            return histogram && !includeScalar && intervals.empty();
        }
    };

    /**
     * Estimate the selectivities of a PartialSchemaRequirements conjunction. It is assumed that the
     * conjuncts are all PartialSchemaEntries. The entire conjunction must be estimated at the same
     * time because some paths may have multiple requirements which should be considered together.
     */
    void estimateConjunct(const PSRExpr::Node& conj,
                          SelectivityTreeBuilder& selTreeBuilder,
                          const CEType& childResult) {
        // Initial first pass through the requirements map to extract information about each path.
        std::map<std::string, SargableConjunct> conjunctRequirements;
        PSRExpr::visitConjuncts(conj, [&](const PSRExpr::Node& atom, const size_t) {
            PSRExpr::visitAtom(atom, [&](const PartialSchemaEntry& e) {
                const auto& [key, req] = e;
                if (req.getIsPerfOnly()) {
                    // Ignore perf-only requirements.
                    return;
                }

                const auto serializedPath = serializePath(key._path.ref());
                const auto& interval = req.getIntervals();
                const bool isPathArrInterval =
                    (_arrayOnlyInterval == interval) && !pathEndsInTraverse(key._path.ref());

                // Check if we have already seen this path.
                if (auto conjunctIt = conjunctRequirements.find({serializedPath});
                    conjunctIt != conjunctRequirements.end()) {
                    auto& conjunctReq = conjunctIt->second;
                    if (isPathArrInterval) {
                        // We should estimate this path's intervals using $elemMatch semantics.
                        // Don't push back the interval for estimation; instead, we use it to change
                        // how we estimate other intervals along this path.
                        conjunctReq.includeScalar = false;
                    } else {
                        // We will need to estimate this interval.
                        conjunctReq.intervals.push_back(interval);
                    }
                    return;
                }

                // Get histogram from statistics if it exists, or null if not.
                const auto* histogram = _stats->getHistogram(serializedPath);

                // Add this path to the map. If this is not a 'PathArr' interval, add it to the
                // vector of intervals we will be estimating.
                SargableConjunct sc{!isPathArrInterval, histogram, {}};
                if (sc.includeScalar) {
                    sc.intervals.push_back(interval);
                }
                conjunctRequirements.emplace(serializedPath, std::move(sc));
            });
        });

        selTreeBuilder.pushConj();
        for (const auto& conjunctRequirement : conjunctRequirements) {
            const auto& serializedPath = conjunctRequirement.first;
            const auto& conjunctReq = conjunctRequirement.second;

            if (conjunctReq.isPathArr()) {
                // If there is a single 'PathArr' interval for this field, we should estimate this
                // as the selectivity of array values.
                selTreeBuilder.atom(getArraySelectivity(*conjunctReq.histogram));
            }

            EstimateIntervalSelFn estimateIntervalFn = [&](SelectivityTreeBuilder& b,
                                                           const IntervalRequirement& interval) {
                const auto selectivity = estimateInterval(
                    conjunctReq.histogram, interval, conjunctReq.includeScalar, childResult);
                selTreeBuilder.atom(selectivity);
                OPTIMIZER_DEBUG_LOG(7151301,
                                    5,
                                    "Estimated path and interval as:",
                                    "path"_attr = serializedPath,
                                    "interval"_attr = ExplainGenerator::explainInterval(interval),
                                    "selectivity"_attr = selectivity._value);
            };
            IntervalSelectivityTreeBuilder intervalSelBuilder{selTreeBuilder, estimateIntervalFn};

            for (const IntervalReqExpr::Node& intervalDNF : conjunctReq.intervals) {
                intervalSelBuilder.build(intervalDNF);
            }
        }
        selTreeBuilder.pop();
    }

    std::shared_ptr<stats::CollectionStatistics> _stats;
    std::unique_ptr<cascades::CardinalityEstimator> _fallbackCE;

    // This is a special interval indicating that we expect to use $elemMatch semantics when
    // estimating the current path.
    const IntervalReqExpr::Node _arrayOnlyInterval;
};

HistogramEstimator::HistogramEstimator(std::shared_ptr<stats::CollectionStatistics> stats,
                                       std::unique_ptr<cascades::CardinalityEstimator> fallbackCE)
    : _transport(std::make_unique<HistogramTransport>(stats, std::move(fallbackCE))) {}

HistogramEstimator::~HistogramEstimator() {}

CEType HistogramEstimator::deriveCE(const Metadata& metadata,
                                    const cascades::Memo& memo,
                                    const properties::LogicalProps& logicalProps,
                                    const ABT::reference_type logicalNodeRef) const {
    return algebra::transport<true>(
        logicalNodeRef, *this->_transport, metadata, memo, logicalProps);
}

}  // namespace mongo::optimizer::ce
