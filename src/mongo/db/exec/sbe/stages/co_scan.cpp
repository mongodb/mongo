// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/co_scan.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"

#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
CoScanStage::CoScanStage(PlanNodeId planNodeId,
                         PlanYieldPolicySBE* yieldPolicy,
                         bool participateInTrialRunTracking,
                         boost::optional<int64_t> numRows)
    : PlanStage("coscan"sv, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _numRows(numRows) {
    tassert(13684400, "CoScanStage numRows must be positive", !_numRows || *_numRows > 0);
}

std::unique_ptr<PlanStage> CoScanStage::clone() const {
    return std::make_unique<CoScanStage>(
        _commonStats.nodeId, _yieldPolicy, participateInTrialRunTracking(), _numRows);
}
void CoScanStage::prepare(CompileCtx& ctx) {}
value::SlotAccessor* CoScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return ctx.getAccessor(slot);
}

void CoScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _remaining = _numRows.value_or(0);
}

PlanState CoScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    checkForInterruptAndYield(_opCtx);

    // Run forever, unless numRows is set, in which case return EOF after numRows rows per open().
    if (_numRows) {
        if (_remaining == 0) {
            return trackPlanState(PlanState::IS_EOF);
        }
        --_remaining;
    }
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

void CoScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                               DebugPrintInfo& debugPrintInfo) const {
    if (_numRows) {
        ret.emplace_back(DebugPrinter::Block(std::to_string(*_numRows)));
    }
}

size_t CoScanStage::estimateCompileTimeSize() const {
    return sizeof(*this);
}

}  // namespace mongo::sbe
