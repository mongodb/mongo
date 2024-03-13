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

#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/interval_evaluation_tree.h"
#include "mongo/db/query/sbe_stage_builder_plan_data.h"

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
 * If the execution tree ('root'), which was cloned from the SBE plan cache, contains an SBE
 * clustered collection scan stage, this method is called to bind the current query ('cq')'s scan
 * bounds into its minRecord and maxRecord slots.
 *
 * - 'cq' is the query
 * - 'root' is the root node of the SBE execution plan from the plan cache
 * - 'data' contains cached info to be substituted into the plan
 * - 'runtimeEnvironment' is the SBE runtime environment
 */
void bindClusteredCollectionBounds(const CanonicalQuery& cq,
                                   const sbe::PlanStage* root,
                                   const stage_builder::PlanStageData* data,
                                   sbe::RuntimeEnvironment* runtimeEnvironment);
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
