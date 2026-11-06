// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"

#include "mongo/db/exec/classic/count.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"

namespace mongo::classic_runtime_planner {

ClassicPlannerInterface::ClassicPlannerInterface(PlannerData plannerData,
                                                 PlanSelectionStrategy planSelectionStrategy)
    : ClassicPlannerInterface(std::move(plannerData), PlanExplainerData{}, planSelectionStrategy) {}

ClassicPlannerInterface::ClassicPlannerInterface(PlannerData plannerData,
                                                 PlanExplainerData explainData,
                                                 PlanSelectionStrategy planSelectionStrategy)
    : _plannerData(std::move(plannerData)),
      _planExplainerData(std::move(explainData)),
      _planSelectionStrategy(planSelectionStrategy) {
    if (collections().hasMainCollection()) {
        _nss = collections().getMainCollection()->ns();
    } else {
        invariant(cq());
        const auto nssOrUuid = cq()->getFindCommandRequest().getNamespaceOrUUID();
        _nss = nssOrUuid.isNamespaceString() ? nssOrUuid.nss() : NamespaceString::kEmpty;
    }
}

OperationContext* ClassicPlannerInterface::opCtx() {
    return _plannerData.opCtx;
}

const CanonicalQuery* ClassicPlannerInterface::cq() const {
    return _plannerData.cq;
}

const MultipleCollectionAccessor& ClassicPlannerInterface::collections() const {
    return _plannerData.collections;
}

PlanYieldPolicy::YieldPolicy ClassicPlannerInterface::yieldPolicy() const {
    return collections().hasMainCollection() ? _plannerData.yieldPolicy
                                             : PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY;
}

const QueryPlannerParams& ClassicPlannerInterface::plannerParams() {
    return *_plannerData.plannerParams;
}

size_t ClassicPlannerInterface::plannerOptions() const {
    return _plannerData.plannerParams->mainCollectionInfo.options;
}

boost::optional<size_t> ClassicPlannerInterface::cachedPlanHash() const {
    return _plannerData.cachedPlanHash;
}

boost::optional<std::string> ClassicPlannerInterface::replanReason() const {
    if (!_plannerData.plannerParams->replanningData) {
        return boost::none;
    }
    return _plannerData.plannerParams->replanningData->replanReason;
}

WorkingSet* ClassicPlannerInterface::ws() const {
    return _plannerData.workingSet.get();
}

SavedExecState ClassicPlannerInterface::extractExecState() && {
    return ClassicExecState{.workingSet = std::move(_plannerData.workingSet),
                            .root = std::move(_root)};
}

void ClassicPlannerInterface::addCountStage(long long limit, long long skip) {
    invariant(_state == kNotInitialized);
    // Make a CountStage to be the new root.
    _root = std::make_unique<CountStage>(cq()->getExpCtxRaw(), limit, skip, ws(), _root.release());
}

Status ClassicPlannerInterface::plan() {
    auto classicTrialPolicy = makeClassicYieldPolicy(opCtx(), _nss, _root.get(), yieldPolicy());
    if (auto status = doPlan(classicTrialPolicy.get()); !status.isOK()) {
        return status;
    }

    _state = kInitialized;
    return Status::OK();
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> ClassicPlannerInterface::makeExecutor(
    std::unique_ptr<CanonicalQuery> canonicalQuery) {
    invariant(_state == kInitialized);
    if (cq()->getExplain().has_value()) {
        bool isCountQuery = getRoot()->stageType() == STAGE_COUNT;
        // This loop will not run if pure multiplanning is configured (featureFlagCostBasedRanker:
        // false) since rejected solutions in that case live in the multiplanner itself.
        for (auto&& solutionWithPlanStage : _planExplainerData.rejectedPlansWithStages) {
            if (!solutionWithPlanStage.planStage) {
                // If planStage is not already built, build it. This will be the case for CBR
                // rejected plans that are not multi-planned.
                auto execTree = buildExecutableTree(*solutionWithPlanStage.solution);
                solutionWithPlanStage.planStage = std::move(execTree);
            }
            if (isCountQuery) {
                tassert(11777400,
                        "Expected rejected plan to not have CountStage as root",
                        solutionWithPlanStage.planStage->stageType() != STAGE_COUNT);

                // Wrap the rejected plan's root stage in a CountStage to reflect the actual
                // execution.
                solutionWithPlanStage.planStage =
                    std::make_unique<CountStage>(cq()->getExpCtxRaw(),
                                                 static_cast<CountStage*>(getRoot())->getLimit(),
                                                 static_cast<CountStage*>(getRoot())->getSkip(),
                                                 ws(),
                                                 solutionWithPlanStage.planStage.release());
            }
        }
        for (auto& mapping : _planStageQsnMap) {
            _planExplainerData.planStageQsnMap.emplace(std::move(mapping));
        }
    }
    _state = kDisposed;

    return plan_executor_factory::make(opCtx(),
                                       std::move(_plannerData.workingSet),
                                       std::move(_root),
                                       extractQuerySolution(),
                                       std::move(canonicalQuery),
                                       cq()->getExpCtx(),
                                       collections().getMainCollectionAcquisition(),
                                       plannerOptions(),
                                       std::move(_nss),
                                       yieldPolicy(),
                                       cachedPlanHash(),
                                       replanReason(),
                                       std::move(_planExplainerData),
                                       planSelectionStrategy());
}

std::unique_ptr<PlanStage> ClassicPlannerInterface::buildExecutableTree(const QuerySolution& qs) {
    return stage_builder::buildClassicExecutableTree(
        opCtx(),
        collections().getMainCollectionPtrOrAcquisition(),
        *cq(),
        qs,
        ws(),
        &_planStageQsnMap);
}

PlanStage* ClassicPlannerInterface::getRoot() const {
    return _root.get();
}

void ClassicPlannerInterface::setRoot(std::unique_ptr<PlanStage> root) {
    _root = std::move(root);
}
}  // namespace mongo::classic_runtime_planner
