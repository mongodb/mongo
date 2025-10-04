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

#include "mongo/base/status_with.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"

#include <cstddef>
#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString::kEmpty,
    std::unique_ptr<QuerySolution> qs = nullptr,
    boost::optional<size_t> cachedPlanHash = boost::none);

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
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString::kEmpty,
    std::unique_ptr<QuerySolution> qs = nullptr);

// TODO: SERVER-86878 Remove `StatusWith` return type from plan_executor_factory::make().
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    std::unique_ptr<QuerySolution> qs,
    std::unique_ptr<CanonicalQuery> cq,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<CollectionAcquisition>& collection,
    size_t plannerOptions,
    NamespaceString nss,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    boost::optional<size_t> cachedPlanHash,
    QueryPlanner::CostBasedRankerResult cbrResult,
    stage_builder::PlanStageToQsnMap planStageQsnMap,
    std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages);

/**
 * Constructs a PlanExecutor for the query 'cq' which will execute the SBE plan 'root'. A yield
 * policy can optionally be provided if the plan should automatically yield during execution.
 * If a classicRuntimePlannerStage is passed in, the PlanStage will be eventually passed to a
 * PlanExplainer and which will in turn extract relevant explain data from the classic multiplanner.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<QuerySolution> solution,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root,
    const MultipleCollectionAccessor& mca,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::unique_ptr<RemoteCursorMap> remoteCursors = nullptr,
    std::unique_ptr<RemoteExplainVector> remoteExplains = nullptr,
    std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage = nullptr);

/**
 * Similar to the factory function above in that it also constructs an executor for the winning SBE
 * plan passed as a 'candidate'. This overload allows callers to pass a pre-existing queue
 * ('stash') of BSON objects or record ids to return to the caller.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> make(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    sbe::plan_ranker::CandidatePlan candidate,
    const MultipleCollectionAccessor& collections,
    size_t plannerOptions,
    NamespaceString nss,
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
    std::unique_ptr<RemoteCursorMap> remoteCursors,
    std::unique_ptr<RemoteExplainVector> remoteExplains,
    boost::optional<size_t> cachedPlanHash = boost::none);

/**
 * Constructs a plan executor for executing the given 'pipeline'.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline> pipeline,
    PlanExecutorPipeline::ResumableScanType resumableScanType =
        PlanExecutorPipeline::ResumableScanType::kNone);

}  // namespace mongo::plan_executor_factory
