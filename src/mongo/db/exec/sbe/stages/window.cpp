/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/window.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/util/spilling.h"
#include "mongo/db/exec/sbe/values/arith_common.h"

namespace mongo::sbe {

WindowStage::WindowStage(std::unique_ptr<PlanStage> input,
                         value::SlotVector currSlots,
                         value::SlotVector boundTestingSlots,
                         size_t partitionSlotCount,
                         std::vector<Window> windows,
                         boost::optional<value::SlotId> collatorSlot,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking)
    : PlanStage("window"_sd, planNodeId, participateInTrialRunTracking),
      _currSlots(std::move(currSlots)),
      _boundTestingSlots(std::move(boundTestingSlots)),
      _partitionSlotCount(partitionSlotCount),
      _windows(std::move(windows)),
      _collatorSlot(collatorSlot) {
    _children.emplace_back(std::move(input));
    tassert(7993411,
            "The number of boundTestingSlots doesn't match the number of currSlots",
            _boundTestingSlots.size() == _currSlots.size());
    for (auto& window : _windows) {
        if (window.frameFirstSlots.size()) {
            tassert(7993412,
                    "The number of frameFirstSlots doesn't match the number of currSlots",
                    window.frameFirstSlots.size() == _currSlots.size());
        }
        if (window.frameLastSlots.size()) {
            tassert(7993413,
                    "The number of frameLastSlots doesn't match the number of currSlots",
                    window.frameLastSlots.size() == _currSlots.size());
        }
    }
    tassert(7993414,
            "The partition slot count should be less or equal to the total number of slots",
            partitionSlotCount <= _currSlots.size());
}

std::unique_ptr<PlanStage> WindowStage::clone() const {
    std::vector<Window> newWindows;
    newWindows.resize(_windows.size());
    for (size_t idx = 0; idx < _windows.size(); idx++) {
        newWindows[idx].windowExprSlots = _windows[idx].windowExprSlots;
        newWindows[idx].frameFirstSlots = _windows[idx].frameFirstSlots;
        newWindows[idx].frameLastSlots = _windows[idx].frameLastSlots;
        newWindows[idx].lowBoundExpr =
            _windows[idx].lowBoundExpr ? _windows[idx].lowBoundExpr->clone() : nullptr;
        newWindows[idx].highBoundExpr =
            _windows[idx].highBoundExpr ? _windows[idx].highBoundExpr->clone() : nullptr;
        for (size_t i = 0; i < _windows[idx].initExprs.size(); ++i) {
            newWindows[idx].initExprs.push_back(
                _windows[idx].initExprs[i] ? _windows[idx].initExprs[i]->clone() : nullptr);
            newWindows[idx].addExprs.push_back(
                _windows[idx].addExprs[i] ? _windows[idx].addExprs[i]->clone() : nullptr);
            newWindows[idx].removeExprs.push_back(
                _windows[idx].removeExprs[i] ? _windows[idx].removeExprs[i]->clone() : nullptr);
        }
    }
    return std::make_unique<WindowStage>(_children[0]->clone(),
                                         _currSlots,
                                         _boundTestingSlots,
                                         _partitionSlotCount,
                                         std::move(newWindows),
                                         _collatorSlot,
                                         _commonStats.nodeId,
                                         _participateInTrialRunTracking);
}

size_t WindowStage::getLastRowId() {
    return _firstRowId + _rows.size() - 1;
}

bool WindowStage::fetchNextRow() {
    if (_isEOF) {
        return false;
    }
    auto state = _children[0]->getNext();
    if (state == PlanState::ADVANCED) {
        auto rowSize = _inCurrAccessors.size();
        value::MaterializedRow row(rowSize);
        size_t idx = 0;
        for (auto accessor : _inCurrAccessors) {
            auto [tag, val] = accessor->copyOrMoveValue();
            row.reset(idx++, true, tag, val);
        }
        _rows.push_back(std::move(row));

        // Remember new partition boundary.
        if (_rows.size() >= 2) {
            auto& row = _rows[_rows.size() - 1];
            auto& prevRow = _rows[_rows.size() - 2];
            for (idx = 0; idx < _partitionSlotCount; idx++) {
                auto [tag, val] = row.getViewOfValue(idx);
                auto [prevTag, prevVal] = prevRow.getViewOfValue(idx);
                auto [cmpTag, cmpVal] =
                    value::compareValue(tag, val, prevTag, prevVal, _collatorView);
                if (cmpTag != value::TypeTags::NumberInt32 || cmpVal != 0) {
                    _nextPartitionId = _firstRowId + _rows.size() - 1;
                    break;
                }
            }
        }
        return true;
    } else {
        _isEOF = true;
        return false;
    }
}

void WindowStage::freeUntilRow(size_t requiredId) {
    for (size_t id = _firstRowId; id < requiredId && _rows.size(); id++) {
        _rows.pop_front();
    }
    _firstRowId = std::max(_firstRowId, requiredId);
    // Clear next partition id once we free everything from the previous partition.
    if (_nextPartitionId && _firstRowId >= *_nextPartitionId) {
        _nextPartitionId = boost::none;
    }
}

void WindowStage::freeRows() {
    _rows.clear();
    _firstRowId = 1;
    _nextPartitionId = boost::none;
    _isEOF = false;
}

void WindowStage::setCurrAccessors(size_t id) {
    invariant(id >= _firstRowId && id < _firstRowId + _rows.size());
    _currRowIdx = id - _firstRowId;
}

void WindowStage::setFrameFirstAccessors(size_t windowIdx, size_t firstId) {
    invariant(firstId >= _firstRowId && firstId < _firstRowId + _rows.size());
    for (auto&& switchAccessor : _outFrameFirstAccessors[windowIdx]) {
        switchAccessor->setIndex(0);
    }
    _frameFirstRowIdxes[windowIdx] = firstId - _firstRowId;
}

void WindowStage::clearFrameFirstAccessors(size_t windowIdx) {
    for (auto&& switchAccessor : _outFrameFirstAccessors[windowIdx]) {
        switchAccessor->setIndex(1);
    }
}

void WindowStage::setFrameLastAccessors(size_t windowIdx, size_t lastId) {
    invariant(lastId >= _firstRowId && lastId < _firstRowId + _rows.size());
    for (auto&& switchAccessor : _outFrameLastAccessors[windowIdx]) {
        switchAccessor->setIndex(0);
    }
    _frameLastRowIdxes[windowIdx] = lastId - _firstRowId;
}

void WindowStage::clearFrameLastAccessors(size_t windowIdx) {
    for (auto&& switchAccessor : _outFrameLastAccessors[windowIdx]) {
        switchAccessor->setIndex(1);
    }
}

void WindowStage::setBoundTestingAccessors(size_t id) {
    invariant(id >= _firstRowId && id < _firstRowId + _rows.size());
    _boundTestingRowIdx = id - _firstRowId;
}

void WindowStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    size_t slotIdx = 0;
    _inCurrAccessors.reserve(_currSlots.size());
    _outCurrAccessors.reserve(_currSlots.size());
    for (auto slot : _currSlots) {
        _inCurrAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        _outCurrAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _currRowIdx, slotIdx++));
        _outAccessorMap.emplace(slot, _outCurrAccessors.back().get());
    }

    slotIdx = 0;
    _boundTestingAccessors.reserve(_boundTestingSlots.size());
    for (auto slot : _boundTestingSlots) {
        _boundTestingAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _boundTestingRowIdx, slotIdx++));
        _boundTestingAccessorMap.emplace(slot, _boundTestingAccessors.back().get());
    }

    _emptyAccessor = std::make_unique<value::OwnedValueAccessor>();
    _outFrameFirstAccessors.reserve(_windows.size());
    _outFrameFirstRowAccessors.reserve(_windows.size());
    _frameFirstRowIdxes.reserve(_windows.size());
    _outFrameLastAccessors.reserve(_windows.size());
    _outFrameLastRowAccessors.reserve(_windows.size());
    _frameLastRowIdxes.reserve(_windows.size());
    _outWindowAccessors.reserve(_windows.size());
    _windowLowBoundCodes.reserve(_windows.size());
    _windowHighBoundCodes.reserve(_windows.size());
    _windowInitCodes.reserve(_windows.size());
    _windowAddCodes.reserve(_windows.size());
    _windowRemoveCodes.reserve(_windows.size());
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];

        _outFrameFirstRowAccessors.push_back(std::vector<std::unique_ptr<BufferedRowAccessor>>());
        _outFrameFirstAccessors.push_back(std::vector<std::unique_ptr<value::SwitchAccessor>>());
        _frameFirstRowIdxes.push_back(-1);
        slotIdx = 0;
        for (auto slot : window.frameFirstSlots) {
            _outFrameFirstRowAccessors[windowIdx].push_back(std::make_unique<BufferedRowAccessor>(
                _rows, _frameFirstRowIdxes[windowIdx], slotIdx++));
            std::vector<value::SlotAccessor*> switchAccessors{
                _outFrameFirstRowAccessors[windowIdx].back().get(), _emptyAccessor.get()};
            _outFrameFirstAccessors[windowIdx].push_back(
                std::make_unique<value::SwitchAccessor>(std::move(switchAccessors)));
            _outAccessorMap.emplace(slot, _outFrameFirstAccessors[windowIdx].back().get());
        }

        _outFrameLastRowAccessors.push_back(std::vector<std::unique_ptr<BufferedRowAccessor>>());
        _outFrameLastAccessors.push_back(std::vector<std::unique_ptr<value::SwitchAccessor>>());
        _frameLastRowIdxes.push_back(-1);
        slotIdx = 0;
        for (auto slot : window.frameLastSlots) {
            _outFrameLastRowAccessors[windowIdx].push_back(std::make_unique<BufferedRowAccessor>(
                _rows, _frameLastRowIdxes[windowIdx], slotIdx++));
            std::vector<value::SlotAccessor*> switchAccessors{
                _outFrameLastRowAccessors[windowIdx].back().get(), _emptyAccessor.get()};
            _outFrameLastAccessors[windowIdx].push_back(
                std::make_unique<value::SwitchAccessor>(std::move(switchAccessors)));
            _outAccessorMap.emplace(slot, _outFrameLastAccessors[windowIdx].back().get());
        }

        ctx.root = this;

        std::vector<std::unique_ptr<value::OwnedValueAccessor>> outAccessors;
        std::vector<std::unique_ptr<vm::CodeFragment>> initCodes;
        std::vector<std::unique_ptr<vm::CodeFragment>> addCodes;
        std::vector<std::unique_ptr<vm::CodeFragment>> removeCodes;

        auto initExprsSize = window.initExprs.size();
        outAccessors.reserve(initExprsSize);
        initCodes.reserve(initExprsSize);
        addCodes.reserve(initExprsSize);
        removeCodes.reserve(initExprsSize);

        for (size_t i = 0; i < initExprsSize; ++i) {
            outAccessors.push_back(std::make_unique<value::OwnedValueAccessor>());
            _outAccessorMap.emplace(window.windowExprSlots[i], outAccessors.back().get());

            initCodes.push_back(window.initExprs[i] ? window.initExprs[i]->compile(ctx) : nullptr);
            ctx.aggExpression = true;
            ctx.accumulator = outAccessors.back().get();
            addCodes.push_back(window.addExprs[i] ? window.addExprs[i]->compile(ctx) : nullptr);
            removeCodes.push_back(window.removeExprs[i] ? window.removeExprs[i]->compile(ctx)
                                                        : nullptr);
            ctx.aggExpression = false;
        }

        _windowLowBoundCodes.push_back(window.lowBoundExpr ? window.lowBoundExpr->compile(ctx)
                                                           : nullptr);
        _windowHighBoundCodes.push_back(window.highBoundExpr ? window.highBoundExpr->compile(ctx)
                                                             : nullptr);

        _outWindowAccessors.push_back(std::move(outAccessors));
        _windowInitCodes.push_back(std::move(initCodes));
        _windowAddCodes.push_back(std::move(addCodes));
        _windowRemoveCodes.push_back(std::move(removeCodes));
    }
    _compiled = true;

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(7870800,
                "collator accessor should exist if collator slot provided to WindowStage",
                _collatorAccessor != nullptr);
    }
}

value::SlotAccessor* WindowStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (!_compiled) {
        if (auto it = _boundTestingAccessorMap.find(slot); it != _boundTestingAccessorMap.end()) {
            return it->second;
        }
    }
    if (auto it = _outAccessorMap.find(slot); it != _outAccessorMap.end()) {
        return it->second;
    }
    return ctx.getAccessor(slot);
}

void WindowStage::resetPartition(int startId) {
    _currPartitionId = startId;
    _windowIdRanges.clear();
    _windowIdRanges.resize(_windows.size(), std::make_pair(startId, startId - 1));
    for (size_t idx = 0; idx < _windows.size(); idx++) {
        auto& windowAccessors = _outWindowAccessors[idx];
        auto& windowInitCodes = _windowInitCodes[idx];

        for (size_t i = 0; i < windowInitCodes.size(); ++i) {
            if (windowInitCodes[i]) {
                auto [owned, tag, val] = _bytecode.run(windowInitCodes[i].get());
                windowAccessors[i]->reset(owned, tag, val);
            } else {
                windowAccessors[i]->reset();
            }
        }
    }
}

void WindowStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    if (_collatorAccessor) {
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        uassert(7870801, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        _collatorView = value::getCollatorView(collatorVal);
    }

    _commonStats.opens++;

    _children[0]->open(reOpen);

    _currId = 0;
    resetPartition(1);
    freeRows();
}

PlanState WindowStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterrupt(_opCtx);

    // Fetch at least the current document into cache.
    _currId++;
    if (_currId > getLastRowId()) {
        auto fetched = fetchNextRow();
        if (!fetched) {
            return trackPlanState(PlanState::IS_EOF);
        }
    }
    invariant(_currId <= getLastRowId());

    // Partition boundary check.
    if (_currId == _nextPartitionId) {
        freeUntilRow(_currId);
        resetPartition(_currId);
    }

    // Add documents into window.
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];
        auto& idRange = _windowIdRanges[windowIdx];
        auto& windowAccessors = _outWindowAccessors[windowIdx];
        auto& windowAddCodes = _windowAddCodes[windowIdx];

        for (size_t id = idRange.second + 1;; id++) {
            // Fetch document if not already in cache.
            if (id > getLastRowId()) {
                auto fetched = fetchNextRow();
                if (!fetched) {
                    break;
                }
            }

            // Partition boundary check.
            if (id == _nextPartitionId) {
                break;
            }

            // Set accessors for the current document and the testing document to perform bound
            // check.
            bool inBound = true;
            if (window.highBoundExpr) {
                setCurrAccessors(_currId);
                setBoundTestingAccessors(id);
                inBound = _bytecode.runPredicate(_windowHighBoundCodes[windowIdx].get());
            }

            // Run aggregation if this document is inside the desired range.
            if (inBound) {
                setCurrAccessors(id);
                for (size_t i = 0; i < windowAddCodes.size(); ++i) {
                    if (windowAddCodes[i]) {
                        auto [owned, tag, val] = _bytecode.run(windowAddCodes[i].get());
                        windowAccessors[i]->reset(owned, tag, val);
                    }
                }
                idRange.second = id;
            } else {
                break;
            }
        }
    }

    // Remove documents from window if it's not unbounded.
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];
        auto& idRange = _windowIdRanges[windowIdx];
        auto& windowAccessors = _outWindowAccessors[windowIdx];
        auto& windowRemoveCodes = _windowRemoveCodes[windowIdx];

        if (window.lowBoundExpr) {
            for (size_t id = idRange.first; idRange.first <= idRange.second; id++) {
                // Set accessors for the current document and the testing document to perform bound
                // check.
                setCurrAccessors(_currId);
                setBoundTestingAccessors(id);
                bool inBound = _bytecode.runPredicate(_windowLowBoundCodes[windowIdx].get());

                // Undo the aggregation if this document is being removed from the updated range.
                if (!inBound) {
                    setCurrAccessors(id);
                    for (size_t i = 0; i < windowRemoveCodes.size(); ++i) {
                        if (windowRemoveCodes[i]) {
                            auto [owned, tag, val] = _bytecode.run(windowRemoveCodes[i].get());
                            windowAccessors[i]->reset(owned, tag, val);
                        }
                    }
                    idRange.first = id + 1;
                } else {
                    break;
                }
            }
        }
    }

    // Free documents from cache if not needed.
    size_t requiredIdLow = _currId;
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        // We can't free past what hasn't been added to idRange.
        // Additionally, if lower bound is not unbounded, we can't free anything that has been added
        // to the range, since its needed to be removed later.
        requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIdx].second + 1);
        if (_windows[windowIdx].lowBoundExpr) {
            requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIdx].first);
        } else {
            if (_windows[windowIdx].frameLastSlots.size()) {
                requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIdx].second);
            }
            if (_windows[windowIdx].frameFirstSlots.size()) {
                requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIdx].first);
            }
        }
    }
    freeUntilRow(requiredIdLow);

    // Set current and frame first/last accessors for the document.
    setCurrAccessors(_currId);
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& idRange = _windowIdRanges[windowIdx];
        if (idRange.first <= idRange.second) {
            if (_windows[windowIdx].frameFirstSlots.size()) {
                setFrameFirstAccessors(windowIdx, idRange.first);
            } else {
                clearFrameFirstAccessors(windowIdx);
            }
            if (_windows[windowIdx].frameLastSlots.size()) {
                setFrameLastAccessors(windowIdx, idRange.second);
            } else {
                clearFrameLastAccessors(windowIdx);
            }
        } else {
            clearFrameFirstAccessors(windowIdx);
            clearFrameLastAccessors(windowIdx);
        }
    }

    return trackPlanState(PlanState::ADVANCED);
}

void WindowStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _children[0]->close();
    freeRows();
}

std::vector<DebugPrinter::Block> WindowStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _currSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _currSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    for (size_t windowIdx = 0; windowIdx < _windows.size(); ++windowIdx) {
        const auto& window = _windows[windowIdx];
        if (windowIdx) {
            DebugPrinter::addNewLine(ret);
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        ret.emplace_back("[frameFirst[`");
        for (size_t slotIdx = 0; slotIdx < window.frameFirstSlots.size(); slotIdx++) {
            if (slotIdx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, window.frameFirstSlots[slotIdx]);
        }
        ret.emplace_back("`],");

        ret.emplace_back("frameLast[`");
        for (size_t slotIdx = 0; slotIdx < window.frameLastSlots.size(); slotIdx++) {
            if (slotIdx) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, window.frameLastSlots[slotIdx]);
        }
        ret.emplace_back("`],");

        ret.emplace_back("lowBound{`");
        if (window.lowBoundExpr) {
            DebugPrinter::addBlocks(ret, window.lowBoundExpr->debugPrint());
        }
        ret.emplace_back("`},");
        ret.emplace_back("highBound{`");
        if (window.highBoundExpr) {
            DebugPrinter::addBlocks(ret, window.highBoundExpr->debugPrint());
        }
        ret.emplace_back("`}]");

        ret.emplace_back(DebugPrinter::Block("[`"));
        for (size_t i = 0; i < window.initExprs.size(); ++i) {
            if (i) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            DebugPrinter::addIdentifier(ret, window.windowExprSlots[i]);
            ret.emplace_back("=");
            ret.emplace_back("{init{`");
            if (window.initExprs[i]) {
                DebugPrinter::addBlocks(ret, window.initExprs[i]->debugPrint());
            }
            ret.emplace_back("`},");
            ret.emplace_back("add{`");
            if (window.addExprs[i]) {
                DebugPrinter::addBlocks(ret, window.addExprs[i]->debugPrint());
            }
            ret.emplace_back("`},");
            ret.emplace_back("remove{`");
            if (window.removeExprs[i]) {
                DebugPrinter::addBlocks(ret, window.removeExprs[i]->debugPrint());
            }
            ret.emplace_back("`}}");
        }
        ret.emplace_back("`]");
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

std::unique_ptr<PlanStageStats> WindowStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<WindowStats>(_specificStats);
    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* WindowStage::getSpecificStats() const {
    return &_specificStats;
}

size_t WindowStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_currSlots);
    size += size_estimator::estimate(_boundTestingSlots);
    size += size_estimator::estimate(_windows);
    return size;
}

}  // namespace mongo::sbe
