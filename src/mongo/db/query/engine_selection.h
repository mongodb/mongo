// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/util/modules.h"

namespace mongo {

enum class EngineChoice { kClassic, kSbe };

struct EngineSelectionResult {
    EngineChoice engine = EngineChoice::kClassic;
    /**
     * When SBE is the chosen engine and a query solution was specified, 'planPushdownRoot'
     * indicates the top-most QuerySolutionNode that should run in SBE. The remainder of the tree
     * should run in classic.
     */
    const QuerySolutionNode* planPushdownRoot = nullptr;
};

/*
 * Returns the engine choice given the query details. An optional query solution may be
 * passed in, which will be analyzed for SBE eligibility depending on the plan shape.
 */
EngineSelectionResult chooseEngine(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* cq,
    const Pipeline* pipeline,
    bool needsMerge,
    std::unique_ptr<QueryPlannerParams> plannerParams,
    const QuerySolution* solution = nullptr,
    const std::function<void()>& extendSolutionWithPipelineFn = nullptr);

/*
 * Selects the execution engine, guaranteeing that if SBE is chosen, the query solution will be
 * extended with the SBE-eligible pipeline prefix. If classic is chosen and the solution was
 * extended for the eligibility check, the extension is removed.
 */
EngineChoice extendSolutionAndSelectEngine(std::unique_ptr<QuerySolution>& solution,
                                           OperationContext* opCtx,
                                           CanonicalQuery* cq,
                                           const Pipeline* pipeline,
                                           const MultipleCollectionAccessor& collections,
                                           QueryPlannerParams& plannerParams);

}  // namespace mongo
