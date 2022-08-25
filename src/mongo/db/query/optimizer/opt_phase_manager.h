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

#include <unordered_set>

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/logical_rewriter.h"
#include "mongo/db/query/optimizer/cascades/physical_rewriter.h"

namespace mongo::optimizer {

using namespace cascades;

/**
 * This class wraps together different optimization phases.
 * First the transport rewrites are applied such as constant folding and redundant expression
 * elimination. Second the logical and physical reordering rewrites are applied using the memo.
 * Third the final transport rewritesd are applied.
 */
class OptPhaseManager {
public:
    enum class OptPhase {
        //  ConstEval performs the following rewrites: constant folding, inlining, and dead code
        //  elimination.
        ConstEvalPre,
        PathFuse,

        // Memo phases below perform Cascades-style optimization.
        // Reorder and transform nodes. Convert Filter and Eval nodes to SargableNodes, and possibly
        // merge them.
        MemoSubstitutionPhase,
        // Performs Local-global and rewrites to enable index intersection.
        // If there is an implementation phase, it runs integrated with the top-down optimization.
        // If there is no implementation phase, it runs standalone.
        MemoExplorationPhase,
        // Implementation and enforcement rules.
        MemoImplementationPhase,

        PathLower,
        ConstEvalPost
    };

    using PhaseSet = opt::unordered_set<OptPhase>;

    OptPhaseManager(PhaseSet phaseSet,
                    PrefixId& prefixId,
                    Metadata metadata,
                    DebugInfo debugInfo,
                    QueryHints queryHints = {});
    OptPhaseManager(PhaseSet phaseSet,
                    PrefixId& prefixId,
                    bool requireRID,
                    Metadata metadata,
                    std::unique_ptr<CEInterface> ceDerivation,
                    std::unique_ptr<CostingInterface> costDerivation,
                    DebugInfo debugInfo,
                    QueryHints queryHints = {});

    // TODO SERVER-68914: Fix object ownership issues of data members of the Memo class.
    OptPhaseManager(const OptPhaseManager&) = delete;
    OptPhaseManager& operator=(const OptPhaseManager&) = delete;
    OptPhaseManager(OptPhaseManager&&) = delete;
    OptPhaseManager& operator=(OptPhaseManager&&) = delete;

    /**
     * Optimization modifies the input argument.
     * Return result is true for successful optimization and false for failure.
     */
    bool optimize(ABT& input);

    static const PhaseSet& getAllRewritesSet();

    MemoPhysicalNodeId getPhysicalNodeId() const;

    const QueryHints& getHints() const;
    QueryHints& getHints();

    const Memo& getMemo() const;

    const Metadata& getMetadata() const;

    PrefixId& getPrefixId() const;

    const NodeToGroupPropsMap& getNodeToGroupPropsMap() const;
    NodeToGroupPropsMap& getNodeToGroupPropsMap();

    const RIDProjectionsMap& getRIDProjections() const;

private:
    bool hasPhase(OptPhase phase) const;

    template <OptPhase phase, class C>
    bool runStructuralPhase(C instance, VariableEnvironment& env, ABT& input);

    /**
     * Run two structural phases until mutual fixpoint.
     * We assume we can construct from the types by initializing with env.
     */
    template <const OptPhase phase1, const OptPhase phase2, class C1, class C2>
    bool runStructuralPhases(C1 instance1, C2 instance2, VariableEnvironment& env, ABT& input);

    bool runMemoLogicalRewrite(OptPhase phase,
                               VariableEnvironment& env,
                               const LogicalRewriter::RewriteSet& rewriteSet,
                               GroupIdType& rootGroupId,
                               bool runStandalone,
                               std::unique_ptr<LogicalRewriter>& logicalRewriter,
                               ABT& input);

    bool runMemoPhysicalRewrite(OptPhase phase,
                                VariableEnvironment& env,
                                GroupIdType rootGroupId,
                                std::unique_ptr<LogicalRewriter>& logicalRewriter,
                                ABT& input);

    bool runMemoRewritePhases(VariableEnvironment& env, ABT& input);


    static PhaseSet _allRewrites;

    const PhaseSet _phaseSet;

    const DebugInfo _debugInfo;

    QueryHints _hints;

    Metadata _metadata;

    /**
     * Final state of the memo after physical rewrites are complete.
     */
    Memo _memo;

    /**
     * Cost derivation function.
     */
    std::unique_ptr<CostingInterface> _costDerivation;

    /**
     * Root physical node if we have performed physical rewrites.
     */
    MemoPhysicalNodeId _physicalNodeId;

    /**
     * Map from node to logical and physical properties.
     */
    NodeToGroupPropsMap _nodeToGroupPropsMap;

    /**
     * Used to optimize update and delete statements. If set will include indexing requirement with
     * seed physical properties.
     */
    const bool _requireRID;

    /**
     * RID projection names we have generated for each scanDef. Used for physical rewriting.
     */
    RIDProjectionsMap _ridProjections;

    // We don't own this.
    PrefixId& _prefixId;
};

}  // namespace mongo::optimizer
