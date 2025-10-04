/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/co_scan.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"

namespace mongo::sbe {
CoScanStage::CoScanStage(PlanNodeId planNodeId,
                         PlanYieldPolicy* yieldPolicy,
                         bool participateInTrialRunTracking)
    : PlanStage("coscan"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking) {}

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
