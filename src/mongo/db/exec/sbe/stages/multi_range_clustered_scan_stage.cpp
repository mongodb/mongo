/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/multi_range_clustered_scan_stage.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <utility>

namespace mongo {
namespace sbe {

void MultiRangeClusteredScanStage::scanResetState(bool reOpen) {
    const auto& ranges = _rangeList.getRanges();
    const bool willSeekToStart = !ranges.empty() &&
        (_state->forward ? ranges.front().getMin().has_value()
                         : ranges.back().getMax().has_value());

    if (!ranges.empty() && (!reOpen || !willSeekToStart)) {
        _cursor = _coll->getCollectionPtr()->getCursor(_opCtx, _state->forward);
    }

    _currentRangeIdx = -1ULL;

    // An empty range list (∅) means no records can match — start in the past-end state so
    // getNext() returns EOF immediately without touching the cursor.
    _havePassedScanEndRecordId = ranges.empty();
}

void MultiRangeClusteredScanStage::getNextHangFailPoint() {
    if (MONGO_unlikely(hangScanGetNext.shouldFail())) {
        hangScanGetNext.pauseWhileSet();
    }
}

MultiRangeClusteredScanStage::MultiRangeClusteredScanStage(
    UUID collUuid,
    DatabaseName dbName,
    boost::optional<value::SlotId> recordSlot,
    boost::optional<value::SlotId> recordIdSlot,
    boost::optional<value::SlotId> snapshotIdSlot,
    boost::optional<value::SlotId> indexIdentSlot,
    boost::optional<value::SlotId> indexKeySlot,
    boost::optional<value::SlotId> indexKeyPatternSlot,
    std::vector<std::string> scanFieldNames,
    value::SlotVector scanFieldSlots,
    RecordIdRangeList rangeList,
    bool forward,
    PlanYieldPolicySBE* yieldPolicy,
    PlanNodeId nodeId,
    ScanOpenCallback scanOpenCallback,
    bool participateInTrialRunTracking)
    : ScanStageBaseImpl(collUuid,
                        dbName,
                        recordSlot,
                        recordIdSlot,
                        snapshotIdSlot,
                        indexIdentSlot,
                        indexKeySlot,
                        indexKeyPatternSlot,
                        scanFieldNames,
                        scanFieldSlots,
                        yieldPolicy,
                        nodeId,
                        scanOpenCallback,
                        forward,
                        participateInTrialRunTracking),
      _rangeList(std::move(rangeList)) {}

MultiRangeClusteredScanStage::MultiRangeClusteredScanStage(
    std::shared_ptr<ScanStageBaseState> state,
    PlanYieldPolicySBE* yieldPolicy,
    PlanNodeId nodeId,
    RecordIdRangeList rangeList,
    bool participateInTrialRunTracking)
    : ScanStageBaseImpl(std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking),
      _rangeList(std::move(rangeList)) {}

std::unique_ptr<PlanStage> MultiRangeClusteredScanStage::clone() const {
    return std::make_unique<MultiRangeClusteredScanStage>(
        _state, _yieldPolicy, _commonStats.nodeId, _rangeList, participateInTrialRunTracking());
}

boost::optional<Record> MultiRangeClusteredScanStage::getNextInternal() {
    boost::optional<Record> nextRecord;
    // If this is not the initial seek
    if (_currentRangeIdx != -1ULL) {
        nextRecord = _cursor->next();
    } else {
        auto nRanges = _rangeList.getRanges().size();
        // Note that nRanges > 0 here.
        _currentRangeIdx = _state->forward ? 0 : nRanges - 1;
        const auto& startRange =
            _state->forward ? _rangeList.getRanges().front() : _rangeList.getRanges().back();
        const auto seekParams = startRange.makeSeekParams(_state->forward);
        if (seekParams) {
            const auto [seekTarget, seekInclusive] = *seekParams;
            nextRecord = _cursor->seek(seekTarget, seekInclusive);
        } else {
            nextRecord = _cursor->next();
        }
    }

    // Leap-frog by seeking over the ranges and the collection
    // in an alternating fashion.
    while (nextRecord) {
        auto seekRes = _rangeList.seek(nextRecord->id, _currentRangeIdx, _state->forward);

        if (std::get_if<RecordIdRangeList::SeekBeyondAllRanges>(&seekRes)) {
            return boost::none;  // EOF
        } else if (auto* r = std::get_if<RecordIdRangeList::SeekBeforeRange>(&seekRes)) {
            _currentRangeIdx = r->idx;
            const auto& range = _rangeList.getRanges()[_currentRangeIdx];
            const auto seekParams = range.makeSeekParams(_state->forward);
            // This is due to the invariant of the RecordIdRangeList
            // that only the outer bounds can be absent.
            // So it always holds that the beginning of the "next" range must be there.
            tassert(12591204, "Expected seekParams.", seekParams);
            // The leap-frog loop may iterate arbitrarily many times when the record stream and
            // the range list never intersect (e.g. even ids vs. odd ranges), so we must check
            // for interrupts and let the operation yield between cursor seeks.
            handleInterruptAndSlotAccess();
            const auto [seekTarget, seekInclusive] = *seekParams;
            nextRecord = _cursor->seek(seekTarget, seekInclusive);

        } else if (auto* r = std::get_if<RecordIdRangeList::SeekInRange>(&seekRes)) {
            _currentRangeIdx = r->idx;
            break;
        }
    }

    return nextRecord;
}

void MultiRangeClusteredScanStage::close() {
    closeShared();
    _cursor.reset();
}

void MultiRangeClusteredScanStage::prepare(CompileCtx& ctx) {
    prepareShared(ctx);
}

std::unique_ptr<PlanStageStats> MultiRangeClusteredScanStage::getStats(
    bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        getStatsShared(bob);
        ret->debugInfo = bob.obj();
    }
    return ret;
}

void MultiRangeClusteredScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                                DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addIdentifier(ret, "rangeList");
    ret.emplace_back("=");
    ret.emplace_back(_rangeList.toBSONArray().toString());
    debugPrintShared(ret);
    ret.emplace_back(_state->forward ? "forward" : "reverse");
}

}  // namespace sbe
}  // namespace mongo
