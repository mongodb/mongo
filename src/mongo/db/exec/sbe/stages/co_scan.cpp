// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/co_scan.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"

#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
CoScanStage::CoScanStage(PlanNodeId planNodeId,
                         PlanYieldPolicySBE* yieldPolicy,
                         bool participateInTrialRunTracking)
    : PlanStage("coscan"sv, yieldPolicy, planNodeId, participateInTrialRunTracking) {}

std::unique_ptr<PlanStage> CoScanStage::clone() const {
    return std::make_unique<CoScanStage>(
        _commonStats.nodeId, _yieldPolicy, participateInTrialRunTracking());
}
void CoScanStage::prepare(CompileCtx& ctx) {}
value::SlotAccessor* CoScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return ctx.getAccessor(slot);
}

void CoScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
}

PlanState CoScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    checkForInterruptAndYield(_opCtx);

    // Run forever.
    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> CoScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    return ret;
}

const SpecificStats* CoScanStage::getSpecificStats() const {
    return nullptr;
}

void CoScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
}

size_t CoScanStage::estimateCompileTimeSize() const {
    return sizeof(*this);
}

}  // namespace mongo::sbe
