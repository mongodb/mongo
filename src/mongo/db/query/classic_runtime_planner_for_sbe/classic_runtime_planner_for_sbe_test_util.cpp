/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/classic_runtime_planner_for_sbe/classic_runtime_planner_for_sbe_test_util.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"

namespace mongo::sbe {

BaseMockStage::BaseMockStage(PlanNodeId planNodeId,
                             PlanYieldPolicy* yieldPolicy,
                             bool participateInTrialRunTracking)
    : PlanStage("mock"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking) {}

std::unique_ptr<PlanStage> BaseMockStage::clone() const {
    return std::make_unique<MockExceededMemoryLimitStage>(
        _commonStats.nodeId, _yieldPolicy, _participateInTrialRunTracking);
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

    if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumReads>(100)) {
        _tracker = nullptr;
        uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in scan");
    }

    // Run forever.
    return trackPlanState(PlanState::ADVANCED);
}

void MockExceededMaxReadsStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask MockExceededMaxReadsStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}


}  // namespace mongo::sbe
