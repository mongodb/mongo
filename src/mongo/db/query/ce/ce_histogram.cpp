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
#include "mongo/db/query/ce/collection_statistics.h"
#include "mongo/db/query/ce/histogram_estimation.h"

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/utils/abt_hash.h"
#include "mongo/db/query/optimizer/utils/ce_math.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"

namespace mongo::optimizer::cascades {

using namespace properties;

class CEHistogramTransportImpl {
public:
    CEHistogramTransportImpl(const ce::CollectionStatistics& stats)
        : _heuristicCE(), _stats(stats) {}

    ~CEHistogramTransportImpl() {}

    CEType transport(const ABT& n,
                     const ScanNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType /*bindResult*/) {
        return _stats.getCardinality();
    }

    CEType transport(const ABT& n,
                     const SargableNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        // Early out and return 0 since we don't expect to get more results.
        if (childResult == 0.0) {
            return 0.0;
        }

        std::vector<double> topLevelSelectivities;
        for (const auto& [key, req] : node.getReqMap()) {
            std::vector<double> disjSelectivities;
            auto path = key._path.cast<PathGet>()->name();

            // Fallback to heuristic if no histogram.
            auto histogram = _stats.getHistogram(path);
            if (!histogram) {
                // For now, because of the structure of SargableNode and the implementation of
                // HeuristicCE, we can't combine heuristic & histogram estimates. In this case,
                // default to Heuristic if we don't have a histogram for any of the predicates.
                return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
            }

            // Intervals are in DNF.
            const auto intervalDNF = req.getIntervals();
            const auto disjuncts = intervalDNF.cast<IntervalReqExpr::Disjunction>()->nodes();
            for (const auto& disjunct : disjuncts) {
                const auto& conjuncts = disjunct.cast<IntervalReqExpr::Conjunction>()->nodes();

                std::vector<double> conjSelectivities;
                for (const auto& conjunct : conjuncts) {
                    const auto& interval = conjunct.cast<IntervalReqExpr::Atom>()->getExpr();
                    auto cardinality =
                        ce::estimateIntervalCardinality(*histogram, interval, childResult);

                    // We have to convert the cardinality to a selectivity. The histogram returns
                    // the cardinality for the entire collection; however, fewer records may be
                    // expected at the SargableNode.
                    conjSelectivities.push_back(cardinality / _stats.getCardinality());
                }

                auto backoff = ce::conjExponentialBackoff(std::move(conjSelectivities));
                disjSelectivities.push_back(backoff);
            }

            auto backoff = ce::disjExponentialBackoff(std::move(disjSelectivities));
            topLevelSelectivities.push_back(backoff);
        }

        // The elements of the PartialSchemaRequirements map represent an implicit conjunction.
        auto backoff = ce::conjExponentialBackoff(std::move(topLevelSelectivities));
        return backoff * childResult;
    }

    CEType transport(const ABT& n,
                     const RootNode& node,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     CEType childResult,
                     CEType /*refsResult*/) {
        // Root node does not change cardinality.
        return childResult;
    }

    /**
     * Other ABT types default to heuristic CE.
     */
    template <typename T, typename... Ts>
    CEType transport(const ABT& n,
                     const T& /*node*/,
                     const Memo& memo,
                     const LogicalProps& logicalProps,
                     Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _heuristicCE.deriveCE(memo, logicalProps, n.ref());
        }
        return 0.0;
    }

private:
    HeuristicCE _heuristicCE;
    const ce::CollectionStatistics& _stats;
};

CEHistogramTransport::CEHistogramTransport(const ce::CollectionStatistics& stats)
    : _impl(std::make_unique<CEHistogramTransportImpl>(stats)) {}

CEHistogramTransport::~CEHistogramTransport() {}

CEType CEHistogramTransport::deriveCE(const Memo& memo,
                                      const LogicalProps& logicalProps,
                                      const ABT::reference_type logicalNodeRef) const {
    return algebra::transport<true>(logicalNodeRef, *this->_impl, memo, logicalProps);
}

}  // namespace mongo::optimizer::cascades
