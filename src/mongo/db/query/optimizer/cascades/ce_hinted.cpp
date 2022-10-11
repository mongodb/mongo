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

#include "mongo/db/query/optimizer/cascades/ce_hinted.h"
#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"

namespace mongo::optimizer::cascades {

using namespace properties;

class CEHintedTransport {
public:
    CEType transport(const ABT& n,
                     const SargableNode& node,
                     CEType childResult,
                     CEType /*bindsResult*/,
                     CEType /*refsResult*/) {
        CEType result = childResult;
        for (const auto& [key, req] : node.getReqMap()) {
            if (!isIntervalReqFullyOpenDNF(req.getIntervals())) {
                auto it = _hints.find(key);
                if (it != _hints.cend()) {
                    // Assume independence.
                    result *= it->second;
                }
            }
        }

        return result;
    }

    template <typename T, typename... Ts>
    CEType transport(const ABT& n, const T& /*node*/, Ts&&...) {
        if (canBeLogicalNode<T>()) {
            return _heuristicCE.deriveCE(_metadata, _memo, _logicalProps, n.ref());
        }
        return 0.0;
    }

    static CEType derive(const Metadata& metadata,
                         const Memo& memo,
                         const PartialSchemaSelHints& hints,
                         const LogicalProps& logicalProps,
                         const ABT::reference_type logicalNodeRef) {
        CEHintedTransport instance(metadata, memo, logicalProps, hints);
        return algebra::transport<true>(logicalNodeRef, instance);
    }

private:
    CEHintedTransport(const Metadata& metadata,
                      const Memo& memo,
                      const LogicalProps& logicalProps,
                      const PartialSchemaSelHints& hints)
        : _heuristicCE(),
          _metadata(metadata),
          _memo(memo),
          _logicalProps(logicalProps),
          _hints(hints) {}

    HeuristicCE _heuristicCE;

    // We don't own this.
    const Metadata& _metadata;
    const Memo& _memo;
    const LogicalProps& _logicalProps;
    const PartialSchemaSelHints& _hints;
};

CEType HintedCE::deriveCE(const Metadata& metadata,
                          const Memo& memo,
                          const LogicalProps& logicalProps,
                          const ABT::reference_type logicalNodeRef) const {
    return CEHintedTransport::derive(metadata, memo, _hints, logicalProps, logicalNodeRef);
}

}  // namespace mongo::optimizer::cascades
