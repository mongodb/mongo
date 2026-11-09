// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"

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

WorkingSet* ClassicPlannerInterface::ws() const {
    return _plannerData.workingSet.get();
}

SavedExecState ClassicPlannerInterface::extractExecState() && {
    return ClassicExecState{.workingSet = std::move(_plannerData.workingSet),
                            .root = std::move(_root)};
}

Status ClassicPlannerInterface::plan() {
    auto classicTrialPolicy = makeClassicYieldPolicy(opCtx(), _nss, _root.get(), yieldPolicy());
    if (auto status = doPlan(classicTrialPolicy.get()); !status.isOK()) {
        return status;
    }

    _state = kInitialized;
    return Status::OK();
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
