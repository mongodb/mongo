// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/multi_range_clustered_scan.h"

#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

#include <memory>
#include <optional>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

extern mongo::FailPoint hangCollScanDoWork;

namespace mongo {

MultiRangeClusteredScan::MultiRangeClusteredScan(ExpressionContext* expCtx,
                                                 CollectionAcquisition collection,
                                                 const MultiRangeClusteredScanParams& params,
                                                 WorkingSet* workingSet,
                                                 const MatchExpression* filter)
    : RequiresCollectionStage("CLUSTERED_IXSCAN", expCtx, collection),
      _workingSet(workingSet),
      _filter((filter && !filter->isTriviallyTrue()) ? filter : nullptr),
      _params(params) {
    const auto& collPtr = collection.getCollectionPtr();
    tassert(12591200,
            "MultiRangeClusteredScan requires a clustered collection",
            collPtr->isClustered());
    tassert(
        12591201, "MultiRangeClusteredScan cannot be used on the oplog", !collPtr->ns().isOplog());

    _specificStats.direction = params.direction;
    _specificStats.rangeList = params.rangeList;

    // Initialize range index: forward scans start at index 0, backward at the last range.
    if (!params.rangeList.isEmpty()) {
        _currentRangeIdx = (params.direction == CollectionScanParams::FORWARD)
            ? 0
            : params.rangeList.getRanges().size() - 1;
    }

    LOGV2_DEBUG(12591203,
                5,
                "MultiRangeClusteredScan bounds",
                "rangeList"_attr = BSONArray(redact(params.rangeList.toBSONArray())));
}

PlanStage::StageState MultiRangeClusteredScan::doWork(WorkingSetID* out) {
    if (MONGO_unlikely(hangCollScanDoWork.shouldFail())) {
        hangCollScanDoWork.pauseWhileSet();
    }

    if (_commonStats.isEOF) {
        return PlanStage::IS_EOF;
    }

    // An empty range list (∅) means no records can match; return EOF immediately without seeking.
    if (_params.rangeList.isEmpty()) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    boost::optional<Record> record;
    const bool needToMakeCursor = !_cursor;
    const auto& collPtr = collectionPtr();

    const auto ret = handlePlanStageYield(
        expCtx(),
        "MultiRangeClusteredCollectionScan",
        [&] {
            if (needToMakeCursor) {
                // First doWork call. Create a new cursor & do an initial seek if needed.
                const bool forward = _params.direction == CollectionScanParams::FORWARD;
                _cursor = collPtr->getCursor(opCtx(), forward);

                const auto outerBounds = _params.rangeList.outerBounds();
                const auto seekParams =
                    outerBounds.makeSeekParams(_params.direction == CollectionScanParams::FORWARD);
                if (seekParams) {
                    const auto [seekTarget, seekInclusive] = *seekParams;
                    record = _cursor->seek(seekTarget, seekInclusive);
                    ++_specificStats.seeks;
                    return PlanStage::ADVANCED;
                }
            } else if (_pendingSeek) {
                // We previously determined that we need to seek to the start of the next range.
                const auto& range = _params.rangeList.getRanges()[_currentRangeIdx];
                const auto seekParams =
                    range.makeSeekParams(_params.direction == CollectionScanParams::FORWARD);
                // By the invariant of RecordIdRangeList, only the outer bounds can be absent,
                // so the beginning of the "next" range is always present.
                tassert(99745620, "Expected seekParams", seekParams);
                const auto [seekTarget, seekInclusive] = *seekParams;
                record = _cursor->seek(seekTarget, seekInclusive);
                // Clear `_pendingSeek` only after the seek successfully returns. If seek()
                // throws (e.g. WriteConflictException), `handlePlanStageYield` catches it and
                // returns NEED_YIELD; we need `_pendingSeek` to still be true so that the next
                // doWork() retries the seek instead of falling through to _cursor->next() and
                // overshooting into the inter-range gap.
                _pendingSeek = false;
                ++_specificStats.seeks;
                return PlanStage::ADVANCED;
            }

            record = _cursor->next();
            return PlanStage::ADVANCED;
        },
        [&] {
            // yieldHandler: leave us in a state to try again next time.
            if (needToMakeCursor)
                _cursor.reset();
            *out = WorkingSet::INVALID_ID;
        });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (!record) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    ++_specificStats.docsTested;

    // Seek for the returned RecordId among the ranges
    const bool forward = (_params.direction == CollectionScanParams::FORWARD);
    auto seekRes = _params.rangeList.seek(record->id, _currentRangeIdx, forward);

    auto earlyReturn = std::visit(
        OverloadedVisitor{
            [&](const RecordIdRangeList::SeekBeyondAllRanges&) -> std::optional<StageState> {
                _commonStats.isEOF = true;
                return IS_EOF;
            },
            [&](const RecordIdRangeList::SeekBeforeRange& r) -> std::optional<StageState> {
                _currentRangeIdx = r.idx;
                _pendingSeek = true;
                return NEED_TIME;
            },
            [&](const RecordIdRangeList::SeekInRange& r) -> std::optional<StageState> {
                _currentRangeIdx = r.idx;
                return std::nullopt;
            },
        },
        seekRes);
    if (earlyReturn) {
        return *earlyReturn;
    }

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = std::move(record->id);
    member->resetDocument(shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId(),
                          record->data.releaseToBson());
    _workingSet->transitionToRecordIdAndObj(id);

    return returnIfMatches(member, id, out);
}

PlanStage::StageState MultiRangeClusteredScan::returnIfMatches(WorkingSetMember* member,
                                                               WorkingSetID memberID,
                                                               WorkingSetID* out) {
    if (!Filter::passes(member, _filter)) {
        _workingSet->free(memberID);
        return PlanStage::NEED_TIME;
    }
    *out = memberID;
    return PlanStage::ADVANCED;
}

bool MultiRangeClusteredScan::isEOF() const {
    return _commonStats.isEOF;
}

void MultiRangeClusteredScan::doSaveStateRequiresCollection() {
    if (_cursor) {
        _cursor->save();
    }
}

void MultiRangeClusteredScan::doRestoreStateRequiresCollection() {
    if (_cursor) {
        _cursor->restore(*shard_role_details::getRecoveryUnit(opCtx()));
    }
}

void MultiRangeClusteredScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void MultiRangeClusteredScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

std::unique_ptr<PlanStageStats> MultiRangeClusteredScan::getStats() {
    if (nullptr != _filter) {
        _commonStats.filter = _filter->serialize();
    }
    auto ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_COLLSCAN);
    ret->specific = std::make_unique<CollectionScanStats>(_specificStats);
    return ret;
}

const SpecificStats* MultiRangeClusteredScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
