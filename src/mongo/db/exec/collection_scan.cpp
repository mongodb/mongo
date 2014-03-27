/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/exec/collection_scan.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/structure/collection_iterator.h"
#include "mongo/util/fail_point_service.h"

#include "mongo/db/client.h" // XXX-ERH
#include "mongo/db/pdfile.h" // XXX-ERH/ACM

namespace mongo {

    // Some fail points for testing.
    MONGO_FP_DECLARE(collscanInMemoryFail);
    MONGO_FP_DECLARE(collscanInMemorySucceed);

    // static
    bool CollectionScan::diskLocInMemory(DiskLoc loc) {
        if (MONGO_FAIL_POINT(collscanInMemoryFail)) {
            return false;
        }

        if (MONGO_FAIL_POINT(collscanInMemorySucceed)) {
            return true;
        }

        return loc.rec()->likelyInPhysicalMemory();
    }

    CollectionScan::CollectionScan(const CollectionScanParams& params,
                                   WorkingSet* workingSet,
                                   const MatchExpression* filter)
        : _workingSet(workingSet),
          _filter(filter),
          _params(params),
          _nsDropped(false) {

        // We pre-allocate a WSID and use it to pass up fetch requests.  It is only
        // used to pass up fetch requests and we should never use it for anything else.
        _wsidForFetch = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(_wsidForFetch);
        // Kind of a lie since the obj isn't pointing to the data at loc. but the obj
        // won't be used.
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
    }

    PlanStage::StageState CollectionScan::work(WorkingSetID* out) {
        ++_commonStats.works;
        if (_nsDropped) { return PlanStage::DEAD; }

        // Do some init if we haven't already.
        if (NULL == _iter) {
            Collection* collection = cc().database()->getCollection( _params.ns );
            if ( collection == NULL ) {
                _nsDropped = true;
                return PlanStage::DEAD;
            }

            _iter.reset( collection->getIterator( _params.start,
                                                  _params.tailable,
                                                  _params.direction ) );

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }

        // See if the record we're about to access is in memory.  If it's not, pass a fetch
        // request up.
        if (!isEOF()) {
            DiskLoc curr = _iter->curr();
            if (!curr.isNull() && !diskLocInMemory(curr)) {
                WorkingSetMember* member = _workingSet->get(_wsidForFetch);
                member->loc = curr;
                *out = _wsidForFetch;
                return PlanStage::NEED_FETCH;
            }
        }

        // What we'll return to the user.
        DiskLoc nextLoc;

        // Should we try getNext() on the underlying _iter if we're EOF?  Yes, if we're tailable.
        if (isEOF()) {
            if (!_params.tailable) {
                return PlanStage::IS_EOF;
            }
            else {
                // See if _iter gives us anything new.
                nextLoc = _iter->getNext();
                if (nextLoc.isNull()) {
                    // Nope, still EOF.
                    return PlanStage::IS_EOF;
                }
            }
        }
        else {
            nextLoc = _iter->getNext();
        }

        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = nextLoc;
        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

        ++_specificStats.docsTested;

        if (Filter::passes(member, _filter)) {
            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            _workingSet->free(id);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
    }

    bool CollectionScan::isEOF() {
        if ((0 != _params.maxScan) && (_specificStats.docsTested >= _params.maxScan)) {
            return true;
        }
        if (_nsDropped) { return true; }
        if (NULL == _iter) { return false; }
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

        // Deletions can harm the underlying CollectionIterator so we must pass them down.
        if (NULL != _iter) {
            _iter->invalidate(dl);
        }

        // We might have 'dl' inside of the WSM that _wsidForFetch references.  This is OK because
        // the runner who handles the fetch request does so before releasing any locks (and allowing
        // the DiskLoc to be deleted).  We also don't use any data in the WSM referenced by
        // _wsidForFetch so it's OK to leave the DiskLoc there.
    }

    void CollectionScan::prepareToYield() {
        ++_commonStats.yields;
        if (NULL != _iter) {
            _iter->prepareToYield();
        }
    }

    void CollectionScan::recoverFromYield() {
        ++_commonStats.unyields;
        if (NULL != _iter) {
            if (!_iter->recoverFromYield()) {
                warning() << "Collection dropped or state deleted during yield of CollectionScan";
                _nsDropped = true;
            }
        }
    }

    PlanStageStats* CollectionScan::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_COLLSCAN));
        ret->specific.reset(new CollectionScanStats(_specificStats));
        return ret.release();
    }

}  // namespace mongo
