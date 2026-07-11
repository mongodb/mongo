// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/classic/plan_stage.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_yield_policy.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
void PlanStage::saveState() {
    ++_commonStats.yields;
    for (auto&& child : _children) {
        child->saveState();
    }

    doSaveState();
}

void PlanStage::restoreState(const RestoreContext& context) {
    ++_commonStats.unyields;
    for (auto&& child : _children) {
        child->restoreState(context);
    }

    doRestoreState(context);
}

void PlanStage::detachFromOperationContext() {
    invariant(_opCtx);
    _opCtx = nullptr;

    for (auto&& child : _children) {
        child->detachFromOperationContext();
    }

    doDetachFromOperationContext();
}

void PlanStage::reattachToOperationContext(OperationContext* opCtx) {
    invariant(_opCtx == nullptr);
    _opCtx = opCtx;

    for (auto&& child : _children) {
        child->reattachToOperationContext(opCtx);
    }

    doReattachToOperationContext();
}

void PlanStage::forceSpill(PlanYieldPolicy* yieldPolicy) {
    if (yieldPolicy && yieldPolicy->shouldYieldOrInterrupt(_opCtx)) {
        uassertStatusOK(
            yieldPolicy->yieldOrInterrupt(_opCtx, nullptr, RestoreContext::RestoreType::kYield));
    }
    doForceSpill();
    for (const auto& child : _children) {
        child->forceSpill(yieldPolicy);
    }
}

}  // namespace mongo
