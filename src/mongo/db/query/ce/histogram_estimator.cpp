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

#include "mongo/db/query/cqf_command_utils.h"

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::optimizer::ce {

namespace {
/**
 * Returns how many values of the given type are known by the array histogram, and may apply
 * heuristics to adjust the count estimate for the case where the counters don't have enough
 * information for us to accurately estimate the given interval.
 */
CEType estimateIntervalByTypeCount(const stats::ArrayHistogram& ah,
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

    return count;
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

IntervalEstimation analyzeIntervalEstimationMode(const IntervalRequirement& interval) {
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

    /**
     * This struct is used to track an intermediate representation of the intervals in the
     * requirements map. In particular, grouping intervals along each path in the map allows us to
     * determine which paths should be estimated as $elemMatches without relying on a particular
     * order of entries in the requirements map.
     */
    struct SargableConjunct {
        bool includeScalar;
        const stats::ArrayHistogram& histogram;
        std::vector<std::reference_wrapper<const IntervalReqExpr::Node>> intervals;
    };

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

        const auto useFallbackEstimator = [&](const std::string& serializedPath) {
            OPTIMIZER_DEBUG_LOG(
                7151300, 5, "Falling back to heuristic CE", "path"_attr = serializedPath);
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n.ref());
        };

        // Initial first pass through the requirements map to extract information about each path.
        std::map<std::string, SargableConjunct> conjunctRequirements;
        for (const auto& [key, req] : node.getReqMap()) {
            if (req.getIsPerfOnly()) {
                // Ignore perf-only requirements.
                continue;
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
                    // Don't push back the interval for estimation; instead, we use it to change how
                    // we estimate other intervals along this path.
                    conjunctReq.includeScalar = false;
                } else {
                    // We will need to estimate this interval.
                    conjunctReq.intervals.push_back(interval);
                }
                continue;
            }

            // Fallback if there is no histogram.
            auto histogram = _stats->getHistogram(serializedPath);
            if (!histogram) {
                // For now, because of the structure of SargableNode and the implementation of
                // the fallback (currently HeuristicCE), we can't combine heuristic & histogram
                // estimates. In this case, default to Heuristic if we don't have a histogram for
                // any of the predicates.
                return useFallbackEstimator(serializedPath);
            }

            // Add this path to the map. If this is not a 'PathArr' interval, add it to the vector
            // of intervals we will be estimating.
            SargableConjunct sc{!isPathArrInterval, *histogram, {}};
            if (sc.includeScalar) {
                sc.intervals.push_back(interval);
            }
            conjunctRequirements.emplace(serializedPath, std::move(sc));
        }

        std::vector<SelectivityType> topLevelSelectivities;
        for (const auto& [serializedPath, conjunctReq] : conjunctRequirements) {
            const CEType totalCard{_stats->getCardinality()};

            if (conjunctReq.intervals.empty() && !conjunctReq.includeScalar) {
                // In this case there is a single 'PathArr' interval for this field.
                // The selectivity of this interval is: (count of all arrays) / totalCard
                SelectivityType pathArrSel =
                    CEType{conjunctReq.histogram.getArrayCount()} / totalCard;
                topLevelSelectivities.push_back(pathArrSel);
            }

            // Intervals are in DNF.
            for (const IntervalReqExpr::Node& intervalDNF : conjunctReq.intervals) {
                std::vector<SelectivityType> disjSelectivities;

                const auto disjuncts = intervalDNF.cast<IntervalReqExpr::Disjunction>()->nodes();
                for (const auto& disjunct : disjuncts) {
                    const auto& conjuncts = disjunct.cast<IntervalReqExpr::Conjunction>()->nodes();

                    std::vector<SelectivityType> conjSelectivities;
                    for (const auto& conjunct : conjuncts) {
                        const auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();

                        CEType cardinality;
                        if (interval.isFullyOpen()) {
                            // No need to estimate, as we're just going to return all inputs.
                            cardinality = childResult;
                        } else {
                            // Determine how this interval will be estimated.
                            const auto [mode, lowBound, highBound] =
                                analyzeIntervalEstimationMode(interval);
                            switch (mode) {
                                case IntervalEstimationMode::kUseHistogram: {
                                    const auto [lowTag, lowVal] = *getBound(lowBound->get());
                                    if (interval.isEquality()) {
                                        cardinality = estimateCardEq(conjunctReq.histogram,
                                                                     lowTag,
                                                                     lowVal,
                                                                     conjunctReq.includeScalar);
                                    } else {
                                        const auto [highTag, highVal] = *getBound(highBound->get());
                                        cardinality =
                                            estimateCardRange(conjunctReq.histogram,
                                                              lowBound->get().isInclusive(),
                                                              lowTag,
                                                              lowVal,
                                                              highBound->get().isInclusive(),
                                                              highTag,
                                                              highVal,
                                                              conjunctReq.includeScalar);
                                    }
                                    break;
                                }
                                case IntervalEstimationMode::kUseTypeCounts: {
                                    const auto bound = lowBound ? *lowBound : *highBound;
                                    cardinality =
                                        estimateIntervalByTypeCount(conjunctReq.histogram,
                                                                    interval,
                                                                    bound,
                                                                    childResult,
                                                                    conjunctReq.includeScalar);
                                    break;
                                }
                                case IntervalEstimationMode::kFallback: {
                                    return useFallbackEstimator(serializedPath);
                                }
                                default: { MONGO_UNREACHABLE; }
                            }
                        }

                        OPTIMIZER_DEBUG_LOG(7151301,
                                            5,
                                            "Estimated path and interval using histograms.",
                                            "path"_attr = serializedPath,
                                            "interval"_attr =
                                                ExplainGenerator::explainInterval(interval),
                                            "ce"_attr = cardinality._value);

                        // We have to convert the cardinality to a selectivity. The histogram
                        // returns the cardinality for the entire collection; however, fewer records
                        // may be expected at the SargableNode.
                        conjSelectivities.push_back(cardinality / totalCard);
                    }

                    const auto backoff = conjExponentialBackoff(std::move(conjSelectivities));
                    disjSelectivities.push_back(backoff);
                }

                const auto backoff = disjExponentialBackoff(std::move(disjSelectivities));
                OPTIMIZER_DEBUG_LOG(7151303,
                                    5,
                                    "Estimating disjunction on path using histograms",
                                    "path"_attr = serializedPath,
                                    "intervalDNF"_attr =
                                        ExplainGenerator::explainIntervalExpr(intervalDNF),
                                    "selectivity"_attr = backoff._value);
                topLevelSelectivities.push_back(backoff);
            }
        }

        // The elements of the PartialSchemaRequirements map represent an implicit conjunction.
        if (!topLevelSelectivities.empty()) {
            const auto backoff = conjExponentialBackoff(std::move(topLevelSelectivities));
            childResult *= backoff;
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
