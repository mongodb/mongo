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
#include "mongo/util/fail_point_service.h"

#include "mongo/db/client.h" // XXX-ERH

namespace mongo {

    // static
    const char* CollectionScan::kStageType = "COLLSCAN";

    CollectionScan::CollectionScan(const CollectionScanParams& params,
                                   WorkingSet* workingSet,
                                   const MatchExpression* filter)
        : _workingSet(workingSet),
          _filter(filter),
          _params(params),
          _nsDropped(false),
          _commonStats(kStageType) { }

    PlanStage::StageState CollectionScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (_nsDropped) { return PlanStage::DEAD; }

        // Do some init if we haven't already.
        if (NULL == _iter) {
            if ( _params.collection == NULL ) {
                _nsDropped = true;
                return PlanStage::DEAD;
            }

            _iter.reset( _params.collection->getIterator( _params.start,
                                                          _params.tailable,
                                                          _params.direction ) );

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
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
        member->obj = _params.collection->docFor(member->loc);
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

        // Deletions can harm the underlying RecordIterator so we must pass them down.
        if (NULL != _iter) {
            _iter->invalidate(dl);
        }
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

}  // namespace mongo
