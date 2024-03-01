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

MONGO_FAIL_POINT_DEFINE(overrideMemoryLimitForSpillForSBEWindowStage);

WindowStage::WindowStage(std::unique_ptr<PlanStage> input,
                         value::SlotVector currSlots,
                         value::SlotVector boundTestingSlots,
                         size_t partitionSlotCount,
                         std::vector<Window> windows,
                         boost::optional<value::SlotId> collatorSlot,
                         bool allowDiskUse,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking)
    : PlanStage("window"_sd, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _currSlots(std::move(currSlots)),
      _boundTestingSlots(std::move(boundTestingSlots)),
      _partitionSlotCount(partitionSlotCount),
      _windows(std::move(windows)),
      _collatorSlot(collatorSlot),
      _allowDiskUse(allowDiskUse) {
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

    _records.reserve(_batchSize);
    _recordBuffers.reserve(_batchSize);
    _recordTimestamps.reserve(_batchSize);
    for (size_t i = 0; i < _batchSize; i++) {
        _recordTimestamps.push_back(Timestamp{});
    }
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
                                         _allowDiskUse,
                                         _commonStats.nodeId,
                                         _participateInTrialRunTracking);
}

void WindowStage::doSaveState(bool relinquishCursor) {
    if (_recordStore) {
        _recordStore->saveState();
    }
}
void WindowStage::doRestoreState(bool relinquishCursor) {
    if (_recordStore) {
        _recordStore->restoreState();
    }
}

size_t WindowStage::getLastRowId() {
    return _lastRowId;
}

size_t WindowStage::getLastSpilledRowId() {
    return _lastRowId - _rows.size();
}

size_t WindowStage::getWindowFrameSize(size_t windowIdx) {
    auto& windowIdRange = _windowIdRanges[windowIdx];
    return (windowIdRange.second + 1) - windowIdRange.first;
}

size_t WindowStage::getMemoryEstimation() {
    size_t rowMemorySize = _windowBufferMemoryEstimator.estimate(_rows.size());
    size_t windowMemorySize = 0;
    for (size_t windowIdx = 0; windowIdx < _windows.size(); ++windowIdx) {
        size_t frameSize = getWindowFrameSize(windowIdx);
        for (size_t exprIdx = 0; exprIdx < _windowInitCodes[windowIdx].size(); ++exprIdx) {
            windowMemorySize +=
                _windowStateMemoryEstimators[windowIdx][exprIdx].estimate(frameSize);
        }
    }
    return rowMemorySize + windowMemorySize;
}

void WindowStage::spill() {
    // Fail if not allowing disk usage.
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for $setWindowFields, but didn't allow external spilling;"
            " pass allowDiskUse:true to opt in",
            _allowDiskUse);

    // Create spilled record storage if not created.
    if (!_recordStore) {
        _recordStore = std::make_unique<SpillingStore>(_opCtx, KeyFormat::Long);
        _specificStats.usedDisk = true;
    }
    _specificStats.spills++;

    auto writeBatch = [&]() {
        auto status = _recordStore->insertRecords(_opCtx, &_records, _recordTimestamps);
        tassert(7870901, "Failed to spill records in the window stage", status.isOK());
        _records.clear();
        _recordBuffers.clear();
    };

    // Spill all in memory rows in batches.
    for (size_t i = 0, id = getLastSpilledRowId() + 1; i < _rows.size(); ++i, ++id) {
        BufBuilder buf;
        _rows[i].serializeForSorter(buf);
        int bufferSize = buf.len();
        auto buffer = buf.release();
        auto recordId = RecordId(id);
        _recordBuffers.push_back(buffer);
        _records.push_back(Record{RecordId(id), RecordData(buffer.get(), bufferSize)});
        _specificStats.spilledRecords++;
        if (_records.size() == _batchSize) {
            writeBatch();
        }
    }

    // Spill the last batch.
    if (_records.size() > 0) {
        writeBatch();
    }
    _specificStats.spilledDataStorageSize = _recordStore->rs()->storageSize(_opCtx);

    // Clear the in memory window buffer.
    _rows.clear();

    // Fail if spilling cannot reduce memory usage below thredshold.
    uassert(7870900,
            "Exceeded memory limit for $setWindowFields, but cannot reduce memory usage by "
            "spilling further.",
            getMemoryEstimation() <= _memoryThreshold);
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
            auto [tag, val] = accessor->getCopyOfValue();
            row.reset(idx++, true, tag, val);
        }
        _rows.push_back(std::move(row));
        _lastRowId++;

        // Remember new partition boundary, the last row must be in the buffer, the previous row
        // might be spilled. Set the bound testing document to the previous row temporarily for
        // partition boundary detection.
        if (_lastRowId > _firstRowId) {
            auto& row = _rows.back();
            setBoundTestingAccessors(_lastRowId - 1);
            for (idx = 0; idx < _partitionSlotCount; idx++) {
                auto [tag, val] = row.getViewOfValue(idx);
                auto [prevTag, prevVal] = _boundTestingAccessors[idx]->getViewOfValue();
                auto [cmpTag, cmpVal] =
                    value::compareValue(tag, val, prevTag, prevVal, _collatorView);
                if (cmpTag != value::TypeTags::NumberInt32 || cmpVal != 0) {
                    _nextPartitionId = _lastRowId;
                    break;
                }
            }
        }

        // Sample window buffer record memory if needed.
        if (_windowBufferMemoryEstimator.shouldSample()) {
            auto memory = size_estimator::estimate(_rows.back());
            _windowBufferMemoryEstimator.sample(memory);
        }

        // Spill if the memory estimation is above threshold.
        if (getMemoryEstimation() > _memoryThreshold) {
            spill();
        }

        return true;
    } else {
        _isEOF = true;
        return false;
    }
}

void WindowStage::freeUntilRow(size_t requiredId) {
    for (size_t id = getLastSpilledRowId() + 1; id < requiredId && _rows.size(); id++) {
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
    _lastRowId = 0;
    _windowBufferMemoryEstimator.reset();
    _recordStore.reset();
    _nextPartitionId = boost::none;
    _isEOF = false;
}

void WindowStage::readSpilledRow(size_t id, value::MaterializedRow& row) {
    invariant(_recordStore);
    auto recordId = RecordId(id);
    RecordData record;
    auto result = _recordStore->findRecord(_opCtx, recordId, &record);
    tassert(7870902, "Failed to find a spilled record in the window stage", result);
    auto buf = BufReader(record.data(), record.size());
    CollatorInterface* collator = nullptr;
    if (_collatorAccessor) {
        auto [tag, val] = _collatorAccessor->getViewOfValue();
        collator = value::getCollatorView(val);
    }
    value::MaterializedRow::deserializeForSorterIntoRow(buf, {collator}, row);
}

void WindowStage::setAccessors(size_t id,
                               const std::vector<std::unique_ptr<value::SwitchAccessor>>& accessors,
                               size_t& bufferedRowIdx,
                               value::MaterializedRow& spilledRow) {
    invariant(id >= _firstRowId && id <= _lastRowId);
    auto lastSpilledRowId = getLastSpilledRowId();
    if (id > lastSpilledRowId) {
        for (auto&& accessor : accessors) {
            accessor->setIndex(0);
        }
        bufferedRowIdx = id - lastSpilledRowId - 1;
    } else {
        for (auto&& accessor : accessors) {
            accessor->setIndex(1);
        }
        readSpilledRow(id, spilledRow);
    }
}

void WindowStage::clearAccessors(
    const std::vector<std::unique_ptr<value::SwitchAccessor>>& accessors,
    value::MaterializedRow& row) {
    for (size_t i = 0; i < accessors.size(); i++) {
        accessors[i]->setIndex(1);
        row.reset(i, false, value::TypeTags::Nothing, 0);
    }
}

void WindowStage::setCurrAccessors(size_t id) {
    setAccessors(id, _outCurrAccessors, _currBufferedRowIdx, _currSpilledRow);
}

void WindowStage::setBoundTestingAccessors(size_t id) {
    setAccessors(id, _boundTestingAccessors, _boundTestingBufferedRowIdx, _boundTestingSpilledRow);
}

void WindowStage::setFrameFirstAccessors(size_t windowIdx, size_t id) {
    setAccessors(id,
                 _outFrameFirstAccessors[windowIdx],
                 _frameFirstBufferedRowIdxes[windowIdx],
                 _frameFirstSpilledRows[windowIdx]);
}

void WindowStage::clearFrameFirstAccessors(size_t windowIdx) {
    clearAccessors(_outFrameFirstAccessors[windowIdx], _frameFirstSpilledRows[windowIdx]);
}

void WindowStage::setFrameLastAccessors(size_t windowIdx, size_t id) {
    setAccessors(id,
                 _outFrameLastAccessors[windowIdx],
                 _frameLastBufferedRowIdxes[windowIdx],
                 _frameLastSpilledRows[windowIdx]);
}

void WindowStage::clearFrameLastAccessors(size_t windowIdx) {
    clearAccessors(_outFrameLastAccessors[windowIdx], _frameLastSpilledRows[windowIdx]);
}

void WindowStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Prepare slot accessors for the current document.
    size_t slotIdx = 0;
    _inCurrAccessors.reserve(_currSlots.size());
    _outCurrAccessors.reserve(_currSlots.size());
    _outCurrBufferAccessors.reserve(_currSlots.size());
    _outCurrSpillAccessors.reserve(_currSlots.size());
    _currSpilledRow.resize(_currSlots.size());
    for (auto slot : _currSlots) {
        _inCurrAccessors.push_back(_children[0]->getAccessor(ctx, slot));
        _outCurrBufferAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _currBufferedRowIdx, slotIdx));
        _outCurrSpillAccessors.push_back(
            std::make_unique<SpilledRowAccessor>(_currSpilledRow, slotIdx++));
        _outCurrAccessors.push_back(
            std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                _outCurrBufferAccessors.back().get(), _outCurrSpillAccessors.back().get()}));
        _outAccessorMap.emplace(slot, _outCurrAccessors.back().get());
    }

    // Prepare slot accessors for the bound testing document.
    slotIdx = 0;
    _boundTestingAccessors.reserve(_boundTestingSlots.size());
    _boundTestingBufferAccessors.reserve(_boundTestingSlots.size());
    _boundTestingSpillAccessors.reserve(_boundTestingSlots.size());
    _boundTestingSpilledRow.resize(_boundTestingSlots.size());
    for (auto slot : _boundTestingSlots) {
        _boundTestingBufferAccessors.push_back(
            std::make_unique<BufferedRowAccessor>(_rows, _boundTestingBufferedRowIdx, slotIdx));
        _boundTestingSpillAccessors.push_back(
            std::make_unique<SpilledRowAccessor>(_boundTestingSpilledRow, slotIdx++));
        _boundTestingAccessors.push_back(std::make_unique<value::SwitchAccessor>(
            std::vector<value::SlotAccessor*>{_boundTestingBufferAccessors.back().get(),
                                              _boundTestingSpillAccessors.back().get()}));
        _boundTestingAccessorMap.emplace(slot, _boundTestingAccessors.back().get());
    }

    // Prepare slot accessors for the frame first/last document of each window, and compile
    // expressions for each window and prepare slot accessors for the window states.
    _outFrameFirstAccessors.reserve(_windows.size());
    _outFrameFirstBufferAccessors.reserve(_windows.size());
    _frameFirstBufferedRowIdxes.reserve(_windows.size());
    _outFrameFirstSpillAccessors.reserve(_windows.size());
    _frameFirstSpilledRows.reserve(_windows.size());
    _outFrameLastAccessors.reserve(_windows.size());
    _outFrameLastBufferAccessors.reserve(_windows.size());
    _frameLastBufferedRowIdxes.reserve(_windows.size());
    _outFrameLastSpillAccessors.reserve(_windows.size());
    _frameLastSpilledRows.reserve(_windows.size());
    _outWindowAccessors.reserve(_windows.size());
    _windowLowBoundCodes.reserve(_windows.size());
    _windowHighBoundCodes.reserve(_windows.size());
    _windowInitCodes.reserve(_windows.size());
    _windowAddCodes.reserve(_windows.size());
    _windowRemoveCodes.reserve(_windows.size());
    _windowStateMemoryEstimators.reserve(_windows.size());
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];

        _outFrameFirstBufferAccessors.push_back(
            std::vector<std::unique_ptr<BufferedRowAccessor>>());
        _frameFirstBufferedRowIdxes.push_back(-1);
        _outFrameFirstSpillAccessors.push_back(std::vector<std::unique_ptr<SpilledRowAccessor>>());
        _frameFirstSpilledRows.push_back(value::MaterializedRow{window.frameFirstSlots.size()});
        _outFrameFirstAccessors.push_back(std::vector<std::unique_ptr<value::SwitchAccessor>>());
        slotIdx = 0;
        for (auto slot : window.frameFirstSlots) {
            _outFrameFirstBufferAccessors[windowIdx].push_back(
                std::make_unique<BufferedRowAccessor>(
                    _rows, _frameFirstBufferedRowIdxes[windowIdx], slotIdx));
            _outFrameFirstSpillAccessors[windowIdx].push_back(
                std::make_unique<SpilledRowAccessor>(_frameFirstSpilledRows[windowIdx], slotIdx++));
            _outFrameFirstAccessors[windowIdx].push_back(
                std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                    _outFrameFirstBufferAccessors[windowIdx].back().get(),
                    _outFrameFirstSpillAccessors[windowIdx].back().get()}));
            _outAccessorMap.emplace(slot, _outFrameFirstAccessors[windowIdx].back().get());
        }

        _outFrameLastBufferAccessors.push_back(std::vector<std::unique_ptr<BufferedRowAccessor>>());
        _frameLastBufferedRowIdxes.push_back(-1);
        _outFrameLastSpillAccessors.push_back(std::vector<std::unique_ptr<SpilledRowAccessor>>());
        _frameLastSpilledRows.push_back(value::MaterializedRow{window.frameLastSlots.size()});
        _outFrameLastAccessors.push_back(std::vector<std::unique_ptr<value::SwitchAccessor>>());
        slotIdx = 0;
        for (auto slot : window.frameLastSlots) {
            _outFrameLastBufferAccessors[windowIdx].push_back(std::make_unique<BufferedRowAccessor>(
                _rows, _frameLastBufferedRowIdxes[windowIdx], slotIdx));
            _outFrameLastSpillAccessors[windowIdx].push_back(
                std::make_unique<SpilledRowAccessor>(_frameLastSpilledRows[windowIdx], slotIdx++));
            _outFrameLastAccessors[windowIdx].push_back(
                std::make_unique<value::SwitchAccessor>(std::vector<value::SlotAccessor*>{
                    _outFrameLastBufferAccessors[windowIdx].back().get(),
                    _outFrameLastSpillAccessors[windowIdx].back().get()}));
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

        std::vector<WindowStateMemoryEstimator> estimators;
        estimators.resize(initExprsSize);
        _windowStateMemoryEstimators.push_back(std::move(estimators));
    }

    // Prepare slot accessor for the collator.
    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(7870800,
                "collator accessor should exist if collator slot provided to WindowStage",
                _collatorAccessor != nullptr);
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

void WindowStage::setPartition(int id) {
    _windowIdRanges.clear();
    _windowIdRanges.resize(_windows.size(), std::make_pair(id, id - 1));
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& windowAccessors = _outWindowAccessors[windowIdx];
        auto& windowInitCodes = _windowInitCodes[windowIdx];
        auto& memoryEstimators = _windowStateMemoryEstimators[windowIdx];

        for (size_t exprIdx = 0; exprIdx < windowInitCodes.size(); ++exprIdx) {
            if (windowInitCodes[exprIdx]) {
                auto [owned, tag, val] = _bytecode.run(windowInitCodes[exprIdx].get());
                windowAccessors[exprIdx]->reset(owned, tag, val);
            } else {
                windowAccessors[exprIdx]->reset();
            }
            memoryEstimators[exprIdx].reset();
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
    freeRows();
    setPartition(1);
}

PlanState WindowStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

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
        setPartition(_currId);
    }

    // Accumulate all window states.
    for (size_t windowIdx = 0; windowIdx < _windows.size(); windowIdx++) {
        auto& window = _windows[windowIdx];
        auto& idRange = _windowIdRanges[windowIdx];
        auto& windowAccessors = _outWindowAccessors[windowIdx];
        auto& windowAddCodes = _windowAddCodes[windowIdx];
        auto& windowRemoveCodes = _windowRemoveCodes[windowIdx];
        auto& windowMemoryEstimators = _windowStateMemoryEstimators[windowIdx];

        // Add documents into window.
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
                for (size_t exprIdx = 0; exprIdx < windowAddCodes.size(); ++exprIdx) {
                    if (windowAddCodes[exprIdx]) {
                        auto [owned, tag, val] = _bytecode.run(windowAddCodes[exprIdx].get());
                        windowAccessors[exprIdx]->reset(owned, tag, val);
                    }
                }
                idRange.second = id;

                // Sample window state memory if needed.
                auto frameSize = getWindowFrameSize(windowIdx);
                for (size_t exprIdx = 0; exprIdx < windowAddCodes.size(); ++exprIdx) {
                    auto& memoryEstimator = windowMemoryEstimators[exprIdx];
                    if (memoryEstimator.shouldSample(frameSize)) {
                        auto [tag, value] = windowAccessors[exprIdx]->getViewOfValue();
                        auto memory = size_estimator::estimate(tag, value);
                        memoryEstimator.sample(frameSize, memory);
                    }
                }
            } else {
                break;
            }
        }

        // Remove documents from window if it's not unbounded.
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
                    for (size_t exprIdx = 0; exprIdx < windowRemoveCodes.size(); ++exprIdx) {
                        if (windowRemoveCodes[exprIdx]) {
                            auto [owned, tag, val] =
                                _bytecode.run(windowRemoveCodes[exprIdx].get());
                            windowAccessors[exprIdx]->reset(owned, tag, val);
                        }
                    }
                    idRange.first = id + 1;
                } else {
                    break;
                }
            }
        }

        // Spill if the memory estimation is above threshold, or the failpoint condition is met.
        bool shouldSpill = getMemoryEstimation() > _memoryThreshold;
        overrideMemoryLimitForSpillForSBEWindowStage.execute([&](const BSONObj& data) {
            _failPointSpillCounter++;
            if (_failPointSpillCounter > data["spillCounter"].numberInt()) {
                shouldSpill = true;
            }
        });
        if (shouldSpill) {
            spill();
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

    if (includeDebugInfo) {
        DebugPrinter printer;
        BSONObjBuilder bob;
        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk);
        bob.appendNumber("spills", _specificStats.spills);
        bob.appendNumber("spilledRecords", _specificStats.spilledRecords);
        bob.appendNumber("spilledDataStorageSize", _specificStats.spilledDataStorageSize);

        ret->debugInfo = bob.obj();
    }

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
