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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <memory>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/opt_counter_info.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/explain_interface.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_builder_plan_data.h"

namespace mongo {

// Arguments to create a PlanExecutor, except for the CanonicalQuery.
struct ExecParams {
    OperationContext* opCtx;
    std::unique_ptr<QuerySolution> solution;
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> root;
    std::unique_ptr<optimizer::AbstractABTPrinter> optimizerData;
    size_t plannerOptions;
    NamespaceString nss;
    std::unique_ptr<PlanYieldPolicySBE> yieldPolicy;
    bool planIsFromCache;
    bool generatedByBonsai;
    const boost::optional<MatchExpression*> pipelineMatchExpr;
    OptimizerCounterInfo optCounterInfo;
};

/**
 * Returns hints from the cascades query knobs.
 */
optimizer::QueryHints getHintsFromQueryKnobs();

/**
 * Enforce that unsupported command options don't run through Bonsai. Note these checks are already
 * present in the Bonsai fallback mechansim, but those checks are skipped when Bonsai is forced.
 * This function prevents us from accidently forcing Bonsai with an unsupported option.
 */
void validateCommandOptions(const CanonicalQuery* query,
                            const CollectionPtr& collection,
                            const boost::optional<BSONObj>& indexHint,
                            const stdx::unordered_set<NamespaceString>& involvedCollections);

/**
 * Returns a the arguments to create a PlanExecutor for the given Pipeline, except the
 * CanonicalQuery which must be provided by the caller.
 *
 * The CanonicalQuery parameter allows for code reuse between functions in this file and should
 * not be set by callers.
 */
boost::optional<ExecParams> getSBEExecutorViaCascadesOptimizer(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    optimizer::QueryHints queryHints,
    const boost::optional<BSONObj>& indexHint,
    BonsaiEligibility eligibility,
    Pipeline* pipeline,
    const CanonicalQuery* = nullptr);

struct PhaseManagerWithPlan {
    optimizer::OptPhaseManager phaseManager;
    boost::optional<optimizer::PlanAndProps> planAndProps;
    OptimizerCounterInfo optCounterInfo;
    boost::optional<MatchExpression*> pipelineMatchExpr;
};

PhaseManagerWithPlan getPhaseManager(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const NamespaceString& nss,
    const CollectionPtr& collection,
    const stdx::unordered_set<NamespaceString>& involvedCollections,
    optimizer::QueryHints queryHints,
    const boost::optional<BSONObj>& hint,
    bool requireRID,
    bool parameterizationOn,
    Pipeline* pipeline,
    const CanonicalQuery* canonicalQuery);

struct PlanWithData {
    bool fromCache;
    std::unique_ptr<sbe::PlanStage> plan;
    stage_builder::PlanStageData planData;
};

/*
 * This function either creates a plan or fetches one from cache.
 */
PlanWithData plan(optimizer::OptPhaseManager& phaseManager,
                  optimizer::PlanAndProps& planAndProps,
                  OperationContext* opCtx,
                  const MultipleCollectionAccessor& collections,
                  bool requireRID,
                  const std::unique_ptr<PlanYieldPolicySBE>& sbeYieldPolicy,
                  boost::optional<MatchExpression*> pipelineMatchExpr,
                  const boost::optional<sbe::PlanCacheKey>& planCacheKey,
                  optimizer::VariableEnvironment& env);

/**
 * Returns a PlanExecutor for the given CanonicalQuery.
 */
boost::optional<ExecParams> getSBEExecutorViaCascadesOptimizer(
    const MultipleCollectionAccessor& collections,
    optimizer::QueryHints queryHints,
    BonsaiEligibility eligibility,
    const CanonicalQuery* query);

/**
 * Constructs a plan executor with the given CanonicalQuery and ExecParams.
 */
StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> makeExecFromParams(
    std::unique_ptr<CanonicalQuery> cq,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    const MultipleCollectionAccessor& collections,
    ExecParams execArgs);

}  // namespace mongo
