// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/generic_scan.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstring>

#include <boost/optional/optional.hpp>

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangGenericScanGetNext);
}  // namespace

namespace sbe {
GenericScanStage::GenericScanStage(UUID collUuid,
                                   DatabaseName dbName,
                                   boost::optional<value::SlotId> recordSlot,
                                   boost::optional<value::SlotId> recordIdSlot,
                                   boost::optional<value::SlotId> snapshotIdSlot,
                                   boost::optional<value::SlotId> indexIdentSlot,
                                   boost::optional<value::SlotId> indexKeySlot,
                                   boost::optional<value::SlotId> indexKeyPatternSlot,
                                   std::vector<std::string> scanFieldNames,
                                   value::SlotVector scanFieldSlots,
                                   bool forward,
                                   PlanYieldPolicySBE* yieldPolicy,
                                   PlanNodeId nodeId,
                                   ScanOpenCallback scanOpenCallback,
                                   // Optional arguments:
                                   bool participateInTrialRunTracking)
    : ScanStageBaseImpl<GenericScanStage>(collUuid,
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
                                          // Optional arguments:
                                          participateInTrialRunTracking) {}

/**
 * Constructor for clone(). Copies '_state' shared_ptr.
 */
GenericScanStage::GenericScanStage(std::shared_ptr<ScanStageBaseState> state,
                                   PlanYieldPolicySBE* yieldPolicy,
                                   PlanNodeId nodeId,
                                   bool participateInTrialRunTracking,
                                   std::shared_ptr<ParallelScanChunks> parallelChunks,
                                   size_t parallelId)
    : ScanStageBaseImpl(std::move(state), yieldPolicy, nodeId, participateInTrialRunTracking),
      _parallelChunks(std::move(parallelChunks)),
      _parallelId(parallelId) {}  // ScanStageBaseImpl constructor for clone()

std::vector<std::unique_ptr<PlanStage>> GenericScanStage::cloneForParallelExecution(
    CompileCtx& ctx, size_t dop) const {
    std::vector<std::unique_ptr<PlanStage>> clones;
    auto chunks = ParallelScanChunks::make(ctx, _opCtx, _state->collUuid, _state->forward, dop);
    if (!chunks) {
        return clones;
    }
    for (size_t i = 0; i < dop; i++) {
        clones.push_back(std::make_unique<GenericScanStage>(
            _state,
            /*yieldPolicy=*/nullptr,  // TODO: SERVER-137779 make PlanYieldPolicy thread safe
            _commonStats.nodeId,
            participateInTrialRunTracking(),
            chunks,
            i));
    }
    return clones;
}

std::unique_ptr<PlanStage> GenericScanStage::clone() const {
    return std::make_unique<GenericScanStage>(
        _state, _yieldPolicy, _commonStats.nodeId, participateInTrialRunTracking(), nullptr);
}

boost::optional<Record> GenericScanStage::getNextInternalParallel() {
    // The loop only runs more than once when the chunk we were scanning ends and we move on to the
    // next one.
    boost::optional<Record> nextRecord;
    while (true) {
        if (!_needNextChunk) {
            nextRecord = _cursor->next();
        } else {
            _needNextChunk = false;
            auto chunk = _parallelChunks->nextChunk(_parallelId);
            if (!chunk) {
                // Every chunk of the collection has been claimed by some thread.
                return boost::none;
            }
            RecordId includedLowerBound;
            std::tie(includedLowerBound, _excludedHigherBound) = *chunk;
            // Check if we just ended the chunk coming right before this one, in this case
            // we are already on the correct record and we can avoid the seek operation.
            if (!nextRecord || nextRecord->id != includedLowerBound) {
                nextRecord = _cursor->seek(includedLowerBound,
                                           SeekableRecordCursor::BoundInclusion::kInclude);
            }
        }

        if (!nextRecord || (_state->forward && nextRecord->id >= _excludedHigherBound) ||
            (!_state->forward && nextRecord->id <= _excludedHigherBound)) {
            // Done with this chunk; claim the next one and carry on from its lower bound.
            _needNextChunk = true;
            continue;
        }

        return nextRecord;
    }
}

std::unique_ptr<PlanStageStats> GenericScanStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<ScanStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        getStatsShared(bob);
        ret->debugInfo = bob.obj();
    }
    return ret;
}

void GenericScanStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                    DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back("generic");
    debugPrintShared(ret);
}

void GenericScanStage::getNextHangFailPoint() {
    if (MONGO_unlikely(hangGenericScanGetNext.shouldFail())) {
        hangGenericScanGetNext.pauseWhileSet();
    }
}
}  // namespace sbe
}  // namespace mongo
