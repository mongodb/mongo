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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/fetch.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* FetchStage::kStageType = "FETCH";

FetchStage::FetchStage(OperationContext* txn,
                       WorkingSet* ws,
                       PlanStage* child,
                       const MatchExpression* filter,
                       const Collection* collection)
    : PlanStage(kStageType, txn),
      _collection(collection),
      _ws(ws),
      _filter(filter),
      _idRetrying(WorkingSet::INVALID_ID) {
    _children.emplace_back(child);
}

FetchStage::~FetchStage() {}

bool FetchStage::isEOF() {
    if (WorkingSet::INVALID_ID != _idRetrying) {
        // We asked the parent for a page-in, but still haven't had a chance to return the
        // paged in document
        return false;
    }

    return child()->isEOF();
}

PlanStage::StageState FetchStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // Either retry the last WSM we worked on or get a new one from our child.
    WorkingSetID id;
    StageState status;
    if (_idRetrying == WorkingSet::INVALID_ID) {
        status = child()->work(&id);
    } else {
        status = ADVANCED;
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    if (PlanStage::ADVANCED == status) {
        WorkingSetMember* member = _ws->get(id);

        // If there's an obj there, there is no fetching to perform.
        if (member->hasObj()) {
            ++_specificStats.alreadyHasObj;
        } else {
            // We need a valid RecordId to fetch from and this is the only state that has one.
            verify(WorkingSetMember::RID_AND_IDX == member->getState());
            verify(member->hasRecordId());

            try {
                if (!_cursor)
                    _cursor = _collection->getCursor(getOpCtx());

                if (auto fetcher = _cursor->fetcherForId(member->recordId)) {
                    // There's something to fetch. Hand the fetcher off to the WSM, and pass up
                    // a fetch request.
                    _idRetrying = id;
                    member->setFetcher(fetcher.release());
                    *out = id;
                    return NEED_YIELD;
                }

                // The doc is already in memory, so go ahead and grab it. Now we have a RecordId
                // as well as an unowned object
                if (!WorkingSetCommon::fetch(getOpCtx(), _ws, id, _cursor)) {
                    _ws->free(id);
                    return NEED_TIME;
                }
            } catch (const WriteConflictException& wce) {
                // Ensure that the BSONObj underlying the WorkingSetMember is owned because it may
                // be freed when we yield.
                member->makeObjOwnedIfNeeded();
                _idRetrying = id;
                *out = WorkingSet::INVALID_ID;
                return NEED_YIELD;
            }
        }

        return returnIfMatches(member, id, out);
    } else if (PlanStage::FAILURE == status || PlanStage::DEAD == status) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it
        // failed, in which case 'id' is valid.  If ID is invalid, we
        // create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            mongoutils::str::stream ss;
            ss << "fetch stage failed to read in results from child";
            Status status(ErrorCodes::InternalError, ss);
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        }
        return status;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

void FetchStage::doSaveState() {
    if (_cursor)
        _cursor->saveUnpositioned();
}

void FetchStage::doRestoreState() {
    if (_cursor)
        _cursor->restore();
}

void FetchStage::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void FetchStage::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(getOpCtx());
}

void FetchStage::doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    // It's possible that the recordId getting invalidated is the one we're about to
    // fetch. In this case we do a "forced fetch" and put the WSM in owned object state.
    if (WorkingSet::INVALID_ID != _idRetrying) {
        WorkingSetMember* member = _ws->get(_idRetrying);
        if (member->hasRecordId() && (member->recordId == dl)) {
            // Fetch it now and kill the recordId.
            WorkingSetCommon::fetchAndInvalidateRecordId(txn, member, _collection);
        }
    }
}

PlanStage::StageState FetchStage::returnIfMatches(WorkingSetMember* member,
                                                  WorkingSetID memberID,
                                                  WorkingSetID* out) {
    // We consider "examining a document" to be every time that we pass a document through
    // a filter by calling Filter::passes(...) below. Therefore, the 'docsExamined' metric
    // is not always equal to the number of documents that were fetched from the collection.
    // In particular, we can sometimes generate plans which have two fetch stages. The first
    // one actually grabs the document from the collection, and the second passes the
    // document through a second filter.
    //
    // One common example of this is geoNear. Suppose that a geoNear plan is searching an
    // annulus to find 2dsphere-indexed documents near some point (x, y) on the globe.
    // After fetching documents within geo hashes that intersect this annulus, the docs are
    // fetched and filtered to make sure that they really do fall into this annulus. However,
    // the user might also want to find only those documents for which accommodationType==
    // "restaurant". The planner will add a second fetch stage to filter by this non-geo
    // predicate.
    ++_specificStats.docsExamined;

    if (Filter::passes(member, _filter)) {
        *out = memberID;
        return PlanStage::ADVANCED;
    } else {
        _ws->free(memberID);
        return PlanStage::NEED_TIME;
    }
}

unique_ptr<PlanStageStats> FetchStage::getStats() {
    _commonStats.isEOF = isEOF();

    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (NULL != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_FETCH);
    ret->specific = make_unique<FetchStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* FetchStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
