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

#pragma once

#include "mongo/db/query/optimizer/cascades/logical_rewriter.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer::cascades {

class PhysicalRewriter {
    friend class PropEnforcerVisitor;
    friend class ImplementationVisitor;

public:
    struct OptimizeGroupResult {
        OptimizeGroupResult();
        OptimizeGroupResult(size_t index, CostType cost);

        OptimizeGroupResult(const OptimizeGroupResult& other) = default;
        OptimizeGroupResult(OptimizeGroupResult&& other) = default;

        bool _success;
        size_t _index;
        CostType _cost;
    };

    PhysicalRewriter(Memo& memo,
                     GroupIdType rootGroupid,
                     const QueryHints& hints,
                     const RIDProjectionsMap& ridProjections,
                     const CostingInterface& costDerivation,
                     const PathToIntervalFn& pathToInterval,
                     std::unique_ptr<LogicalRewriter>& logicalRewriter);

    /**
     * Main entry point for physical optimization.
     * Optimize a logical plan rooted at a RootNode, and return an index into the winner's circle if
     * successful.
     */
    OptimizeGroupResult optimizeGroup(GroupIdType groupId,
                                      properties::PhysProps physProps,
                                      PrefixId& prefixId,
                                      CostType costLimit);

private:
    void costAndRetainBestNode(ABT node,
                               ChildPropsType childProps,
                               NodeCEMap nodeCEMap,
                               PhysicalRewriteType rule,
                               GroupIdType groupId,
                               PrefixId& prefixId,
                               PhysOptimizationResult& bestResult);

    std::pair<bool, CostType> optimizeChildren(CostType nodeCost,
                                               ChildPropsType childProps,
                                               PrefixId& prefixId,
                                               CostType costLimit);

    // We don't own any of this.
    Memo& _memo;
    const GroupIdType _rootGroupId;
    const CostingInterface& _costDerivation;
    const QueryHints& _hints;
    const RIDProjectionsMap& _ridProjections;
    const PathToIntervalFn& _pathToInterval;
    // If set, we'll perform logical rewrites as part of OptimizeGroup().
    std::unique_ptr<LogicalRewriter>& _logicalRewriter;
};

}  // namespace mongo::optimizer::cascades
