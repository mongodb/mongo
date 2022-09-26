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

#include "mongo/db/exec/sbe/abt/abt_lower.h"

#include "mongo/db/query/ce/ce_histogram.h"
#include "mongo/db/query/ce/collection_statistics_impl.h"
#include "mongo/db/query/ce/histogram_estimation.h"

#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

#include "mongo/db/pipeline/abt/utils.h"

namespace mongo::optimizer::cascades {

using namespace properties;

namespace {

/**
 * This transport combines chains of PathGets and PathTraverses into an MQL-like string path.
 */
class PathDescribeTransport {
public:
    std::string transport(const optimizer::PathTraverse& /*node*/, std::string childResult) {
        return childResult;
    }

    std::string transport(const optimizer::PathGet& node, std::string childResult) {
        return str::stream() << node.name() << (childResult.length() > 0 ? "." : "") << childResult;
    }

    std::string transport(const optimizer::EvalFilter& node,
                          std::string pathResult,
                          std::string inputResult) {
        return pathResult;
    }

    std::string transport(const optimizer::PathIdentity& node) {
        return "";
    }

    template <typename T, typename... Ts>
    std::string transport(const T& node, Ts&&... /* args */) {
        uasserted(6903900, "Unexpected node in path serialization.");
    }
};

std::string serializePath(const optimizer::ABT& path) {
    PathDescribeTransport pdt;
    auto str = optimizer::algebra::transport<false>(path, pdt);
    return str;
}

}  // namespace

class CEHistogramTransportImpl {
public:
    CEHistogramTransportImpl(std::shared_ptr<ce::CollectionStatistics> stats,
                             std::unique_ptr<CEInterface> fallbackCE)
        : _stats(stats),
          _fallbackCE(std::move(fallbackCE)),
          _arrayOnlyInterval(*defaultConvertPathToInterval(make<PathArr>())) {}

    ~CEHistogramTransportImpl() {}

    CEType transport(const ABT& n,
                     const ScanNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType /*bindResult*/) {
        return _stats->getCardinality();
    }

    /**
     * This struct is used to track an intermediate representation of the intervals in the
     * requirements map. In particular, grouping intervals along each path in the map allows us to
     * determine which paths should be estimated as $elemMatches without relying on a particular
     * order of entries in the requirements map.
     */
    struct SargableConjunct {
        bool includeScalar;
        const ce::ArrayHistogram& histogram;
        std::vector<std::reference_wrapper<const IntervalReqExpr::Node>> intervals;
    };

    CEType transport(const ABT& n,
                     const SargableNode& node,
                     const Metadata& metadata,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        // Early out and return 0 since we don't expect to get more results.
        if (childResult == 0.0) {
            return 0.0;
        }

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
                return _fallbackCE->deriveCE(metadata, memo, logicalProps, n.ref());
            }

            // Add this path to the map. If this is not a 'PathArr' interval, add it to the vector
            // of intervals we will be estimating.
            SargableConjunct sc{!isPathArrInterval, *histogram, {}};
            if (sc.includeScalar) {
                sc.intervals.push_back(interval);
            }
            conjunctRequirements.emplace(serializedPath, std::move(sc));
        }

        std::vector<double> topLevelSelectivities;
        for (const auto& [_, conjunctReq] : conjunctRequirements) {
            const CEType totalCard = _stats->getCardinality();

            if (conjunctReq.intervals.empty() && !conjunctReq.includeScalar) {
                // In this case there is a single 'PathArr' interval for this field.
                // The selectivity of this interval is: (count of all arrays) / totalCard
                double pathArrSel = conjunctReq.histogram.getArrayCount() / totalCard;
                topLevelSelectivities.push_back(pathArrSel);
            }

            // Intervals are in DNF.
            for (const IntervalReqExpr::Node& intervalDNF : conjunctReq.intervals) {
                std::vector<double> disjSelectivities;

                const auto disjuncts = intervalDNF.cast<IntervalReqExpr::Disjunction>()->nodes();
                for (const auto& disjunct : disjuncts) {
                    const auto& conjuncts = disjunct.cast<IntervalReqExpr::Conjunction>()->nodes();

                    std::vector<double> conjSelectivities;
                    for (const auto& conjunct : conjuncts) {
                        const auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
                        auto cardinality =
                            ce::estimateIntervalCardinality(conjunctReq.histogram,
                                                            interval,
                                                            childResult,
                                                            conjunctReq.includeScalar);

                        // We have to convert the cardinality to a selectivity. The histogram
                        // returns the cardinality for the entire collection; however, fewer records
                        // may be expected at the SargableNode.
                        conjSelectivities.push_back(cardinality / totalCard);
                    }

                    auto backoff = ce::conjExponentialBackoff(std::move(conjSelectivities));
                    disjSelectivities.push_back(backoff);
                }

                auto backoff = ce::disjExponentialBackoff(std::move(disjSelectivities));
                topLevelSelectivities.push_back(backoff);
            }
        }

        // The elements of the PartialSchemaRequirements map represent an implicit conjunction.
        if (!topLevelSelectivities.empty()) {
            auto backoff = ce::conjExponentialBackoff(std::move(topLevelSelectivities));
            childResult *= backoff;
        }
        return childResult;
    }

    CEType transport(const ABT& n,
                     const RootNode& node,
                     const Metadata& metadata,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
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
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _fallbackCE->deriveCE(metadata, memo, logicalProps, n.ref());
        }
        return 0.0;
    }

private:
    std::shared_ptr<ce::CollectionStatistics> _stats;
    std::unique_ptr<CEInterface> _fallbackCE;

    // This is a special interval indicating that we expect to use $elemMatch semantics when
    // estimating the current path.
    const IntervalReqExpr::Node _arrayOnlyInterval;
};

CEHistogramTransport::CEHistogramTransport(std::shared_ptr<ce::CollectionStatistics> stats,
                                           std::unique_ptr<CEInterface> fallbackCE)
    : _impl(std::make_unique<CEHistogramTransportImpl>(stats, std::move(fallbackCE))) {}

CEHistogramTransport::~CEHistogramTransport() {}

CEType CEHistogramTransport::deriveCE(const Metadata& metadata,
                                      const Memo& memo,
                                      const LogicalProps& logicalProps,
                                      const ABT::reference_type logicalNodeRef) const {
    return algebra::transport<true>(logicalNodeRef, *this->_impl, metadata, memo, logicalProps);
}

}  // namespace mongo::optimizer::cascades
