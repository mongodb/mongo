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
                         value::SlotVector partitionSlots,
                         value::SlotVector forwardSlots,
                         std::vector<Window> windows,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking)
    : PlanStage("window"_sd, planNodeId, participateInTrialRunTracking),
      _partitionSlots(std::move(partitionSlots)),
      _forwardSlots(std::move(forwardSlots)),
      _windows(std::move(windows)) {
    _children.emplace_back(std::move(input));

    // Dedupe the list of window bound slots and remember the index for each window.
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto dedupeBoundSlot = [&](value::SlotId boundSlot) {
            for (size_t boundIdx = 0; boundIdx < _boundSlots.size(); boundIdx++) {
                if (boundSlot == _boundSlots[boundIdx]) {
                    return boundIdx;
                }
            }
            // If we didn't find this bound slot previously.
            _boundSlots.push_back(boundSlot);
            return _boundSlots.size() - 1;
        };
        _lowBoundSlotIndex.push_back(boost::none);
        if (_windows[windowIdx].lowBoundSlot) {
            _lowBoundSlotIndex[windowIdx] = dedupeBoundSlot(*_windows[windowIdx].lowBoundSlot);
        }
        _highBoundSlotIndex.push_back(boost::none);
        if (_windows[windowIdx].highBoundSlot) {
            _highBoundSlotIndex[windowIdx] = dedupeBoundSlot(*_windows[windowIdx].highBoundSlot);
        }
    }
}

std::unique_ptr<PlanStage> WindowStage::clone() const {
    std::vector<Window> newWindows;
    newWindows.resize(_windows.size());
    for (size_t idx = 0; idx < _windows.size(); idx++) {
        newWindows[idx].windowSlot = _windows[idx].windowSlot;
        newWindows[idx].lowBoundSlot = _windows[idx].lowBoundSlot;
        newWindows[idx].highBoundSlot = _windows[idx].highBoundSlot;
        newWindows[idx].lowBoundTestingSlot = _windows[idx].lowBoundTestingSlot;
        newWindows[idx].highBoundTestingSlot = _windows[idx].highBoundTestingSlot;
        newWindows[idx].lowBoundExpr =
            _windows[idx].lowBoundExpr ? _windows[idx].lowBoundExpr->clone() : nullptr;
        newWindows[idx].highBoundExpr =
            _windows[idx].highBoundExpr ? _windows[idx].highBoundExpr->clone() : nullptr;
        newWindows[idx].initExpr =
            _windows[idx].initExpr ? _windows[idx].initExpr->clone() : nullptr;
        newWindows[idx].addExpr = _windows[idx].addExpr->clone();
        newWindows[idx].removeExpr =
            _windows[idx].removeExpr ? _windows[idx].removeExpr->clone() : nullptr;
    }
    return std::make_unique<WindowStage>(_children[0]->clone(),
                                         _partitionSlots,
                                         _forwardSlots,
                                         std::move(newWindows),
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
        auto rowSize =
            _inPartitionAccessors.size() + _inForwardAccessors.size() + _inBoundAccessors.size();
        value::MaterializedRow row(rowSize);
        size_t idx = 0;
        for (auto partitionAccessor : _inPartitionAccessors) {
            auto [tag, val] = partitionAccessor->copyOrMoveValue();
            row.reset(idx++, true, tag, val);
        }
        for (auto forwardAccessor : _inForwardAccessors) {
            auto [tag, val] = forwardAccessor->copyOrMoveValue();
            row.reset(idx++, true, tag, val);
        }
        for (auto boundAccessor : _inBoundAccessors) {
            auto [tag, val] = boundAccessor->copyOrMoveValue();
            row.reset(idx++, true, tag, val);
        }
        _rows.push_back(std::move(row));

        // Remember new partition boundary.
        if (_rows.size() >= 2) {
            auto& row = _rows[_rows.size() - 1];
            auto& prevRow = _rows[_rows.size() - 2];
            for (idx = 0; idx < _partitionSlots.size(); idx++) {
                auto [tag, val] = row.getViewOfValue(idx);
                auto [prevTag, prevVal] = prevRow.getViewOfValue(idx);
                auto [cmpTag, cmpVal] = value::compareValue(tag, val, prevTag, prevVal);
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

void WindowStage::freeUntilRow(size_t untilId) {
    for (size_t id = _firstRowId; id <= untilId && _rows.size(); id++) {
        _rows.pop_front();
    }
    _firstRowId = untilId + 1;
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

void WindowStage::setOutAccessors(size_t id) {
    invariant(id >= _firstRowId && id < _firstRowId + _rows.size());
    _outRowIdx = id - _firstRowId;
}

void WindowStage::setBoundTestingAccessor(size_t id) {
    invariant(id >= _firstRowId && id < _firstRowId + _rows.size());
    _boundTestingRowIdx = id - _firstRowId;
}

void WindowStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    _inPartitionAccessors.reserve(_partitionSlots.size());
    _outPartitionAccessors.reserve(_partitionSlots.size());
    size_t slotIdx = 0;
    for (auto slot : _partitionSlots) {
        _inPartitionAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        _outPartitionAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _outRowIdx, slotIdx++));
        _outAccessorMap.emplace(slot, _outPartitionAccessors.back().get());
    }

    _inForwardAccessors.reserve(_forwardSlots.size());
    _outForwardAccessors.reserve(_forwardSlots.size());
    for (auto slot : _forwardSlots) {
        _inForwardAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        _outForwardAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _outRowIdx, slotIdx++));
        _outAccessorMap.emplace(slot, _outForwardAccessors.back().get());
    }

    _inBoundAccessors.reserve(_boundSlots.size());
    _outBoundAccessors.reserve(_boundSlots.size());
    for (auto slot : _boundSlots) {
        _inBoundAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        _outBoundAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _outRowIdx, slotIdx++));
        _outAccessorMap.emplace(slot, _outBoundAccessors.back().get());
    }

    _outWindowAccessors.reserve(_windows.size());
    _windowLowBoundCodes.reserve(_windows.size());
    _windowHighBoundCodes.reserve(_windows.size());
    _windowInitCodes.reserve(_windows.size());
    _windowAddCodes.reserve(_windows.size());
    _windowRemoveCodes.reserve(_windows.size());
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];
        _outWindowAccessors.push_back(std::make_unique<value::OwnedValueAccessor>());
        _outAccessorMap.emplace(window.windowSlot, _outWindowAccessors.back().get());

        if (window.lowBoundExpr && window.lowBoundTestingSlot) {
            slotIdx =
                _partitionSlots.size() + _forwardSlots.size() + *_lowBoundSlotIndex[windowIdx];
            _lowBoundTestingAccessors.push_back(
                std::make_unique<BufferedRowAccessor>(_rows, _boundTestingRowIdx, slotIdx));
            _boundTestingAccessorMap.emplace(*window.lowBoundTestingSlot,
                                             _lowBoundTestingAccessors.back().get());
        }

        if (window.highBoundExpr && window.highBoundTestingSlot) {
            slotIdx =
                _partitionSlots.size() + _forwardSlots.size() + *_highBoundSlotIndex[windowIdx];
            _highBoundTestingAccessors.push_back(
                std::make_unique<BufferedRowAccessor>(_rows, _boundTestingRowIdx, slotIdx));
            _boundTestingAccessorMap.emplace(*window.highBoundTestingSlot,
                                             _highBoundTestingAccessors.back().get());
        }

        ctx.root = this;
        _windowLowBoundCodes.push_back(window.lowBoundExpr ? window.lowBoundExpr->compile(ctx)
                                                           : nullptr);
        _windowHighBoundCodes.push_back(window.highBoundExpr ? window.highBoundExpr->compile(ctx)
                                                             : nullptr);
        _windowInitCodes.push_back(window.initExpr ? window.initExpr->compile(ctx) : nullptr);
        ctx.aggExpression = true;
        ctx.accumulator = _outWindowAccessors.back().get();
        _windowAddCodes.push_back(window.addExpr->compile(ctx));
        _windowRemoveCodes.push_back(window.removeExpr ? window.removeExpr->compile(ctx) : nullptr);
        ctx.aggExpression = false;
    }
    _compiled = true;
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

void WindowStage::resetWindowRange(int startId) {
    _windowIdRanges.clear();
    _windowIdRanges.resize(_windows.size(), std::make_pair(startId, startId - 1));
    for (size_t idx = 0; idx < _windows.size(); idx++) {
        if (_windows[idx].initExpr) {
            auto [owned, tag, val] = _bytecode.run(_windowInitCodes[idx].get());
            _outWindowAccessors[idx]->reset(owned, tag, val);
        } else {
            _outWindowAccessors[idx]->reset();
        }
    }
}

void WindowStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    _commonStats.opens++;

    _children[0]->open(reOpen);

    freeRows();
    _currId = 0;
    resetWindowRange(1);
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
        freeUntilRow(_currId - 1);
        resetWindowRange(_currId);
    }

    // Add documents into window.
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];
        auto& idRange = _windowIdRanges[windowIdx];
        auto& windowAccessor = _outWindowAccessors[windowIdx];

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
                setOutAccessors(_currId);
                setBoundTestingAccessor(id);
                inBound = _bytecode.runPredicate(_windowHighBoundCodes[windowIdx].get());
            }

            // Run aggregation if this document is inside the desired range.
            if (inBound) {
                setOutAccessors(id);
                auto [owned, tag, val] = _bytecode.run(_windowAddCodes[windowIdx].get());
                windowAccessor->reset(owned, tag, val);
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
        auto& windowAccessor = _outWindowAccessors[windowIdx];

        if (window.lowBoundExpr) {
            for (size_t id = idRange.first; idRange.first <= idRange.second; id++) {
                // Set accessors for the current document and the testing document to perform bound
                // check.
                setOutAccessors(_currId);
                setBoundTestingAccessor(id);
                bool inBound = _bytecode.runPredicate(_windowLowBoundCodes[windowIdx].get());

                // Undo the aggregation if this document is being removed from the updated range.
                if (!inBound) {
                    setOutAccessors(id);
                    auto [owned, tag, val] = _bytecode.run(_windowRemoveCodes[windowIdx].get());
                    windowAccessor->reset(owned, tag, val);
                    idRange.first = id + 1;
                } else {
                    break;
                }
            }
        }
    }

    // Free documents from cache if not needed.
    size_t requiredIdLow = _currId;
    for (size_t windowIndex = 0; windowIndex < _windows.size(); windowIndex++) {
        // We can't free past what hasn't been added to idRange.
        // Additionally, if lower bound is not unbounded, we can't free anything that has been added
        // to the range, since its needed to be removed later.
        requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIndex].second + 1);
        if (_windows[windowIndex].lowBoundExpr) {
            requiredIdLow = std::min(requiredIdLow, _windowIdRanges[windowIndex].first);
        }
    }
    freeUntilRow(requiredIdLow - 1);

    // Set out accessors for the current document.
    setOutAccessors(_currId);

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
    for (size_t idx = 0; idx < _partitionSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _partitionSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _forwardSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _forwardSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t windowIdx = 0; windowIdx < _windows.size(); ++windowIdx) {
        const auto& window = _windows[windowIdx];
        if (windowIdx) {
            DebugPrinter::addNewLine(ret);
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, window.windowSlot);
        ret.emplace_back("=");

        ret.emplace_back("lowBound{`");
        if (window.lowBoundExpr) {
            DebugPrinter::addBlocks(ret, window.lowBoundExpr->debugPrint());
        }
        ret.emplace_back("`},");
        ret.emplace_back("highBound{`");
        if (window.highBoundExpr) {
            DebugPrinter::addBlocks(ret, window.highBoundExpr->debugPrint());
        }
        ret.emplace_back("`},");
        ret.emplace_back("init{`");
        if (window.initExpr) {
            DebugPrinter::addBlocks(ret, window.initExpr->debugPrint());
        }
        ret.emplace_back("`},");
        ret.emplace_back("add{`");
        DebugPrinter::addBlocks(ret, window.addExpr->debugPrint());
        ret.emplace_back("`},");
        ret.emplace_back("remove{`");
        if (window.removeExpr) {
            DebugPrinter::addBlocks(ret, window.removeExpr->debugPrint());
        }
        ret.emplace_back("`}");
    }
    ret.emplace_back("`]");

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
    size += size_estimator::estimate(_partitionSlots);
    size += size_estimator::estimate(_forwardSlots);
    size += size_estimator::estimate(_windows);
    return size;
}

}  // namespace mongo::sbe
