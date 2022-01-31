/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/column_scan.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"

namespace mongo {
namespace sbe {
ColumnScanStage::ColumnScanStage(UUID collectionUuid,
                                 StringData columnIndexName,
                                 std::vector<value::SlotId> fieldSlots,
                                 std::vector<std::string> paths,
                                 PlanYieldPolicy* yieldPolicy,
                                 PlanNodeId nodeId)
    : PlanStage("columnscan"_sd, yieldPolicy, nodeId),
      _collUuid(collectionUuid),
      _columnIndexName(columnIndexName),
      _fieldSlots(std::move(fieldSlots)),
      _paths(std::move(paths)) {
    invariant(_fieldSlots.size() == _paths.size());
}

std::unique_ptr<PlanStage> ColumnScanStage::clone() const {
    return std::make_unique<ColumnScanStage>(
        _collUuid, _columnIndexName, _fieldSlots, _paths, _yieldPolicy, _commonStats.nodeId);
}

void ColumnScanStage::prepare(CompileCtx& ctx) {
    _outputFields.resize(_fieldSlots.size());

    for (size_t idx = 0; idx < _outputFields.size(); ++idx) {
        auto [it, inserted] = _outputFieldsMap.emplace(_fieldSlots[idx], &_outputFields[idx]);
        uassert(6298601, str::stream() << "duplicate slot: " << _fieldSlots[idx], inserted);
    }
}

value::SlotAccessor* ColumnScanStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outputFieldsMap.find(slot); it != _outputFieldsMap.end()) {
        return it->second;
    }
    return ctx.getAccessor(slot);
}

void ColumnScanStage::doSaveState(bool relinquishCursor) {}

void ColumnScanStage::doRestoreState(bool relinquishCursor) {}

void ColumnScanStage::doDetachFromOperationContext() {}

void ColumnScanStage::doAttachToOperationContext(OperationContext* opCtx) {}

void ColumnScanStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask ColumnScanStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    _tracker = tracker;
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToStreamingStage;
}

void ColumnScanStage::open(bool reOpen) {}

PlanState ColumnScanStage::getNext() {
    return trackPlanState(PlanState::IS_EOF);
}

void ColumnScanStage::close() {}

std::unique_ptr<PlanStageStats> ColumnScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("columnIndexName", _columnIndexName);
        bob.appendNumber("numReads", static_cast<long long>(_specificStats.numReads));

        bob.append("paths", _paths);
        bob.append("outputSlots", _fieldSlots.begin(), _fieldSlots.end());

        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* ColumnScanStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> ColumnScanStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    // Print out output slots.
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _fieldSlots.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _fieldSlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    // Print out paths.
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _paths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        ret.emplace_back(str::stream() << "\"" << _paths[idx] << "\"");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _collUuid.toString());
    ret.emplace_back("`\"");

    ret.emplace_back("@\"`");
    DebugPrinter::addIdentifier(ret, _columnIndexName);
    ret.emplace_back("`\"");

    return ret;
}

size_t ColumnScanStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_fieldSlots);
    size += size_estimator::estimate(_paths);
    size += size_estimator::estimate(_specificStats);
    return size;
}

}  // namespace sbe
}  // namespace mongo
