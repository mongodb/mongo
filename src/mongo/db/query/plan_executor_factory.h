/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/duration.h"
#include <queue>

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/sbe_runtime_planner.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo::plan_executor_factory {

/**
 * Creates a new 'PlanExecutor' capable of executing the query 'cq', or a non-OK status if a
 * plan executor could not be created.
 *
 * Passing YIELD_AUTO will construct a yielding executor which may yield in the following
 * circumstances:
 *   - During plan selection inside the call to make().
 *   - On any call to getNext().
 *   - On any call to restoreState().
 *   - While executing the plan inside executePlan().
 *
 * If auto-yielding is enabled, a yield during make() may result in the PlanExecutorImpl being
 * killed, in which case this method will return a non-OK status.
 *
 * The caller must provide either a non-null value for 'collection, or a non-empty 'nss'
 * NamespaceString but not both.
 *
 * Note that the PlanExecutor will use the ExpressionContext associated with 'cq' and the
 * OperationContext associated with that ExpressionContext.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const CollectionPtr* collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString(),
    std::unique_ptr<QuerySolution> qs = nullptr);

/**
 * This overload is provided for executors that do not need a CanonicalQuery. For example, the
 * outer plan executor for an aggregate command does not have a CanonicalQuery.
 *
 * Note that the PlanExecutor will use the OperationContext associated with the 'expCtx'
 * ExpressionContext.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    const CollectionPtr* collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString(),
    std::unique_ptr<QuerySolution> qs = nullptr);

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rt,
    std::unique_ptr<QuerySolution> qs,
    std::unique_ptr<CanonicalQuery> cq,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const CollectionPtr* collection,
    size_t plannerOptions,
    NamespaceString nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy);

/**
 * Constructs a PlanExecutor for the query 'cq' which will execute the SBE plan 'root'. A yield
 * policy can optionally be provided if the plan should automatically yield during execution.
 * "optimizerData" is used to print optimizer ABT plans, and may be empty.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    std::unique_ptr<optimizer::AbstractABTPrinter> optimizerData,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    bool isFromPlanCache,
    bool generatedByBonsai);

/**
 * Similar to the factory function above in that it also constructs an executor for the winning SBE
 * plan passed in 'candidates' vector. This overload allows callers to pass a pre-existing queue
 * ('stash') of BSON objects or record ids to return to the caller.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    sbe::CandidatePlans candidates,
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy);

/**
 * Constructs a plan executor for executing the given 'pipeline'.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    PlanExecutorPipeline::ResumableScanType resumableScanType =
        PlanExecutorPipeline::ResumableScanType::kNone);

}  // namespace mongo::plan_executor_factory
