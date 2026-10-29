// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/classic_runtime_planner_for_sbe_test_util.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

BaseMockStage::BaseMockStage(PlanNodeId planNodeId,
                             PlanYieldPolicySBE* yieldPolicy,
                             bool participateInTrialRunTracking)
    : PlanStage("mock"sv,
                yieldPolicy,
                planNodeId,
                participateInTrialRunTracking,
                TrialRunTrackingType::TrackReads) {}

std::unique_ptr<PlanStage> BaseMockStage::clone() const {
    return std::make_unique<MockExceededMemoryLimitStage>(
        _commonStats.nodeId, _yieldPolicy, participateInTrialRunTracking());
}
void BaseMockStage::prepare(CompileCtx& ctx) {}

value::SlotAccessor* BaseMockStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return ctx.getAccessor(slot);
}
void BaseMockStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
}
void BaseMockStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
}

std::unique_ptr<PlanStageStats> BaseMockStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    return ret;
}
const SpecificStats* BaseMockStage::getSpecificStats() const {
    return nullptr;
}
size_t BaseMockStage::estimateCompileTimeSize() const {
    return sizeof(*this);
}

value::SlotAccessor* MockExceededMemoryLimitStage::getAccessor(CompileCtx& ctx,
                                                               value::SlotId slot) {
    return ctx.getAccessor(slot);
}
PlanState MockExceededMemoryLimitStage::getNext() {
    uasserted(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
              "Mock stage to throw memory exceeds error");
    auto optTimer(getOptTimer(_opCtx));

    checkForInterruptAndYield(_opCtx);

    // Run forever.
    return trackPlanState(PlanState::ADVANCED);
}

void MockExceededMaxReadsStage::prepare(CompileCtx& ctx) {
    _recordAccessor = std::make_unique<value::ViewOfValueAccessor>();
}

value::SlotAccessor* MockExceededMaxReadsStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _recordAccessor.get();
}

PlanState MockExceededMaxReadsStage::getNext() {
    if (_recordAccessor) {
        _recordAccessor->reset(value::TypeTags::bsonObject,
                               value::bitcastFrom<const char*>(BSONObj{}.objdata()));
    }
    auto optTimer(getOptTimer(_opCtx));

    checkForInterruptAndYield(_opCtx);

    for (size_t i = 0; i < 100; ++i) {
        trackRead();
    }

    // Run forever.
    return trackPlanState(PlanState::ADVANCED);
}

}  // namespace mongo::sbe
