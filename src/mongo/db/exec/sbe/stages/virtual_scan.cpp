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

#include "mongo/db/exec/sbe/stages/virtual_scan.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/values/value_printer.h"

namespace mongo::sbe {
VirtualScanStage::VirtualScanStage(PlanNodeId planNodeId,
                                   value::SlotId out,
                                   value::TypeTags arrTag,
                                   value::Value arrVal,
                                   PlanYieldPolicy* yieldPolicy,
                                   bool participateInTrialRunTracking)
    : PlanStage("virtualscan"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _outField(out),
      _arrTag(arrTag),
      _arrVal(arrVal) {
    invariant(value::isArray(arrTag));
}

VirtualScanStage::~VirtualScanStage() {
    value::releaseValue(_arrTag, _arrVal);
    for (; _releaseIndex < _values.size(); ++_releaseIndex) {
        auto [tagElem, valueElem] = _values.at(_releaseIndex);
        value::releaseValue(tagElem, valueElem);
    }
}

std::unique_ptr<PlanStage> VirtualScanStage::clone() const {
    auto [tag, val] = value::copyValue(_arrTag, _arrVal);
    return std::make_unique<VirtualScanStage>(
        _commonStats.nodeId, _outField, tag, val, _yieldPolicy, participateInTrialRunTracking());
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

    for (; _releaseIndex < _values.size(); ++_releaseIndex) {
        auto [tagElem, valueElem] = _values.at(_releaseIndex);
        value::releaseValue(tagElem, valueElem);
    }
    _values.clear();

    value::ArrayEnumerator enumerator(_arrTag, _arrVal);
    while (!enumerator.atEnd()) {
        auto [tagElem, valueElem] = enumerator.getViewOfValue();
        _values.push_back(value::copyValue(tagElem, valueElem));
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

    auto [tagElem, valueElem] = _values.at(_index);
    _outFieldOutputAccessor->reset(tagElem, valueElem);
    _index++;

    // Depends on whether the last call was to getNext() or open()/doSaveState().
    invariant(_releaseIndex == _index - 1 || _releaseIndex == _index - 2);

    // We don't want to release at _index-1, since this is the data we're in the process of
    // returning, but data at any prior index is allowed to be freed.
    if (_releaseIndex == _index - 2) {
        auto [returnedTagElem, returnedValueElem] = _values.at(_releaseIndex);
        value::releaseValue(returnedTagElem, returnedValueElem);
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

std::vector<DebugPrinter::Block> VirtualScanStage::debugPrint() const {
    auto debugPrintValue = [](value::TypeTags tag, value::Value val) {
        std::stringstream ss;
        value::ValuePrinters::make(
            ss, PrintOptions().useTagForAmbiguousValues(true).normalizeOutput(true))
            .writeValueToStream(tag, val);

        std::vector<DebugPrinter::Block> blocks;
        blocks.emplace_back(ss.str());
        return blocks;
    };

    std::vector<DebugPrinter::Block> ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _outField);

    ret.emplace_back("{`");
    DebugPrinter::addBlocks(ret, debugPrintValue(_arrTag, _arrVal));
    ret.emplace_back("`}");

    return ret;
}

size_t VirtualScanStage::estimateCompileTimeSize() const {
    return sizeof(*this);
}

void VirtualScanStage::doSaveState() {
    for (; _releaseIndex < _index; ++_releaseIndex) {
        auto [tagElem, valueElem] = _values.at(_releaseIndex);
        value::releaseValue(tagElem, valueElem);
    }
}
}  // namespace mongo::sbe
