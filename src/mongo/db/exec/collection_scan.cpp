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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/collection_scan.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

#include "mongo/db/client.h"  // XXX-ERH

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* CollectionScan::kStageType = "COLLSCAN";

CollectionScan::CollectionScan(OperationContext* opCtx,
                               const Collection* collection,
                               const CollectionScanParams& params,
                               WorkingSet* workingSet,
                               const MatchExpression* filter)
    : RequiresCollectionStage(kStageType, opCtx, collection),
      _workingSet(workingSet),
      _filter(filter),
      _params(params) {
    // Explain reports the direction of the collection scan.
    _specificStats.direction = params.direction;
    _specificStats.minTs = params.minTs;
    _specificStats.maxTs = params.maxTs;
    _specificStats.tailable = params.tailable;
    if (params.minTs || params.maxTs) {
        // The 'minTs' and 'maxTs' parameters are used for a special optimization that
        // applies only to forwards scans of the oplog.
        invariant(params.direction == CollectionScanParams::FORWARD);
        invariant(collection->ns().isOplog());
    }
    invariant(!_params.shouldTrackLatestOplogTimestamp || collection->ns().isOplog());

    // Set early stop condition.
    if (params.maxTs) {
        _endConditionBSON = BSON("$gte"_sd << *(params.maxTs));
        _endCondition = std::make_unique<GTEMatchExpression>(repl::OpTime::kTimestampFieldName,
                                                             _endConditionBSON.firstElement());
    }
}

PlanStage::StageState CollectionScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF) {
        return PlanStage::IS_EOF;
    }

    boost::optional<Record> record;
    const bool needToMakeCursor = !_cursor;
    try {
        if (needToMakeCursor) {
            const bool forward = _params.direction == CollectionScanParams::FORWARD;

            if (forward && _params.shouldWaitForOplogVisibility) {
                // Forward, non-tailable scans from the oplog need to wait until all oplog entries
                // before the read begins to be visible. This isn't needed for reverse scans because
                // we only hide oplog entries from forward scans, and it isn't necessary for tailing
                // cursors because they ignore EOF and will eventually see all writes. Forward,
                // non-tailable scans are the only case where a meaningful EOF will be seen that
                // might not include writes that finished before the read started. This also must be
                // done before we create the cursor as that is when we establish the endpoint for
                // the cursor. Also call abandonSnapshot to make sure that we are using a fresh
                // storage engine snapshot while waiting. Otherwise, we will end up reading from the
                // snapshot where the oplog entries are not yet visible even after the wait.
                invariant(!_params.tailable && collection()->ns().isOplog());

                getOpCtx()->recoveryUnit()->abandonSnapshot();
                collection()->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(getOpCtx());
            }

            _cursor = collection()->getCursor(getOpCtx(), forward);

            if (!_lastSeenId.isNull()) {
                invariant(_params.tailable);
                // Seek to where we were last time. If it no longer exists, mark us as dead since we
                // want to signal an error rather than silently dropping data from the stream.
                //
                // Note that we want to return the record *after* this one since we have already
                // returned this one. This is only possible in the tailing case because that is the
                // only time we'd need to create a cursor after already getting a record out of it.
                if (!_cursor->seekExact(_lastSeenId)) {
                    Status status(ErrorCodes::CappedPositionLost,
                                  str::stream() << "CollectionScan died due to failure to restore "
                                                << "tailable cursor position. "
                                                << "Last seen record id: " << _lastSeenId);
                    *out = WorkingSetCommon::allocateStatusMember(_workingSet, status);
                    return PlanStage::FAILURE;
                }
            }

            return PlanStage::NEED_TIME;
        }

        if (_lastSeenId.isNull() && _params.minTs) {
            // See if the RecordStore supports the oplogStartHack.
            StatusWith<RecordId> goal = oploghack::keyForOptime(*_params.minTs);
            if (goal.isOK()) {
                boost::optional<RecordId> startLoc =
                    collection()->getRecordStore()->oplogStartHack(getOpCtx(), goal.getValue());
                if (startLoc && !startLoc->isNull()) {
                    LOG(3) << "Using direct oplog seek";
                    record = _cursor->seekExact(*startLoc);
                }
            }
        }

        if (!record) {
            record = _cursor->next();
        }
    } catch (const WriteConflictException&) {
        // Leave us in a state to try again next time.
        if (needToMakeCursor)
            _cursor.reset();
        *out = WorkingSet::INVALID_ID;
        return PlanStage::NEED_YIELD;
    }

    if (!record) {
        // We just hit EOF. If we are tailable and have already returned data, leave us in a
        // state to pick up where we left off on the next call to work(). Otherwise EOF is
        // permanent.
        if (_params.tailable && !_lastSeenId.isNull()) {
            _cursor.reset();
        } else {
            _commonStats.isEOF = true;
        }

        return PlanStage::IS_EOF;
    }

    _lastSeenId = record->id;
    if (_params.shouldTrackLatestOplogTimestamp) {
        auto status = setLatestOplogEntryTimestamp(*record);
        if (!status.isOK()) {
            *out = WorkingSetCommon::allocateStatusMember(_workingSet, status);
            return PlanStage::FAILURE;
        }
    }

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = record->id;
    member->resetDocument(getOpCtx()->recoveryUnit()->getSnapshotId(),
                          record->data.releaseToBson());
    _workingSet->transitionToRecordIdAndObj(id);

    return returnIfMatches(member, id, out);
}

Status CollectionScan::setLatestOplogEntryTimestamp(const Record& record) {
    auto tsElem = record.data.toBson()[repl::OpTime::kTimestampFieldName];
    if (tsElem.type() != BSONType::bsonTimestamp) {
        Status status(ErrorCodes::InternalError,
                      str::stream() << "CollectionScan was asked to track latest operation time, "
                                       "but found a result without a valid 'ts' field: "
                                    << record.data.toBson().toString());
        return status;
    }
    _latestOplogEntryTimestamp = std::max(_latestOplogEntryTimestamp, tsElem.timestamp());
    return Status::OK();
}

PlanStage::StageState CollectionScan::returnIfMatches(WorkingSetMember* member,
                                                      WorkingSetID memberID,
                                                      WorkingSetID* out) {
    ++_specificStats.docsTested;
    if (Filter::passes(member, _filter)) {
        if (_params.stopApplyingFilterAfterFirstMatch) {
            _filter = nullptr;
        }
        *out = memberID;
        return PlanStage::ADVANCED;
    } else if (_endCondition && Filter::passes(member, _endCondition.get())) {
        _workingSet->free(memberID);
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    } else {
        _workingSet->free(memberID);
        return PlanStage::NEED_TIME;
    }
}

bool CollectionScan::isEOF() {
    return _commonStats.isEOF;
}

void CollectionScan::doSaveStateRequiresCollection() {
    if (_cursor) {
        _cursor->save();
    }
}

void CollectionScan::doRestoreStateRequiresCollection() {
    if (_cursor) {
        const bool couldRestore = _cursor->restore();
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
}

void CollectionScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(getOpCtx());
}

unique_ptr<PlanStageStats> CollectionScan::getStats() {
    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (nullptr != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_COLLSCAN);
    ret->specific = std::make_unique<CollectionScanStats>(_specificStats);
    return ret;
}

const SpecificStats* CollectionScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
