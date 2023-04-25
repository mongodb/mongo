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

#include "mongo/db/query/ce/heuristic_estimator.h"
#include "mongo/db/query/ce/sel_tree_utils.h"

namespace mongo::optimizer::ce {
class HintedTransport {
public:
    CEType transport(const ABT& n,
                     const SargableNode& node,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        EstimatePartialSchemaEntrySelFn entrySelFn = [&](SelectivityTreeBuilder& selTreeBuilder,
                                                         const PartialSchemaEntry& e) {
            const auto& [key, req] = e;
            if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                auto it = _hints.find(key);
                if (it != _hints.cend()) {
                    selTreeBuilder.atom(it->second);
                }
            }
        };

        PartialSchemaRequirementsCardinalityEstimator estimator(entrySelFn, childResult);
        return estimator.estimateCE(node.getReqMap().getRoot());
    }

    template <typename T, typename... Ts>
    CEType transport(const ABT& n, const T& /*node*/, Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _heuristicCE.deriveCE(_metadata, _memo, _logicalProps, n.ref());
        }
        return {0.0};
    }

    static CEType derive(const Metadata& metadata,
                         const cascades::Memo& memo,
                         const PartialSchemaSelHints& hints,
                         const properties::LogicalProps& logicalProps,
                         const ABT::reference_type logicalNodeRef) {
        HintedTransport instance(metadata, memo, logicalProps, hints);
        return algebra::transport<true>(logicalNodeRef, instance);
    }

private:
    HintedTransport(const Metadata& metadata,
                    const cascades::Memo& memo,
                    const properties::LogicalProps& logicalProps,
                    const PartialSchemaSelHints& hints)
        : _heuristicCE(),
          _metadata(metadata),
          _memo(memo),
          _logicalProps(logicalProps),
          _hints(hints) {}

    HeuristicEstimator _heuristicCE;

    // We don't own this.
    const Metadata& _metadata;
    const cascades::Memo& _memo;
    const properties::LogicalProps& _logicalProps;
    const PartialSchemaSelHints& _hints;
};

CEType HintedEstimator::deriveCE(const Metadata& metadata,
                                 const cascades::Memo& memo,
                                 const properties::LogicalProps& logicalProps,
                                 const ABT::reference_type logicalNodeRef) const {
    return HintedTransport::derive(metadata, memo, _hints, logicalProps, logicalNodeRef);
}

}  // namespace mongo::optimizer::ce
