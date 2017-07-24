/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/collection_scan.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

#include "mongo/db/client.h"  // XXX-ERH

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* CollectionScan::kStageType = "COLLSCAN";

CollectionScan::CollectionScan(OperationContext* opCtx,
                               const CollectionScanParams& params,
                               WorkingSet* workingSet,
                               const MatchExpression* filter)
    : PlanStage(kStageType, opCtx),
      _workingSet(workingSet),
      _filter(filter),
      _params(params),
      _isDead(false),
      _wsidForFetch(_workingSet->allocate()) {
    // Explain reports the direction of the collection scan.
    _specificStats.direction = params.direction;
}

PlanStage::StageState CollectionScan::doWork(WorkingSetID* out) {
    if (_isDead) {
        Status status(
            ErrorCodes::CappedPositionLost,
            str::stream()
                << "CollectionScan died due to position in capped collection being deleted. "
                << "Last seen record id: "
                << _lastSeenId);
        *out = WorkingSetCommon::allocateStatusMember(_workingSet, status);
        return PlanStage::DEAD;
    }

    if ((0 != _params.maxScan) && (_specificStats.docsTested >= _params.maxScan)) {
        _commonStats.isEOF = true;
    }

    if (_commonStats.isEOF) {
        return PlanStage::IS_EOF;
    }

    boost::optional<Record> record;
    const bool needToMakeCursor = !_cursor;
    try {
        if (needToMakeCursor) {
            const bool forward = _params.direction == CollectionScanParams::FORWARD;

            if (forward && !_params.tailable && _params.collection->ns().isOplog()) {
                // Forward, non-tailable scans from the oplog need to wait until all oplog entries
                // before the read begins to be visible. This isn't needed for reverse scans because
                // we only hide oplog entries from forward scans, and it isn't necessary for tailing
                // cursors because they ignore EOF and will eventually see all writes. Forward,
                // non-tailable scans are the only case where a meaningful EOF will be seen that
                // might not include writes that finished before the read started. This also must be
                // done before we create the cursor as that is when we establish the endpoint for
                // the cursor.
                _params.collection->getRecordStore()->waitForAllEarlierOplogWritesToBeVisible(
                    getOpCtx());
            }

            _cursor = _params.collection->getCursor(getOpCtx(), forward);

            if (!_lastSeenId.isNull()) {
                invariant(_params.tailable);
                // Seek to where we were last time. If it no longer exists, mark us as dead
                // since we want to signal an error rather than silently dropping data from the
                // stream. This is related to the _lastSeenId handling in invalidate. Note that
                // we want to return the record *after* this one since we have already returned
                // this one. This is only possible in the tailing case because that is the only
                // time we'd need to create a cursor after already getting a record out of it.
                if (!_cursor->seekExact(_lastSeenId)) {
                    _isDead = true;
                    Status status(ErrorCodes::CappedPositionLost,
                                  str::stream() << "CollectionScan died due to failure to restore "
                                                << "tailable cursor position. "
                                                << "Last seen record id: "
                                                << _lastSeenId);
                    *out = WorkingSetCommon::allocateStatusMember(_workingSet, status);
                    return PlanStage::DEAD;
                }
            }

            return PlanStage::NEED_TIME;
        }

        if (_lastSeenId.isNull() && !_params.start.isNull()) {
            record = _cursor->seekExact(_params.start);
        } else {
            // See if the record we're about to access is in memory. If not, pass a fetch
            // request up.
            if (auto fetcher = _cursor->fetcherForNext()) {
                // Pass the RecordFetcher up.
                WorkingSetMember* member = _workingSet->get(_wsidForFetch);
                member->setFetcher(fetcher.release());
                *out = _wsidForFetch;
                return PlanStage::NEED_YIELD;
            }

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

    WorkingSetID id = _workingSet->allocate();
    WorkingSetMember* member = _workingSet->get(id);
    member->recordId = record->id;
    member->obj = {getOpCtx()->recoveryUnit()->getSnapshotId(), record->data.releaseToBson()};
    _workingSet->transitionToRecordIdAndObj(id);

    return returnIfMatches(member, id, out);
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
    } else {
        _workingSet->free(memberID);
        return PlanStage::NEED_TIME;
    }
}

bool CollectionScan::isEOF() {
    return _commonStats.isEOF || _isDead;
}

void CollectionScan::doInvalidate(OperationContext* opCtx,
                                  const RecordId& id,
                                  InvalidationType type) {
    // We don't care about mutations since we apply any filters to the result when we (possibly)
    // return it.
    if (INVALIDATION_DELETION != type) {
        return;
    }

    // If we're here, 'id' is being deleted.

    // Deletions can harm the underlying RecordCursor so we must pass them down.
    if (_cursor) {
        _cursor->invalidate(opCtx, id);
    }

    if (_params.tailable && id == _lastSeenId) {
        // This means that deletes have caught up to the reader. We want to error in this case
        // so readers don't miss potentially important data.
        _isDead = true;
    }
}

void CollectionScan::doSaveState() {
    if (_cursor) {
        _cursor->save();
    }
}

void CollectionScan::doRestoreState() {
    if (_cursor) {
        if (!_cursor->restore()) {
            _isDead = true;
        }
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
    if (NULL != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_COLLSCAN);
    ret->specific = make_unique<CollectionScanStats>(_specificStats);
    return ret;
}

const SpecificStats* CollectionScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
