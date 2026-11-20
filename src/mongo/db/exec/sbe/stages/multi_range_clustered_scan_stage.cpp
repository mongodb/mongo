// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/multi_range_clustered_scan_stage.h"

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <tuple>
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

    if (_parallelChunks) {
        _parallelChunks->reset(_parallelId);
    }

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
    bool participateInTrialRunTracking,
    std::shared_ptr<ParallelScanChunks> parallelChunks,
    size_t parallelId)
    : ScanStageBaseImpl(std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking),
      _rangeList(std::move(rangeList)),
      _parallelChunks(std::move(parallelChunks)),
      _parallelId(parallelId) {}

std::vector<std::unique_ptr<PlanStage>> MultiRangeClusteredScanStage::cloneForParallelExecution(
    CompileCtx& ctx, size_t dop) const {
    std::vector<std::unique_ptr<PlanStage>> clones;
    if (_rangeList.isEmpty()) {
        // Nothing to scan, so nothing to parallelize.
        return clones;
    }
    auto chunks = ParallelScanChunks::make(ctx, _opCtx, _state->collUuid, _state->forward, dop);
    if (!chunks) {
        return clones;
    }
    for (size_t i = 0; i < dop; i++) {
        clones.push_back(std::make_unique<MultiRangeClusteredScanStage>(
            _state,
            /*yieldPolicy=*/nullptr,  // TODO: SERVER-137779 make PlanYieldPolicy thread safe
            _commonStats.nodeId,
            _rangeList,
            participateInTrialRunTracking(),
            chunks,
            i));
    }
    return clones;
}

std::unique_ptr<PlanStage> MultiRangeClusteredScanStage::clone() const {
    return std::make_unique<MultiRangeClusteredScanStage>(_state,
                                                          _yieldPolicy,
                                                          _commonStats.nodeId,
                                                          _rangeList,
                                                          participateInTrialRunTracking(),
                                                          nullptr);
}

boost::optional<Record> MultiRangeClusteredScanStage::seekToRange(size_t rangeId) {
    const auto& range = _rangeList.getRanges()[rangeId];
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
    return _cursor->seek(seekTarget, seekInclusive);
}

boost::optional<Record> MultiRangeClusteredScanStage::getNextInternal() {
    if (MONGO_unlikely(_parallelChunks)) {
        return getNextInternalParallel();
    }
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
            nextRecord = seekToRange(_currentRangeIdx);
        } else if (auto* r = std::get_if<RecordIdRangeList::SeekInRange>(&seekRes)) {
            _currentRangeIdx = r->idx;
            break;
        }
    }

    return nextRecord;
}

boost::optional<Record> MultiRangeClusteredScanStage::getNextInternalParallel() {
    // Whether 'rid' lies at or beyond the end of the chunk being scanned by a parallel scan.
    auto pastChunkEnd = [&](const RecordId& rid) {
        return _state->forward ? rid >= _excludedHigherBound : rid <= _excludedHigherBound;
    };

    // The loop only ever runs more than once for a parallel scan, when the chunk we were scanning
    // ends and we move on to the next one.
    boost::optional<Record> nextRecord;
    while (true) {
        // If this is not the initial seek
        if (_currentRangeIdx != -1ULL) {
            nextRecord = _cursor->next();
        } else {
            auto nRanges = _rangeList.getRanges().size();
            // Note that nRanges > 0 here.
            _currentRangeIdx = _state->forward ? 0 : nRanges - 1;
            auto chunk = _parallelChunks->nextChunk(_parallelId);
            if (!chunk) {
                // Every chunk of the collection has been claimed by some thread.
                return boost::none;
            }
            RecordId includedLowerBound;
            std::tie(includedLowerBound, _excludedHigherBound) = *chunk;
            // Jump to the chunk's lower bound, unless we are already there; the leap-frogging below
            // takes us to the first range that intersects the chunk.
            if (!nextRecord || nextRecord->id != includedLowerBound) {
                nextRecord = _cursor->seek(includedLowerBound,
                                           SeekableRecordCursor::BoundInclusion::kInclude);
            }
        }

        // Leap-frog by seeking over the ranges and the collection
        // in an alternating fashion.
        while (nextRecord && !pastChunkEnd(nextRecord->id)) {
            auto seekRes = _rangeList.seek(nextRecord->id, _currentRangeIdx, _state->forward);

            if (std::get_if<RecordIdRangeList::SeekBeyondAllRanges>(&seekRes)) {
                nextRecord.reset();  // EOF
            } else if (auto* r = std::get_if<RecordIdRangeList::SeekBeforeRange>(&seekRes)) {
                _currentRangeIdx = r->idx;
                nextRecord = seekToRange(_currentRangeIdx);
            } else if (auto* r = std::get_if<RecordIdRangeList::SeekInRange>(&seekRes)) {
                _currentRangeIdx = r->idx;
                break;
            }
        }

        if (!nextRecord || pastChunkEnd(nextRecord->id)) {
            // Done with this chunk; claim the next one and carry on from its lower bound.
            _currentRangeIdx = -1ULL;
            continue;
        }

        return nextRecord;
    }
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
