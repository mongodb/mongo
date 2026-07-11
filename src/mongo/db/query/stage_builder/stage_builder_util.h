// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo::stage_builder {
/**
 * Turns 'solution' into an executable tree of PlanStage(s). Returns a pointer to the root of
 * the plan stage tree.
 *
 * 'cq' must be the CanonicalQuery from which 'solution' is derived. Illegal to call if 'ws'
 * is nullptr, or if 'solution.root' is nullptr.
 *
 * The 'PlanStageType' type parameter defines a specific type of PlanStage the executable tree
 * will consist of.
 */
std::unique_ptr<PlanStage> buildClassicExecutableTree(OperationContext* opCtx,
                                                      CollectionAcquisition collection,
                                                      const CanonicalQuery& cq,
                                                      const QuerySolution& solution,
                                                      WorkingSet* ws);

std::unique_ptr<PlanStage> buildClassicExecutableTree(OperationContext* opCtx,
                                                      CollectionAcquisition collection,
                                                      const CanonicalQuery& cq,
                                                      const QuerySolution& solution,
                                                      WorkingSet* ws,
                                                      PlanStageToQsnMap* planStageQsnMap);

std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>
buildSlotBasedExecutableTree(OperationContext* opCtx,
                             const MultipleCollectionAccessor& collections,
                             const CanonicalQuery& cq,
                             const QuerySolution& solution,
                             PlanYieldPolicySBE* yieldPolicy,
                             const cost_based_ranker::EstimateMap* estimates = nullptr);

}  // namespace mongo::stage_builder
