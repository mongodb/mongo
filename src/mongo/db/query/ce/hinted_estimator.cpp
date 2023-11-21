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

#include "mongo/db/query/ce/hinted_estimator.h"

#include <type_traits>
#include <vector>

#include <absl/container/node_hash_map.h>

#include "mongo/db/query/ce/heuristic_estimator.h"
#include "mongo/db/query/ce/sel_tree_utils.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/partial_schema_requirements.h"

namespace mongo::optimizer::ce {

bool PartialSchemaIntervalComparator::operator()(const PartialSchemaInterval& k1,
                                                 const PartialSchemaInterval& k2) const {
    int result = PartialSchemaKeyComparator::Cmp3W{}(k1.first, k2.first);
    result = result == 0 ? compareIntervalExpr(k1.second, k2.second) : result;
    return result < 0;
}

class HintedTransport {
public:
    CEType transport(const ABT::reference_type n,
                     const SargableNode& node,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        EstimatePartialSchemaEntrySelFn entrySelFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                         const PartialSchemaEntry& e) {
            const auto& [key, req] = e;
            const auto& interval = req.getIntervals();
            if (!isIntervalReqFullyOpenDNF(interval)) {
                if (auto it = _intervalHints.find({key, interval}); it != _intervalHints.cend()) {
                    selTreeBuilder.atom(it->second);
                } else if (auto it = _pathHints.find(key); it != _pathHints.cend()) {
                    selTreeBuilder.atom(it->second);
                }
            }
        };

        PartialSchemaRequirementsCardinalityEstimator estimator(entrySelFn, childResult);
        return estimator.estimateCE(node.getReqMap());
    }

    // Handle any node type that doesn't have a specific overload.
    template <typename T, typename... Ts>
    CEType transport(ABT::reference_type n, const T& /*node*/, Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _heuristicCE.deriveCE(_metadata, _memo, _logicalProps, _queryParameters, n)._ce;
        }
        return {0.0};
    }

    static CERecord derive(const Metadata& metadata,
                           const cascades::Memo& memo,
                           const PartialSchemaSelHints& pathHints,
                           const PartialSchemaIntervalSelHints& intervalHints,
                           const properties::LogicalProps& logicalProps,
                           const QueryParameterMap& queryParameters,
                           const ABT::reference_type logicalNodeRef) {
        HintedTransport instance(
            metadata, memo, logicalProps, pathHints, intervalHints, queryParameters);
        CEType ce = algebra::transport<true>(logicalNodeRef, instance);
        return {ce, "hinted"};
    }

private:
    HintedTransport(const Metadata& metadata,
                    const cascades::Memo& memo,
                    const properties::LogicalProps& logicalProps,
                    const PartialSchemaSelHints& pathHints,
                    const PartialSchemaIntervalSelHints& intervalHints,
                    const QueryParameterMap& queryParameters)
        : _heuristicCE(),
          _metadata(metadata),
          _memo(memo),
          _logicalProps(logicalProps),
          _pathHints(pathHints),
          _intervalHints(intervalHints),
          _queryParameters(queryParameters) {}

    HeuristicEstimator _heuristicCE;

    // We don't own this.
    const Metadata& _metadata;
    const cascades::Memo& _memo;
    const properties::LogicalProps& _logicalProps;

    // Selectivity hints per PartialSchemaKey.
    const PartialSchemaSelHints& _pathHints;
    // Selectivity hints per PartialSchemaKey and IntervalReqExpr::Node.
    const PartialSchemaIntervalSelHints& _intervalHints;
    const QueryParameterMap& _queryParameters;
};

CERecord HintedEstimator::deriveCE(const Metadata& metadata,
                                   const cascades::Memo& memo,
                                   const properties::LogicalProps& logicalProps,
                                   const QueryParameterMap& queryParameters,
                                   const ABT::reference_type logicalNodeRef) const {
    return HintedTransport::derive(
        metadata, memo, _pathHints, _intervalHints, logicalProps, queryParameters, logicalNodeRef);
}

}  // namespace mongo::optimizer::ce
