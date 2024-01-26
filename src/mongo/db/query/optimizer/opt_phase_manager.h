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

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <memory>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mongo/db/query/opt_counter_info.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/logical_rewriter.h"
#include "mongo/db/query/optimizer/cascades/logical_rewrites.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/cascades/physical_rewriter.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"
#include "mongo/db/query/optimizer/utils/memo_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/util/named_enum.h"


namespace mongo::optimizer {

using namespace cascades;

#define OPT_PHASE(F)                                                                               \
    /* ConstEval performs the following rewrites: constant folding, inlining, and dead code        \
     * elimination.                                                                                \
     * PathFusion implements path laws, for example shortcutting field assignment and reads, and   \
     * other path optimizations.                                                                   \
     * We switch between applying ConstEval and PathFusion for as long as they change the query,   \
     * as they can enable new rewrites in each other. These are both done in-place rather than     \
     * creating plan alternatives                                                                  \
     */                                                                                            \
    F(ConstEvalPre)                                                                                \
    F(PathFuse)                                                                                    \
                                                                                                   \
    /* Memo phases below perform Cascades-style optimization. Reorder and transform nodes. Convert \
     * Filter and Eval nodes to SargableNodes, and possibly merge them.*/                          \
    F(MemoSubstitutionPhase)                                                                       \
    /* Performs Local-global and rewrites to enable index intersection. If there is an             \
     * implementation phase, it runs integrated with the top-down optimization. If there is no     \
     * implementation phase, it runs standalone.*/                                                 \
    F(MemoExplorationPhase)                                                                        \
    /* Implementation and enforcement rules. */                                                    \
    F(MemoImplementationPhase)                                                                     \
                                                                                                   \
    /* Lowers paths to expressions. Not to be confused with SBENodeLowering, which lowers ABT      \
     * nodes and expressions to an SBE plan. */                                                    \
    F(PathLower)                                                                                   \
    /* Final round of constant folding, identical to the first ConstEval stage. */                 \
    F(ConstEvalPost)                                                                               \
    /* Simplified constant folding for sampling estimator. */                                      \
    F(ConstEvalPost_ForSampling)                                                                   \
                                                                                                   \
    /* DEBUGGING ONLY: Normalize projection names to ensure assertable plan. */                    \
    F(ProjNormalize)

QUERY_UTIL_NAMED_ENUM_DEFINE(OptPhase, OPT_PHASE);
#undef OPT_PHASE

/**
 * This class drives the optimization process, wrapping together different optimization phases.
 * First the transport rewrites are applied such as constant folding and redundant expression
 * elimination. Second the logical and physical reordering rewrites are applied using the memo.
 * Third the final transport rewrites are applied.
 * Phases may be skipped by specifying a subset of the phases to run in the phaseSet argument.
 */
class OptPhaseManager {
public:
    using PhaseSet = opt::unordered_set<OptPhase>;

    /**
     * Helper struct to configure which phases & rewrites the optimizer should run.
     */
    struct PhasesAndRewrites {
        PhaseSet phaseSet;
        LogicalRewriteSet explorationSet;
        LogicalRewriteSet substitutionSet;

        // Factories for common configurations.
        static PhasesAndRewrites getDefaultForProd();
        static PhasesAndRewrites getDefaultForSampling();
        static PhasesAndRewrites getDefaultForUnindexed();
    };

    OptPhaseManager(PhasesAndRewrites phasesAndRewrites,
                    PrefixId& prefixId,
                    bool requireRID,
                    Metadata metadata,
                    std::unique_ptr<CardinalityEstimator> explorationCE,
                    std::unique_ptr<CardinalityEstimator> substitutionCE,
                    std::unique_ptr<CostEstimator> costEstimator,
                    PathToIntervalFn pathToInterval,
                    ConstFoldFn constFold,
                    DebugInfo debugInfo,
                    QueryHints queryHints,
                    QueryParameterMap queryParameters,
                    OptimizerCounterInfo& optCounterInfo,
                    boost::optional<ExplainOptions::Verbosity> explain = {});

    // We only allow moving.
    OptPhaseManager(const OptPhaseManager& /*other*/) = delete;
    OptPhaseManager(OptPhaseManager&& /*other*/) = default;
    OptPhaseManager& operator=(const OptPhaseManager& /*other*/) = delete;
    OptPhaseManager& operator=(OptPhaseManager&& /*other*/) = delete;

    /**
     * Note: this method is primarily used for testing.
     * Optimization modifies the input argument to return the best plan. If there is an optimization
     * failure (including no plans or more than one plan found), program will tassert.
     */
    void optimize(ABT& input);

    /**
     * Note: this method is primarily used for testing.
     * Same as above, but we also return the associated NodeToGroupPropsMap for the best plan.
     */
    [[nodiscard]] PlanAndProps optimizeAndReturnProps(ABT input);

    /**
     * Similar to optimize, but returns a vector of optimized plans. The vector can be empty if we
     * failed to find a plan, or contain more than one entry if we also requested the rejected plans
     * for the implementation phase.
     */
    [[nodiscard]] PlanExtractorResult optimizeNoAssert(ABT input, bool includeRejected);

    static const PhaseSet& getAllProdRewrites();

    MemoPhysicalNodeId getPhysicalNodeId() const;
    const boost::optional<PlanAndProps>& getPostMemoPlan() const;

    const QueryHints& getHints() const;
    QueryHints& getHints();

    const Memo& getMemo() const;

    const PathToIntervalFn& getPathToInterval() const;

    const Metadata& getMetadata() const;

    const RIDProjectionsMap& getRIDProjections() const;

    const QueryParameterMap& getQueryParameters() const;

    QueryParameterMap& getQueryParameters();

    QueryPlannerOptimizationStagesForDebugExplain& getQueryPlannerOptimizationStages();

    bool hasPhase(OptPhase phase) const;

private:
    template <OptPhase phase, class C>
    void runStructuralPhase(C instance, VariableEnvironment& env, ABT& input);

    /**
     * Run two structural phases until mutual fixpoint.
     * We assume we can construct from the types by initializing with env.
     */
    template <const OptPhase phase1, const OptPhase phase2, class C1, class C2>
    void runStructuralPhases(C1 instance1, C2 instance2, VariableEnvironment& env, ABT& input);

    void runMemoLogicalRewrite(OptPhase phase,
                               VariableEnvironment& env,
                               const LogicalRewriteSet& rewriteSet,
                               GroupIdType& rootGroupId,
                               bool runStandalone,
                               std::unique_ptr<LogicalRewriter>& logicalRewriter,
                               ABT& input);

    [[nodiscard]] PlanExtractorResult runMemoPhysicalRewrite(
        OptPhase phase,
        VariableEnvironment& env,
        GroupIdType rootGroupId,
        bool includeRejected,
        std::unique_ptr<LogicalRewriter>& logicalRewriter,
        ABT& input);

    [[nodiscard]] PlanExtractorResult runMemoRewritePhases(bool includeRejected,
                                                           VariableEnvironment& env,
                                                           ABT& input);

    /**
     * Stores the set of phases and logical rewrites that the optimizer will run.
     */
    const PhasesAndRewrites _phasesAndRewrites;

    const DebugInfo _debugInfo;

    QueryHints _hints;

    Metadata _metadata;

    /**
     * Final state of the memo after physical rewrites are complete.
     */
    Memo _memo;

    /**
     * Logical properties derivation implementation.
     */
    std::unique_ptr<LogicalPropsInterface> _logicalPropsDerivation;

    /**
     * Cardinality estimation implementation to be used during the exploraton phase..
     */
    std::unique_ptr<CardinalityEstimator> _explorationCE;

    /**
     * Cardinality estimation implementation to be used during the substitution phase.
     *
     * The substitution phase typically doesn't care about CE, because it doesn't generate/compare
     * alternatives. Since some CE implementations are expensive (sampling), we let the caller pass
     * a different one for this phase.
     */
    std::unique_ptr<CardinalityEstimator> _substitutionCE;

    /**
     * Cost derivation implementation.
     */
    std::unique_ptr<CostEstimator> _costEstimator;

    /**
     * Path ABT node to index bounds converter implementation.
     */
    PathToIntervalFn _pathToInterval;

    /**
     * Constant fold an expression.
     */
    ConstFoldFn _constFold;

    /**
     * Root physical node if we have performed physical rewrites.
     */
    MemoPhysicalNodeId _physicalNodeId;

    /**
     * Stores the best physical plan ABT (with corresponding properties) after performing memo
     * logical substitution, memo logical exploration and physical rewrite phases.
     * Populated for explain purposes.
     */
    boost::optional<PlanAndProps> _postMemoPlan;

    /**
     * Used to optimize update and delete statements. If set will include indexing requirement with
     * seed physical properties.
     */
    const bool _requireRID;

    /**
     * RID projection names we have generated for each scanDef. Used for physical rewriting.
     */
    RIDProjectionsMap _ridProjections;

    /**
     * We don't own this.
     */
    PrefixId& _prefixId;

    /**
     * Map from parameter ID to constant for the query we are optimizing. This is used by the CE
     * module to estimate selectivities of query parameters.
     */
    QueryParameterMap _queryParameters;

    /**
     * This tracks notable events during optimization. It is used for explain purposes. We don't own
     * this.
     */
    OptimizerCounterInfo& _optCounterInfo;

    /**
     * Query explain verbosity
     */
    boost::optional<ExplainOptions::Verbosity> _explain;

    /**
     * Track query planner optimization stages for explain using queryPlannerDebug verbosity.
     */
    QueryPlannerOptimizationStagesForDebugExplain _queryPlannerOptimizationStages;
};

}  // namespace mongo::optimizer
