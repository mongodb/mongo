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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo::input_params {
/**
 * Walks the MatchExpression from the 'CanonicalQuery' looking for nodes which have an input
 * parameter id, along with a constant associated with that parameter id. For each such match
 * expression node, looks up the corresponding slot in the 'RuntimeEnvironment' using the
 * 'InputParamToSlotMap' and sets the value of that slot.
 *
 * The caller should pass true for 'bindingCachedPlan' if we are binding-in new parameter values for
 * a plan that was recovered from the SBE plan cache.
 */
void bind(const CanonicalQuery&, stage_builder::PlanStageData&, bool bindingCachedPlan);

/**
 * Binds index bounds evaluated from IETs to index bounds slots for the given query.
 *
 * - 'cq' is the query
 * - 'indexBoundsInfo' contains the IETs and the slots
 * - runtimeEnvironment SBE runtime environment
 * - 'indexBoundsEvaluationCache' is the evaluation cache used by the explode nodes to keep the
 * common IET evaluation results.
 */
void bindIndexBounds(
    const CanonicalQuery& cq,
    const stage_builder::IndexBoundsEvaluationInfo& indexBoundsInfo,
    sbe::RuntimeEnvironment* runtimeEnvironment,
    interval_evaluation_tree::IndexBoundsEvaluationCache* indexBoundsEvaluationCache = nullptr);
}  // namespace mongo::input_params
