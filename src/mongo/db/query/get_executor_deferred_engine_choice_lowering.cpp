/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/get_executor_deferred_engine_choice_lowering.h"

#include "mongo/db/exec/classic/count.h"
#include "mongo/db/exec/runtime_planners/planner_types.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::exec_deferred_engine_choice {

namespace {
/*
 * This class takes information about the query and planning results, and outputs an executor when
 * `lower` is called. In `lower`, the plan ranking result is analyzed, the execution engine is
 * chosen, and then stage builders for the chosen engine are called.
 */
class ExecConstructor {
public:
    ExecConstructor(std::unique_ptr<CanonicalQuery> cq,
                    PlanRankingResult rankingResult,
                    OperationContext* opCtx,
                    const MultipleCollectionAccessor& collections,
                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                    Pipeline* pipeline)
        : _cq(std::move(cq)),
          _rankingResult(std::move(rankingResult)),
          _opCtx(opCtx),
          _collections(collections),
          _yieldPolicy(std::move(yieldPolicy)),
          _pipeline(pipeline) {}

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> lower() {
        if (_rankingResult.usedIdhack) {
            // Idhack always uses the classic engine.
            tassert(11974305,
                    "Expected no query solution for idhack queries.",
                    _rankingResult.solutions.empty());
            return makeClassicExecutor(nullptr /* solution */);
        } else {
            tassert(11974304,
                    "Expected 1 query solutions for non-idhack queries",
                    _rankingResult.solutions.size() == 1);
        }

        auto solution = std::move(_rankingResult.solutions[0]);

        // There are two EOF-related optimizations:
        //     - If a non-existent collection is queried, we may create an EOF plan.
        //     - If multiplanning was used and a plan reached an EOF state, the query
        //       has been fully answered and there's no more execution that needs to
        //       happen. In this case we return a classic executor so that we don't
        //       have to restart work using SBE.
        if (solution->root()->getType() == STAGE_EOF || useEofOptimization()) {
            return makeClassicExecutor(std::move(solution));
        }

        const auto engine = attachPipelineStagesAndSelectEngine(solution);
        return engine == EngineChoice::kClassic ? makeClassicExecutor(std::move(solution))
                                                : makeSbePlanExecutor(std::move(solution));
    }

private:
    /*
     * Selects the engine to execute in, guaranteeing that if SBE is chosen, the QSN will be
     * extended with SBE-eligible pipeline prefix.
     */
    EngineChoice attachPipelineStagesAndSelectEngine(std::unique_ptr<QuerySolution>& solution) {
        bool qsnExtendFnCalled = false;
        bool qsnExtendedForSbe = false;
        // If there is an eligible pipeline prefix to attach to the QSN, fills out the planner
        // params for secondary collections and attaches the stages to the QSN.
        //    - Tracks if this function was called already via `qsnExtendFnCalled`, since engine
        //      selection won't always need to call it.
        //    - Tracks `qsnExtendedForSbe` so we know if the resulting QSN contains a SentinelNode
        //      that needs to be removed.
        auto extendSolutionWithPipelineFn = [&]() {
            qsnExtendFnCalled = true;
            if (_cq->cqPipeline().empty()) {
                // Nothing to extend if the CQ pipeline is empty.
                return;
            }
            qsnExtendedForSbe = true;
            plannerParams()->fillOutSecondaryCollectionsPlannerParams(_opCtx, *_cq, _collections);
            extendSolutionWithPipeline(solution);
        };

        const auto engine = chooseEngine(
            _opCtx,
            _collections,
            _cq.get(),
            _pipeline,
            _cq->getExpCtx()->getNeedsMerge(),
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
                .opCtx = _opCtx,
                .canonicalQuery = *_cq,
                .collections = _collections,
                .plannerOptions = _rankingResult.plannerParams->providedOptions,
            }),
            solution.get(),
            extendSolutionWithPipelineFn);


        if (engine == EngineChoice::kClassic && qsnExtendedForSbe) {
            // If classic was chosen and we extended the QSN to check for SBE eligibility, remove
            // the extension.
            solution->removeRootToSentinel();
        } else if (engine == EngineChoice::kSbe) {
            // If SBE is chosen, we might still need to call the extension function.
            if (!qsnExtendFnCalled) {
                extendSolutionWithPipelineFn();
            }
            // If there was a pipeline to extend the QSN with, the QSN now has a SentinelNode that
            // we need to remove. There is also an additional optimization we may perform if a
            // $project is its child.
            if (qsnExtendedForSbe) {
                solution->removeSentinelNode();
                solution =
                    QueryPlannerAnalysis::removeInclusionProjectionBelowGroup(std::move(solution));
            }
        }
        return engine;
    }

    QueryPlannerParams* plannerParams() {
        return _rankingResult.plannerParams.get();
    }

    const MultiPlanStage* peekMps() const {
        if (!_rankingResult.execState) {
            return nullptr;
        }
        const PlanStage* planStage = _rankingResult.execState->root.get();
        if (!planStage || planStage->stageType() != STAGE_MULTI_PLAN) {
            return nullptr;
        }
        return static_cast<const MultiPlanStage*>(planStage);
    }

    std::unique_ptr<MultiPlanStage> extractMps() {
        if (!_rankingResult.execState) {
            return nullptr;
        }
        auto& planStage = _rankingResult.execState->root;
        if (!planStage || planStage->stageType() != STAGE_MULTI_PLAN) {
            return nullptr;
        }
        return std::unique_ptr<MultiPlanStage>(static_cast<MultiPlanStage*>(planStage.release()));
    }

    /*
     * Analyzes the multiplan stage (if present) to see if the query is eligible for an
     * optimization where a plan that reaches EOF can skip engine selection since the
     * query is effectively answered already by classic.
     */
    bool useEofOptimization() const {
        auto mps = peekMps();
        return mps && mps->bestSolutionEof() &&
            // If explain is used, go through regular engine selection to display which engine is
            // targeted if EOF is not reached during multiplanning.
            !_cq->getExpCtxRaw()->getExplain() &&
            // We can't use EOF optimization if pipeline is present. Because we may need to execute
            // the pipeline part in SBE, we have to rebuild and rerun the whole query.
            _cq->cqPipeline().empty() &&
            // We want more coverage for SBE in debug builds.
            !kDebugBuild;
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeSbePlanExecutor(
        std::unique_ptr<QuerySolution> solution) {
        // Remove any stages from `pipeline` that will be pushed down to SBE.
        finalizePipelineStages(_pipeline, _cq.get());

        auto sbeYieldPolicy =
            PlanYieldPolicySBE::make(_opCtx, _yieldPolicy, _collections, _cq->nss());
        auto sbePlanAndData = stage_builder::buildSlotBasedExecutableTree(
            _opCtx, _collections, *_cq, *solution, sbeYieldPolicy.get());

        const auto* expCtx = _cq->getExpCtxRaw();
        auto remoteCursors = expCtx->getExplain()
            ? nullptr
            : search_helpers::getSearchRemoteCursors(_cq->cqPipeline());
        auto remoteExplains = expCtx->getExplain()
            ? search_helpers::getSearchRemoteExplains(expCtx, _cq->cqPipeline())
            : nullptr;

        // SERVER-117566 integrate with plan cache.
        static const bool isFromPlanCache = false;
        stage_builder::prepareSlotBasedExecutableTree(_opCtx,
                                                      sbePlanAndData.first.get(),
                                                      &sbePlanAndData.second,
                                                      *_cq.get(),
                                                      _collections,
                                                      sbeYieldPolicy.get(),
                                                      isFromPlanCache,
                                                      remoteCursors.get());

        // Count queries are not supported in SBE
        buildRejectedExecutableTreesForExplain(solution.get(), false /*isCountQuery*/);

        auto nss = _cq->nss();
        tassert(11742306,
                "Solution must be present if cachedPlanHash is present: ",
                solution != nullptr || !_rankingResult.cachedPlanHash.has_value());
        return uassertStatusOK(
            plan_executor_factory::make(_opCtx,
                                        std::move(_cq),
                                        std::move(solution),
                                        std::move(sbePlanAndData),
                                        _collections,
                                        plannerParams()->providedOptions,
                                        std::move(nss),
                                        std::move(sbeYieldPolicy),
                                        isFromPlanCache,
                                        _rankingResult.cachedPlanHash,
                                        false /*usedJoinOpt*/,
                                        {} /*estimates*/,
                                        {} /*rejectedJoinPlans*/,
                                        std::move(remoteCursors),
                                        std::move(remoteExplains),
                                        extractMps(),
                                        std::move(_rankingResult.maybeExplainData)));
    }

    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeClassicExecutor(
        std::unique_ptr<QuerySolution> solution) {
        auto expCtx = _cq->getExpCtx();
        auto nss = [&]() {
            if (_collections.hasMainCollection()) {
                return _collections.getMainCollection()->ns();
            } else {
                tassert(11742309, "Expected non-null canonical query", _cq.get());
                const auto nssOrUuid = _cq->getFindCommandRequest().getNamespaceOrUUID();
                return nssOrUuid.isNamespaceString() ? nssOrUuid.nss() : NamespaceString::kEmpty;
            }
        }();

        std::unique_ptr<WorkingSet> workingSet;
        std::unique_ptr<PlanStage> planStage;
        if (_rankingResult.execState) {
            workingSet = std::move(_rankingResult.execState->workingSet);
            planStage = std::move(_rankingResult.execState->root);
        } else {
            workingSet = std::make_unique<WorkingSet>();
            if (!_rankingResult.maybeExplainData.has_value()) {
                _rankingResult.maybeExplainData.emplace();
            }
            planStage = stage_builder::buildClassicExecutableTree(
                _opCtx,
                _collections.getMainCollectionPtrOrAcquisition(),
                *_cq,
                *solution,
                workingSet.get(),
                &_rankingResult.maybeExplainData->planStageQsnMap);
        }

        if (const auto* countStage = dynamic_cast<const CountStage*>(planStage.get())) {
            buildRejectedExecutableTreesForExplain(
                solution.get(), true, countStage->getLimit(), countStage->getSkip());
        } else {
            buildRejectedExecutableTreesForExplain(solution.get(), false /*isCountQuery*/);
        }

        return uassertStatusOK(plan_executor_factory::make(
            _opCtx,
            std::move(workingSet),
            std::move(planStage),
            std::move(solution),
            std::move(_cq),
            expCtx,
            _collections.getMainCollectionAcquisition(),
            _rankingResult.plannerParams->providedOptions,
            std::move(nss),
            _yieldPolicy,
            _rankingResult.cachedPlanHash,
            _rankingResult.plannerParams->replanningData.has_value()
                ? boost::make_optional<std::string>(
                      std::move(_rankingResult.plannerParams->replanningData->replanReason))
                : boost::none,
            std::move(_rankingResult.maybeExplainData)));
    }

    void extendSolutionWithPipeline(std::unique_ptr<QuerySolution>& solution) {
        solution = QueryPlanner::extendWithAggPipeline(*_cq,
                                                       std::move(solution),
                                                       plannerParams()->secondaryCollectionsInfo,
                                                       true /* keepSentinel */);
    }

    void buildRejectedExecutableTreesForExplain(const QuerySolution* solution,
                                                const bool isCountQuery,
                                                const long long countLimit = 0,
                                                const long long countSkip = 0) {
        if (!_rankingResult.maybeExplainData || !_cq->getExplain()) {
            return;
        }

        _rankingResult.maybeExplainData->workingSetForRejectedPlansExplain =
            std::make_unique<WorkingSet>();
        stage_builder::PlanStageToQsnMap planStageToQsnMap;
        for (auto&& solutionWithPlanStage :
             _rankingResult.maybeExplainData->rejectedPlansWithStages) {
            if (!solutionWithPlanStage.planStage) {
                // If planStage is not already built, build it. This will be the case for CBR
                // rejected plans that are not multi-planned.
                auto execTree = stage_builder::buildClassicExecutableTree(
                    _opCtx,
                    _collections.getMainCollectionPtrOrAcquisition(),
                    *_cq,
                    *solutionWithPlanStage.solution,
                    _rankingResult.maybeExplainData->workingSetForRejectedPlansExplain.get(),
                    &planStageToQsnMap);
                solutionWithPlanStage.planStage = std::move(execTree);
            }
            if (isCountQuery) {
                tassert(11960602,
                        "Expected rejected plan to not have CountStage as root",
                        solutionWithPlanStage.planStage->stageType() != STAGE_COUNT);

                // Wrap the rejected plan's root stage in a CountStage to reflect the actual
                // execution.
                solutionWithPlanStage.planStage = std::make_unique<CountStage>(
                    _cq->getExpCtxRaw(),
                    countLimit,
                    countSkip,
                    _rankingResult.maybeExplainData->workingSetForRejectedPlansExplain.get(),
                    solutionWithPlanStage.planStage.release());
            }
        }
        for (auto& mapping : planStageToQsnMap) {
            _rankingResult.maybeExplainData->planStageQsnMap.emplace(std::move(mapping));
        }
    }

    std::unique_ptr<CanonicalQuery> _cq;
    PlanRankingResult _rankingResult;
    OperationContext* _opCtx;
    const MultipleCollectionAccessor& _collections;
    PlanYieldPolicy::YieldPolicy _yieldPolicy;
    Pipeline* _pipeline;
};

}  // namespace

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> lowerPlanRankingResult(
    std::unique_ptr<CanonicalQuery> cq,
    PlanRankingResult rankingResult,
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    Pipeline* pipeline) {
    return ExecConstructor(std::move(cq),
                           std::move(rankingResult),
                           opCtx,
                           collections,
                           std::move(yieldPolicy),
                           pipeline)
        .lower();
}

}  // namespace mongo::exec_deferred_engine_choice
