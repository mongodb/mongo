/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/collection_scan.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace {
MONGO_FAIL_POINT_DEFINE(hangCollScanDoWork);
}  // namespace

namespace mongo {

using std::unique_ptr;

namespace {
bool shouldIncludeStartRecord(const CollectionScanParams& params) {
    return params.boundInclusion ==
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
        params.boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeStartRecordOnly;
}

const char* getStageName(const VariantCollectionPtrOrAcquisition& coll,
                         const CollectionScanParams& params) {
    return (!coll.getCollectionPtr()->ns().isOplog() && (params.minRecord || params.maxRecord))
        ? "CLUSTERED_IXSCAN"
        : "COLLSCAN";
}
}  // namespace


CollectionScan::CollectionScan(ExpressionContext* expCtx,
                               VariantCollectionPtrOrAcquisition collection,
                               const CollectionScanParams& params,
                               WorkingSet* workingSet,
                               const MatchExpression* filter)
    : RequiresCollectionStage(getStageName(collection, params), expCtx, collection),
      _workingSet(workingSet),
      _filter((filter && !filter->isTriviallyTrue()) ? filter : nullptr),
      _params(params) {
    const auto& collPtr = collection.getCollectionPtr();
    // Explain reports the direction of the collection scan.
    _specificStats.direction = params.direction;
    _specificStats.minRecord = params.minRecord;
    _specificStats.maxRecord = params.maxRecord;
    _specificStats.tailable = params.tailable;
    if (params.minRecord || params.maxRecord) {
        // The 'minRecord' and 'maxRecord' parameters are used for a special optimization that
        // applies only to forwards scans of the oplog and scans on clustered collections.
        tassert(9049500,
                "Cannot resume using '$_resumeAfter'/ '$_startAt' if 'minRecord' or 'maxRecord' is "
                "used",
                !params.resumeScanPoint);
        if (collPtr->ns().isOplogOrChangeCollection()) {
            invariant(params.direction == CollectionScanParams::FORWARD);
        } else {
            invariant(collPtr->isClustered());
        }
    }

    if (params.boundInclusion !=
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords) {
        // A collection must be clustered if the bounds aren't both included by default.
        tassert(6125000,
                "Only collection scans on clustered collections may specify recordId "
                "BoundInclusion policies",
                collPtr->isClustered());

        if (filter) {
            // The filter is applied after the ScanBoundInclusion is considered.
            LOGV2_DEBUG(6125007,
                        5,
                        "Running a bounded collection scan with a ScanInclusionBound may cause "
                        "the filter to be overriden");
        }
    }

    LOGV2_DEBUG(5400802,
                5,
                "collection scan bounds",
                "min"_attr = (!_params.minRecord) ? "none" : redact(_params.minRecord->toString()),
                "max"_attr = (!_params.maxRecord) ? "none" : redact(_params.maxRecord->toString()));
    tassert(6521000,
            "Expected an oplog or a change collection with 'shouldTrackLatestOplogTimestamp'",
            !_params.shouldTrackLatestOplogTimestamp || collPtr->ns().isOplogOrChangeCollection());

    if (params.assertTsHasNotFallenOff) {
        tassert(6521001,
                "Expected 'shouldTrackLatestOplogTimestamp' with 'assertTsHasNotFallenOff'",
                params.shouldTrackLatestOplogTimestamp);
        tassert(6521002,
                "Expected forward collection scan with 'assertTsHasNotFallenOff'",
                params.direction == CollectionScanParams::FORWARD);
    }

    if (params.resumeScanPoint) {
        // The 'resumeScanPoint' parameter is used for resumable collection scans, which we
        // only support in the forward direction.
        tassert(6521003,
                "Expected forward collection scan with 'resumeScanPoint'",
                params.direction == CollectionScanParams::FORWARD);
    }

    // Set up 'OplogWaitConfig' if we are scanning the oplog.
    if (collPtr && collPtr->ns().isOplog()) {
        _oplogWaitConfig = OplogWaitConfig();
    }
}

namespace {

/**
 * Returns the first document (owned) in the collection using the cursor 'newCursor' assuming that
 * the cursor has not been used and is unpositioned.
 */
BSONObj getFirstEntry(RecoveryUnit& ru, SeekableRecordCursor* newCursor) {
    auto firstRecord = newCursor->next();
    uassert(ErrorCodes::CollectionIsEmpty,
            "Found collection empty when checking that the first record has not rolled over",
            firstRecord);
    auto entry = firstRecord->data.toBson().getOwned();

    // If we use the cursor, unposition it so that it is ready for use by future callers.
    newCursor->saveUnpositioned();
    newCursor->restore(ru);
    return entry;
};

/**
 * Asserts that the timestamp has not already fallen off the oplog or change collection and then
 * returns an unpositioned cursor.
 *
 * Throws OplogQueryMinTsMissing if tsToCheck no longer exists in the oplog.
 * Throws CollectionIsEmpty if the collection has no documents.
 */
std::unique_ptr<SeekableRecordCursor> initCursorAndAssertTsHasNotFallenOff(
    OperationContext* opCtx, const CollectionPtr& coll, Timestamp tsToCheck) {
    auto cursor = coll->getCursor(opCtx);
    const auto firstEntry =
        getFirstEntry(*shard_role_details::getRecoveryUnit(opCtx), cursor.get());
    const repl::OplogEntryParserNonStrict oplogEntryParser{firstEntry};

    // Indicates that 'firstEntry' means initialization of a replica set.
    bool isNewRS{false};
    try {
        // Verify that the timestamp of the first observed oplog entry is earlier than or equal to
        // timestamp that should not have fallen off the oplog.
        if (oplogEntryParser.getOpTime().getTimestamp() <= tsToCheck) {
            return cursor;
        }

        // If the first entry we see in the oplog is the replset initialization, then it doesn't
        // matter if its timestamp is later than the timestamp that should not have fallen off the
        // oplog; no events earlier can have fallen off this oplog.
        // NOTE: A change collection can be created at any moment as such it might not have replset
        // initialization message, as such this case is not fully applicable for the change
        // collection.
        isNewRS = oplogEntryParser.getOpType() == repl::OpTypeEnum::kNoop &&
            oplogEntryParser.getObject().binaryEqual(BSON("msg" << repl::kInitiatingSetMsg));
    } catch (const AssertionException& exception) {
        uasserted(8881102,
                  str::stream() << "Failed to parse the oldest oplog entry" << causedBy(exception));
    }
    uassert(ErrorCodes::OplogQueryMinTsMissing,
            str::stream()
                << "Specified timestamp has already fallen off the oplog for the input timestamp: "
                << tsToCheck << ", first oplog entry: " << firstEntry.toString(),
            isNewRS);
    return cursor;
}
}  // namespace

void CollectionScan::initCursor(OperationContext* opCtx,
                                const CollectionPtr& collPtr,
                                bool forward) {
    if (_params.assertTsHasNotFallenOff) {
        invariant(forward);
        _cursor =
            initCursorAndAssertTsHasNotFallenOff(opCtx, collPtr, *_params.assertTsHasNotFallenOff);

        // We don't need to check this assertion again after we've confirmed the first oplog event.
        _params.assertTsHasNotFallenOff = boost::none;
    } else {
        _cursor = collPtr->getCursor(opCtx, forward);
    }
}

PlanStage::StageState CollectionScan::doWork(WorkingSetID* out) {
    if (MONGO_unlikely(hangCollScanDoWork.shouldFail())) {
        hangCollScanDoWork.pauseWhileSet();
    }

    if (_commonStats.isEOF) {
        _priority.reset();
        return PlanStage::IS_EOF;
    }

    boost::optional<Record> record;
    const bool needToMakeCursor = !_cursor;
    const auto& collPtr = collectionPtr();

    const auto ret = handlePlanStageYield(
        expCtx(),
        "CollectionScan",
        [&] {
            if (needToMakeCursor) {
                const bool forward = _params.direction == CollectionScanParams::FORWARD;
                if (forward && _params.shouldWaitForOplogVisibility) {
                    tassert(9478714, "Must have oplog wait config configured", _oplogWaitConfig);
                    if (_oplogWaitConfig->shouldWaitForOplogVisibility()) {
                        tassert(9478701,
                                "We should only request yield for a tailable oplog scan",
                                !_params.tailable && collPtr->ns().isOplog());

                        // Perform wait during yield. Note that we mark this as having waited before
                        // actually waiting so that we can distinguish waiting for oplog visiblity
                        // from a WriteConflictException when handling this yield.
                        _oplogWaitConfig->setWaitedForOplogVisibility();
                        LOGV2_DEBUG(
                            9478711, 2, "Oplog scan triggering yield to wait for visibility");
                        return NEED_YIELD;
                    }
                }

                try {
                    initCursor(opCtx(), collPtr, forward);
                } catch (const ExceptionFor<ErrorCodes::CollectionIsEmpty>&) {
                    _commonStats.isEOF = true;
                    return PlanStage::IS_EOF;
                }

                if (!_lastSeenId.isNull()) {
                    invariant(_params.tailable);
                    // Seek to where we were last time. If it no longer exists, mark us as dead
                    // since we want to signal an error rather than silently dropping data from the
                    // stream.
                    //
                    // Note that we want to return the record *after* this one since we have already
                    // returned this one. This is possible in the tailing case. Notably, tailing is
                    // the only time we'd need to create a cursor after already getting a record out
                    // of it and updating our _lastSeenId.
                    if (!_cursor->seekExact(_lastSeenId)) {
                        uasserted(ErrorCodes::CappedPositionLost,
                                  str::stream() << "CollectionScan died due to failure to restore "
                                                << "tailable cursor position. "
                                                << "Last seen record id: " << _lastSeenId);
                    }
                }

                if (_params.resumeScanPoint) {
                    invariant(!_params.tailable);
                    invariant(_lastSeenId.isNull());
                    // Seek to where we are trying to resume the scan from. Signal a KeyNotFound
                    // error if the recordId is null.
                    //
                    // Note that we want to return the record *after* this one since we have already
                    // returned this one prior to the resume *unless* we are resuming from a deleted
                    // record id in which case the cursor is already positioned on the next record
                    // id.
                    auto& resumeScanPoint = *_params.resumeScanPoint;
                    auto& recordIdToSeek = resumeScanPoint.recordId;
                    if (recordIdToSeek.isNull()) {
                        uasserted(
                            ErrorCodes::KeyNotFound,
                            "Failed to resume collection scan: cannot resume from null recordId");
                    }

                    // Attempt to seek to the exact recordId. If it doesn't exist and we're using
                    // '$_startAt' (tolerateKeyNotFound = true), we'll use seek() instead to find
                    // the next highest recordId and return. In this case the cursor is already
                    // positioned on the recordId we want to return so we don't need to advance it
                    // again below. If tolerateKeyNotFound = false, we throw.
                    auto testRecord = _cursor->seekExact(recordIdToSeek);
                    if (!testRecord) {
                        if (resumeScanPoint.tolerateKeyNotFound) {
                            record = _cursor->seek(recordIdToSeek,
                                                   SeekableRecordCursor::BoundInclusion::kInclude);
                            return PlanStage::ADVANCED;
                        } else {
                            uasserted(
                                ErrorCodes::KeyNotFound,
                                str::stream()
                                    << "Failed to resume collection scan: the recordId from "
                                    << "which we are attempting to resume no longer exists in "
                                    << "the collection: " << recordIdToSeek);
                        }
                    }
                }

                if (_lastSeenId.isNull() && _params.direction == CollectionScanParams::FORWARD &&
                    _params.minRecord) {
                    // Seek to the start location and return it.
                    record = _cursor->seek(_params.minRecord->recordId(),
                                           shouldIncludeStartRecord(_params)
                                               ? SeekableRecordCursor::BoundInclusion::kInclude
                                               : SeekableRecordCursor::BoundInclusion::kExclude);
                    return PlanStage::ADVANCED;
                } else if (_lastSeenId.isNull() &&
                           _params.direction == CollectionScanParams::BACKWARD &&
                           _params.maxRecord) {
                    // Seek to the start location and return it.
                    record = _cursor->seek(_params.maxRecord->recordId(),
                                           shouldIncludeStartRecord(_params)
                                               ? SeekableRecordCursor::BoundInclusion::kInclude
                                               : SeekableRecordCursor::BoundInclusion::kExclude);
                    return PlanStage::ADVANCED;
                }
            }

            record = _cursor->next();
            return PlanStage::ADVANCED;
        },
        [&] {
            // yieldHandler
            // Leave us in a state to try again next time.
            if (needToMakeCursor)
                _cursor.reset();
            *out = WorkingSet::INVALID_ID;
        });

    if (ret != PlanStage::ADVANCED) {
        return ret;
    }

    if (!record) {
        // We hit EOF. If we are tailable, leave us in a state to pick up where we left off on the
        // next call to work(). Otherwise, the EOF is permanent.
        if (_params.tailable) {
            _cursor.reset();
        } else {
            _lastSeenId = RecordId();
            _commonStats.isEOF = true;
        }

        // For change collections, advance '_latestOplogEntryTimestamp' to the current snapshot
        // timestamp, i.e. the latest available timestamp in the global oplog.
        if (_params.shouldTrackLatestOplogTimestamp && collPtr->ns().isChangeCollection()) {
            setLatestOplogEntryTimestampToReadTimestamp();
        }
        _priority.reset();
        return PlanStage::IS_EOF;
    }

    _lastSeenId = record->id;
    if (_params.shouldTrackLatestOplogTimestamp) {
        setLatestOplogEntryTimestamp(*record);
    }

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = std::move(record->id);
    member->resetDocument(shard_role_details::getRecoveryUnit(opCtx())->getSnapshotId(),
                          record->data.releaseToBson());
    _workingSet->transitionToRecordIdAndObj(id);

    return returnIfMatches(member, id, out);
}

void CollectionScan::setLatestOplogEntryTimestampToReadTimestamp() {
    // Since this method is only ever called when iterating a change collection, the following check
    // effectively disables optime advancement in Serverless, for reasons outlined in SERVER-76288.
    if (collectionPtr()->ns().isChangeCollection()) {
        return;
    }

    const auto readTimestamp =
        shard_role_details::getRecoveryUnit(opCtx())->getPointInTimeReadTimestamp();

    // If we don't have a read timestamp, we take no action here.
    if (!readTimestamp) {
        return;
    }

    // Otherwise, verify that it is equal to or greater than the last recorded timestamp, and
    // advance it accordingly.
    tassert(
        6663000,
        "The read timestamp must always be greater than or equal to the last recorded timestamp",
        *readTimestamp >= _latestOplogEntryTimestamp);
    _latestOplogEntryTimestamp = *readTimestamp;
}

void CollectionScan::setLatestOplogEntryTimestamp(const Record& record) {
    auto tsElem = record.data.toBson()[repl::OpTime::kTimestampFieldName];
    uassert(ErrorCodes::Error(4382100),
            str::stream() << "CollectionScan was asked to track latest operation time, "
                             "but found a result without a valid 'ts' field: "
                          << record.data.toBson().toString(),
            tsElem.type() == BSONType::timestamp);
    LOGV2_DEBUG(550450,
                5,
                "Setting _latestOplogEntryTimestamp to the max of the timestamp of the current "
                "latest oplog entry and the timestamp of the current record",
                "latestOplogEntryTimestamp"_attr = _latestOplogEntryTimestamp,
                "currentRecordTimestamp"_attr = tsElem.timestamp());
    _latestOplogEntryTimestamp = std::max(_latestOplogEntryTimestamp, tsElem.timestamp());
}

BSONObj CollectionScan::getPostBatchResumeToken() const {
    // Return a resume token compatible with resumable initial sync.
    if (_params.requestResumeToken) {
        BSONObjBuilder builder;
        _lastSeenId.serializeToken("$recordId", &builder);
        auto initialSyncId = repl::ReplicationCoordinator::get(opCtx())->getInitialSyncId(opCtx());
        if (initialSyncId) {
            initialSyncId.value().appendToBuilder(&builder, "$initialSyncId");
        }
        return builder.obj();
    }
    // Return a resume token compatible with resharding oplog sync.
    if (_params.shouldTrackLatestOplogTimestamp) {
        return ResumeTokenOplogTimestamp{_latestOplogEntryTimestamp}.toBSON();
    }

    return {};
}

namespace {
bool shouldIncludeEndRecord(const CollectionScanParams& params) {
    return params.boundInclusion ==
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords ||
        params.boundInclusion == CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly;
}

bool pastEndOfRange(const CollectionScanParams& params, const WorkingSetMember& member) {
    if (params.direction == CollectionScanParams::FORWARD) {
        // A forward scan ends with the maxRecord when it is specified.
        if (!params.maxRecord) {
            return false;
        }

        const auto& endRecord = params.maxRecord->recordId();
        return member.recordId > endRecord ||
            (member.recordId == endRecord && !shouldIncludeEndRecord(params));
    } else {
        // A backward scan ends with the minRecord when it is specified.
        if (!params.minRecord) {
            return false;
        }
        const auto& endRecord = params.minRecord->recordId();

        return member.recordId < endRecord ||
            (member.recordId == endRecord && !shouldIncludeEndRecord(params));
    }
}

bool beforeStartOfRange(const CollectionScanParams& params, const WorkingSetMember& member) {
    if (params.direction == CollectionScanParams::FORWARD) {
        // A forward scan begins with the minRecord when it is specified.
        if (!params.minRecord) {
            return false;
        }

        const auto& startRecord = params.minRecord->recordId();
        return member.recordId < startRecord ||
            (member.recordId == startRecord && !shouldIncludeStartRecord(params));
    } else {
        // A backward scan begins with the maxRecord when specified.
        if (!params.maxRecord) {
            return false;
        }
        const auto& startRecord = params.maxRecord->recordId();
        return member.recordId > startRecord ||
            (member.recordId == startRecord && !shouldIncludeStartRecord(params));
    }
}
}  // namespace

PlanStage::StageState CollectionScan::returnIfMatches(WorkingSetMember* member,
                                                      WorkingSetID memberID,
                                                      WorkingSetID* out) {
    ++_specificStats.docsTested;

    // The 'maxRecord' bound is always inclusive, even if the query predicate is
    // an exclusive inequality like $lt. In such cases, we rely on '_filter' to either
    // exclude or include the endpoints as required by the user's query.
    if (pastEndOfRange(_params, *member)) {
        _workingSet->free(memberID);
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    if (!Filter::passes(member, _filter)) {
        _workingSet->free(memberID);
        if (_params.shouldReturnEofOnFilterMismatch) {
            _commonStats.isEOF = true;
            return PlanStage::IS_EOF;
        }
        return PlanStage::NEED_TIME;
    }
    if (_params.stopApplyingFilterAfterFirstMatch) {
        _filter = nullptr;
    }
    *out = memberID;
    return PlanStage::ADVANCED;
}

bool CollectionScan::isEOF() const {
    return _commonStats.isEOF;
}

void CollectionScan::doSaveStateRequiresCollection() {
    if (_cursor) {
        _cursor->save();
    }
}

void CollectionScan::doRestoreStateRequiresCollection() {
    if (_cursor) {
        // If this collection scan serves a read operation on a capped collection, only restore the
        // cursor if it can be repositioned exactly where it was, so that consumers don't silently
        // get 'holes' when scanning capped collections. If this collection scan serves a write
        // operation on a capped collection like a clustered TTL deletion, exempt this operation
        // from the guarantees above.
        const auto tolerateCappedCursorRepositioning = expCtx()->getIsCappedDelete();
        const bool couldRestore = _cursor->restore(*shard_role_details::getRecoveryUnit(opCtx()),
                                                   tolerateCappedCursorRepositioning);
        uassert(ErrorCodes::CappedPositionLost,
                str::stream()
                    << "CollectionScan died due to position in capped collection being deleted. "
                    << "Last seen record id: " << _lastSeenId,
                couldRestore);
    }
}

void CollectionScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();

    _priority.reset();
}

void CollectionScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(opCtx());
}

unique_ptr<PlanStageStats> CollectionScan::getStats() {
    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (nullptr != _filter) {
        _commonStats.filter = _filter->serialize();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_COLLSCAN);
    ret->specific = std::make_unique<CollectionScanStats>(_specificStats);
    return ret;
}

const SpecificStats* CollectionScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
