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

#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

#include "mongo/db/client.h" // XXX-ERH

namespace mongo {

    // static
    const char* CollectionScan::kStageType = "COLLSCAN";

    CollectionScan::CollectionScan(OperationContext* txn,
                                   const CollectionScanParams& params,
                                   WorkingSet* workingSet,
                                   const MatchExpression* filter)
        : _txn(txn),
          _workingSet(workingSet),
          _filter(filter),
          _params(params),
          _isDead(false),
          _wsidForFetch(_workingSet->allocate()),
          _commonStats(kStageType) {
        // Explain reports the direction of the collection scan.
        _specificStats.direction = params.direction;

        // We pre-allocate a WSM and use it to pass up fetch requests. This should never be used
        // for anything other than passing up NEED_FETCH. We use the loc and unowned obj state, but
        // the loc isn't really pointing at any obj. The obj field of the WSM should never be used.
        WorkingSetMember* member = _workingSet->get(_wsidForFetch);
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
    }

    PlanStage::StageState CollectionScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (_isDead) { return PlanStage::DEAD; }

        // Do some init if we haven't already.
        if (NULL == _iter) {
            if ( _params.collection == NULL ) {
                _isDead = true;
                return PlanStage::DEAD;
            }

            if (_lastSeenLoc.isNull()) {
                _iter.reset( _params.collection->getIterator( _txn,
                                                              _params.start,
                                                              _params.direction ) );
            }
            else {
                invariant(_params.tailable);

                _iter.reset( _params.collection->getIterator( _txn,
                                                              _lastSeenLoc,
                                                              _params.direction ) );

                // Advance _iter past where we were last time. If it returns something else, mark us
                // as dead since we want to signal an error rather than silently dropping data from
                // the stream. This is related to the _lastSeenLock handling in invalidate.
                if (_iter->getNext() != _lastSeenLoc) {
                    _isDead = true;
                    return PlanStage::DEAD;
                }
            }

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }

        // Should we try getNext() on the underlying _iter?
        if (isEOF())
            return PlanStage::IS_EOF;

        // See if the record we're about to access is in memory. If not, pass a fetch request up.
        // Note that curr() returns the same thing as getNext() will, except without advancing the
        // iterator or touching the DiskLoc. This means that we can use curr() to check whether we
        // need to fetch on the DiskLoc prior to touching it with getNext().
        DiskLoc curr = _iter->curr();
        if (!curr.isNull()) {
            std::auto_ptr<RecordFetcher> fetcher(
                _params.collection->documentNeedsFetch(_txn, curr));
            if (NULL != fetcher.get()) {
                WorkingSetMember* member = _workingSet->get(_wsidForFetch);
                member->loc = curr;
                // Pass the RecordFetcher off to the WSM.
                member->setFetcher(fetcher.release());
                *out = _wsidForFetch;
                _commonStats.needFetch++;
                return NEED_FETCH;
            }
        }

        // What we'll return to the user.
        DiskLoc nextLoc;

        // See if _iter gives us anything new.
        nextLoc = _iter->getNext();
        if (nextLoc.isNull()) {
            if (_params.tailable)
                _iter.reset(); // pick up where we left off on the next call to work()
            return PlanStage::IS_EOF;
        }

        _lastSeenLoc = nextLoc;

        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = nextLoc;
        member->obj = _iter->dataFor(member->loc).toBson();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

        return returnIfMatches(member, id, out);
    }

    PlanStage::StageState CollectionScan::returnIfMatches(WorkingSetMember* member,
                                                          WorkingSetID memberID,
                                                          WorkingSetID* out) {
        ++_specificStats.docsTested;

        if (Filter::passes(member, _filter)) {
            *out = memberID;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            _workingSet->free(memberID);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
    }

    bool CollectionScan::isEOF() {
        if ((0 != _params.maxScan) && (_specificStats.docsTested >= _params.maxScan)) {
            return true;
        }
        if (_isDead) { return true; }
        if (NULL == _iter) { return false; }
        if (_params.tailable) { return false; } // tailable cursors can return data later.
        return _iter->isEOF();
    }

    void CollectionScan::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        // We don't care about mutations since we apply any filters to the result when we (possibly)
        // return it.
        if (INVALIDATION_DELETION != type) {
            return;
        }

        // If we're here, 'dl' is being deleted.

        // Deletions can harm the underlying RecordIterator so we must pass them down.
        if (NULL != _iter) {
            _iter->invalidate(dl);
        }

        if (_params.tailable && dl == _lastSeenLoc) {
            // This means that deletes have caught up to the reader. We want to error in this case
            // so readers don't miss potentially important data.
            _isDead = true;
        }
    }

    void CollectionScan::saveState() {
        ++_commonStats.yields;
        if (NULL != _iter) {
            _iter->saveState();
        }
    }

    void CollectionScan::restoreState(OperationContext* opCtx) {
        _txn = opCtx;
        ++_commonStats.unyields;
        if (NULL != _iter) {
            if (!_iter->restoreState(opCtx)) {
                warning() << "Collection dropped or state deleted during yield of CollectionScan";
                _isDead = true;
            }
        }
    }

    vector<PlanStage*> CollectionScan::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    PlanStageStats* CollectionScan::getStats() {
        _commonStats.isEOF = isEOF();

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_COLLSCAN));
        ret->specific.reset(new CollectionScanStats(_specificStats));
        return ret.release();
    }

    const CommonStats* CollectionScan::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* CollectionScan::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
