// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/virtual_scan.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/values/value_printer.h"

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
VirtualScanStage::VirtualScanStage(PlanNodeId planNodeId,
                                   value::SlotId out,
                                   value::TagValueMaybeOwned arr,
                                   PlanYieldPolicySBE* yieldPolicy,
                                   bool participateInTrialRunTracking)
    : PlanStage("virtualscan"sv, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _outField(out),
      _arr(std::move(arr)) {
    tassert(11094700, "expect arr parameter to be an array", value::isArray(_arr.tag()));
}

std::unique_ptr<PlanStage> VirtualScanStage::clone() const {
    return std::make_unique<VirtualScanStage>(_commonStats.nodeId,
                                              _outField,
                                              _arr.getOwnedCopy(),
                                              _yieldPolicy,
                                              participateInTrialRunTracking());
}

void VirtualScanStage::prepare(CompileCtx& ctx) {
    _outFieldOutputAccessor = std::make_unique<value::ViewOfValueAccessor>();
}

value::SlotAccessor* VirtualScanStage::getAccessor(sbe::CompileCtx& ctx, sbe::value::SlotId slot) {
    if (_outField == slot) {
        return _outFieldOutputAccessor.get();
    }
    return ctx.getAccessor(slot);
}

void VirtualScanStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _values.clear();

    value::ArrayEnumerator enumerator(_arr.tag(), _arr.value());
    while (!enumerator.atEnd()) {
        auto view = enumerator.getViewOfValue();
        _values.emplace_back(value::TagValueOwned::fromRaw(value::copyValue(view.tag, view.value)));
        enumerator.advance();
    }
    _releaseIndex = 0;
    _index = 0;

    _commonStats.opens++;
}

PlanState VirtualScanStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    checkForInterruptAndYield(_opCtx);

    if (_index >= _values.size()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    _outFieldOutputAccessor->reset(_values[_index].tag(), _values[_index].value());
    _index++;

    // Depends on whether the last call was to getNext() or open()/doSaveState().
    tassert(11093511,
            "Unexpected value of releaseIndex",
            _releaseIndex == _index - 1 || _releaseIndex == _index - 2);

    // We don't want to release at _index-1, since this is the data we're in the process of
    // returning, but data at any prior index is allowed to be freed.
    if (_releaseIndex == _index - 2) {
        auto released = std::move(_values.at(_releaseIndex));
        _releaseIndex++;
    }

    return trackPlanState(PlanState::ADVANCED);
}

void VirtualScanStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
}

std::unique_ptr<PlanStageStats> VirtualScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    return ret;
}

const SpecificStats* VirtualScanStage::getSpecificStats() const {
    return nullptr;
}

void VirtualScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                    DebugPrintInfo& debugPrintInfo) const {
    auto debugPrintValue = [](value::TypeTags tag, value::Value val) {
        std::stringstream ss;
        value::ValuePrinters::make(
            ss, PrintOptions().useTagForAmbiguousValues(true).normalizeOutput(true))
            .writeValueToStream(tag, val);

        std::vector<DebugPrinter::Block> blocks;
        blocks.emplace_back(ss.str());
        return blocks;
    };

    DebugPrinter::addIdentifier(ret, _outField);

    ret.emplace_back("{`");
    DebugPrinter::addBlocks(ret, debugPrintValue(_arr.tag(), _arr.value()));
    ret.emplace_back("`}");
}

size_t VirtualScanStage::estimateCompileTimeSize() const {
    return sizeof(*this);
}

void VirtualScanStage::doSaveState() {
    for (; _releaseIndex < _index; ++_releaseIndex) {
        auto released = std::move(_values.at(_releaseIndex));
    }
}
}  // namespace mongo::sbe
