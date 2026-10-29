// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

namespace mongo::input_params {
/**
 * Walks the MatchExpression looking for nodes which have an input parameter id, along with a
 * constant associated with that parameter id. For each such match expression node, looks up the
 * corresponding slot in the 'RuntimeEnvironment' using the 'InputParamToSlotMap' and sets the value
 * of that slot.
 *
 * The caller should pass true for 'bindingCachedPlan' if we are binding-in new parameter values for
 * a plan that was recovered from the SBE plan cache.
 */
void bind(const MatchExpression* matchExpr,
          stage_builder::PlanStageData& data,
          bool bindingCachedPlan);

/**
 * Binds index bounds evaluated from IETs to index bounds slots for the given query.
 *
 * - 'cq' is the query
 * - 'indexBoundsInfo' contains the IETs and the slots
 * - 'runtimeEnvironment' is the SBE runtime environment
 * - 'indexBoundsEvaluationCache' is the evaluation cache used by the explode nodes to keep the
 * common IET evaluation results.
 */
void bindIndexBounds(
    const CanonicalQuery& cq,
    const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
    sbe::RuntimeEnvironment* runtimeEnvironment,
    interval_evaluation_tree::IndexBoundsEvaluationCache* indexBoundsEvaluationCache = nullptr);

/**
 * If the plan was cloned from SBE plan cache and limit and/or skip values were parameterized,
 * this method is called to bind the current query's limit and skip values to corresponding slots.
 */
void bindLimitSkipInputSlots(const CanonicalQuery& cq,
                             const stage_builder::PlanStageData* data,
                             sbe::RuntimeEnvironment* runtimeEnvironment);

/**
 * In each $where expression in the given 'filter', recover the JS function predicate which has
 * been previously extracted from the expression into SBE runtime environment during the input
 * parameters bind-in process. In order to enable the filter to participate in replanning, we
 * need to perform the reverse operation and put the JS function back into the filter.
 */
void recoverWhereExprPredicate(MatchExpression* filter, stage_builder::PlanStageData& data);

}  // namespace mongo::input_params
