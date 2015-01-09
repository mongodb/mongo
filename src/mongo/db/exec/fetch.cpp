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

#include "mongo/db/exec/fetch.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using std::auto_ptr;
    using std::vector;

    // static
    const char* FetchStage::kStageType = "FETCH";

    FetchStage::FetchStage(OperationContext* txn,
                           WorkingSet* ws,
                           PlanStage* child,
                           const MatchExpression* filter,
                           const Collection* collection)
        : _txn(txn),
          _collection(collection),
          _ws(ws),
          _child(child),
          _filter(filter),
          _idBeingPagedIn(WorkingSet::INVALID_ID),
          _commonStats(kStageType) { }

    FetchStage::~FetchStage() { }

    bool FetchStage::isEOF() {
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            // We asked the parent for a page-in, but still haven't had a chance to return the
            // paged in document
            return false;
        }

        return _child->isEOF();
    }

    PlanStage::StageState FetchStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        // We might have a fetched result to return.
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            WorkingSetID id = _idBeingPagedIn;
            _idBeingPagedIn = WorkingSet::INVALID_ID;
            WorkingSetMember* member = _ws->get(id);

            WorkingSetCommon::completeFetch(_txn, member, _collection);

            return returnIfMatches(member, id, out);
        }

        // If we're here, we're not waiting for a RecordId to be fetched.  Get another to-be-fetched
        // result from our child.
        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            WorkingSetMember* member = _ws->get(id);

            // If there's an obj there, there is no fetching to perform.
            if (member->hasObj()) {
                ++_specificStats.alreadyHasObj;
            }
            else {
                // We need a valid loc to fetch from and this is the only state that has one.
                verify(WorkingSetMember::LOC_AND_IDX == member->state);
                verify(member->hasLoc());

                // We might need to retrieve 'nextLoc' from secondary storage, in which case we send
                // a NEED_FETCH request up to the PlanExecutor.
                if (!member->loc.isNull()) {
                    std::auto_ptr<RecordFetcher> fetcher(
                        _collection->documentNeedsFetch(_txn, member->loc));
                    if (NULL != fetcher.get()) {
                        // There's something to fetch. Hand the fetcher off to the WSM, and pass up
                        // a fetch request.
                        _idBeingPagedIn = id;
                        member->setFetcher(fetcher.release());
                        *out = id;
                        _commonStats.needFetch++;
                        return NEED_FETCH;
                    }
                }

                // The doc is already in memory, so go ahead and grab it. Now we have a RecordId
                // as well as an unowned object
                member->obj = _collection->docFor(_txn, member->loc);
                member->keyData.clear();
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
            }

            return returnIfMatches(member, id, out);
        }
        else if (PlanStage::FAILURE == status) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "fetch stage failed to read in results from child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
            return status;
        }
        else if (PlanStage::NEED_TIME == status) {
            ++_commonStats.needTime;
        }
        else if (PlanStage::NEED_FETCH == status) {
            ++_commonStats.needFetch;
            *out = id;
        }

        return status;
    }

    void FetchStage::saveState() {
        _txn = NULL;
        ++_commonStats.yields;
        _child->saveState();
    }

    void FetchStage::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void FetchStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        _child->invalidate(txn, dl, type);

        // It's possible that the loc getting invalidated is the one we're about to
        // fetch. In this case we do a "forced fetch" and put the WSM in owned object state.
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            WorkingSetMember* member = _ws->get(_idBeingPagedIn);
            if (member->hasLoc() && (member->loc == dl)) {
                // Fetch it now and kill the diskloc.
                WorkingSetCommon::fetchAndInvalidateLoc(txn, member, _collection);
            }
        }
    }

    PlanStage::StageState FetchStage::returnIfMatches(WorkingSetMember* member,
                                                      WorkingSetID memberID,
                                                      WorkingSetID* out) {
        ++_specificStats.docsExamined;

        if (Filter::passes(member, _filter)) {
            if (NULL != _filter) {
                ++_specificStats.matchTested;
            }

            *out = memberID;

            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            _ws->free(memberID);

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
    }

    vector<PlanStage*> FetchStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* FetchStage::getStats() {
        _commonStats.isEOF = isEOF();

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_FETCH));
        ret->specific.reset(new FetchStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* FetchStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* FetchStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
