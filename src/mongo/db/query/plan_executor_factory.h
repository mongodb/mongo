// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/sbe_plan_ranker.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::plan_executor_factory {

/**
 * Creates a new 'PlanExecutor' capable of executing the query 'cq'.
 *
 * Passing YIELD_AUTO will construct a yielding executor which may yield in the following
 * circumstances:
 *   - During plan selection inside the call to make().
 *   - On any call to getNext().
 *   - On any call to restoreState().
 *   - While executing the plan inside executePlan().
 *
 * If auto-yielding is enabled, a yield during make() may result in the PlanExecutorImpl being
 * killed, in which case this method will throw.
 *
 * The caller must provide either a non-null value for 'collection, or a non-empty 'nss'
 * NamespaceString but not both.
 *
 * Note that the PlanExecutor will use the ExpressionContext associated with 'cq' and the
 * OperationContext associated with that ExpressionContext.
 */
[[MONGO_MOD_PUBLIC]] std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString::kEmpty,
    std::unique_ptr<QuerySolution> qs = nullptr,
    boost::optional<size_t> cachedPlanHash = boost::none,
    boost::optional<std::string> replanReason = boost::none);

/**
 * This overload is provided for executors that do not need a CanonicalQuery. For example, the
 * outer plan executor for an aggregate command does not have a CanonicalQuery.
 *
 * Note that the PlanExecutor will use the OperationContext associated with the 'expCtx'
 * ExpressionContext.
 */
[[MONGO_MOD_PUBLIC]] std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::unique_ptr<WorkingSet> ws,
    std::unique_ptr<PlanStage> rootStage,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    size_t plannerOptions,
    NamespaceString nss = NamespaceString::kEmpty,
    std::unique_ptr<QuerySolution> qs = nullptr);

[[MONGO_MOD_PUBLIC]] std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
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
    boost::optional<std::string> replanReason,
    boost::optional<PlanExplainerData> maybeExplainData);

/**
 * Constructs a PlanExecutor for the query 'cq' which will execute the SBE plan 'root'. A yield
 * policy can optionally be provided if the plan should automatically yield during execution.
 * If a classicRuntimePlannerStage is passed in, the PlanStage will be eventually passed to a
 * PlanExplainer and which will in turn extract relevant explain data from the classic multiplanner.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
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
    bool usedJoinOpt = false,
    cost_based_ranker::EstimateMap estimates = {},
    std::vector<JoinOptPlan> rejectedJoinPlans = {},
    std::unique_ptr<RemoteCursorMap> remoteCursors = nullptr,
    std::unique_ptr<RemoteExplainVector> remoteExplains = nullptr,
    std::unique_ptr<MultiPlanStage> classicRuntimePlannerStage = nullptr,
    boost::optional<PlanExplainerData> maybeExplainData = boost::none);

/**
 * Similar to the factory function above in that it also constructs an executor for the winning SBE
 * plan passed as a 'candidate'. This overload allows callers to pass a pre-existing queue
 * ('stash') of BSON objects or record ids to return to the caller.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
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
[[MONGO_MOD_PUBLIC]] std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> make(
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::unique_ptr<Pipeline> pipeline,
    PlanExecutorPipeline::ResumableScanType resumableScanType =
        PlanExecutorPipeline::ResumableScanType::kNone);

}  // namespace mongo::plan_executor_factory
