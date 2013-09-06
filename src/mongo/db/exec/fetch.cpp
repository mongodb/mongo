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

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

    // Some fail points for testing.
    MONGO_FP_DECLARE(fetchInMemoryFail);
    MONGO_FP_DECLARE(fetchInMemorySucceed);

    FetchStage::FetchStage(WorkingSet* ws, PlanStage* child, const MatchExpression* filter)
        : _ws(ws), _child(child), _filter(filter), _idBeingPagedIn(WorkingSet::INVALID_ID) { }

    FetchStage::~FetchStage() { }

    bool FetchStage::isEOF() {
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            // We asked our parent for a page-in but he didn't get back to us.  We still need to
            // return the result that _idBeingPagedIn refers to.
            return false;
        }

        return _child->isEOF();
    }

    bool recordInMemory(const char* data) {
        if (MONGO_FAIL_POINT(fetchInMemoryFail)) {
            return false;
        }

        if (MONGO_FAIL_POINT(fetchInMemorySucceed)) {
            return true;
        }

        return Record::likelyInPhysicalMemory(data);
    }

    PlanStage::StageState FetchStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        // If we asked our parent for a page-in last time work(...) was called, finish the fetch.
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            return fetchCompleted(out);
        }

        // If we're here, we're not waiting for a DiskLoc to be fetched.  Get another to-be-fetched
        // result from our child.
        WorkingSetID id;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            WorkingSetMember* member = _ws->get(id);

            // If there's an obj there, there is no fetching to perform.
            if (member->hasObj()) {
                ++_specificStats.alreadyHasObj;
                return returnIfMatches(member, id, out);
            }

            // We need a valid loc to fetch from and this is the only state that has one.
            verify(WorkingSetMember::LOC_AND_IDX == member->state);
            verify(member->hasLoc());

            Record* record = member->loc.rec();
            const char* data = record->dataNoThrowing();

            if (!recordInMemory(data)) {
                // member->loc points to a record that's NOT in memory.  Pass a fetch request up.
                verify(WorkingSet::INVALID_ID == _idBeingPagedIn);
                _idBeingPagedIn = id;
                *out = id;
                ++_commonStats.needFetch;
                return PlanStage::NEED_FETCH;
            }
            else {
                // Don't need index data anymore as we have an obj.
                member->keyData.clear();
                member->obj = BSONObj(data);
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                return returnIfMatches(member, id, out);
            }
        }
        else {
            if (PlanStage::NEED_FETCH == status) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == status) {
                ++_commonStats.needTime;
            }
            return status;
        }
    }

    void FetchStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void FetchStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void FetchStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;

        _child->invalidate(dl);

        // If we're holding on to an object that we're waiting for the runner to page in...
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            WorkingSetMember* member = _ws->get(_idBeingPagedIn);
            verify(member->hasLoc());
            // The DiskLoc is about to perish so we force a fetch of the data.
            if (member->loc == dl) {
                // This is a fetch inside of a write lock (that somebody else holds) but the other
                // holder is likely operating on this object so this shouldn't have to hit disk.
                WorkingSetCommon::fetchAndInvalidateLoc(member);
                ++_specificStats.forcedFetches;
            }
        }
    }

    PlanStage::StageState FetchStage::fetchCompleted(WorkingSetID* out) {
        WorkingSetMember* member = _ws->get(_idBeingPagedIn);

        // The DiskLoc we're waiting to page in was invalidated (forced fetch).  Test for
        // matching and maybe pass it up.
        if (member->state == WorkingSetMember::OWNED_OBJ) {
            WorkingSetID memberID = _idBeingPagedIn;
            _idBeingPagedIn = WorkingSet::INVALID_ID;
            return returnIfMatches(member, memberID, out);
        }

        // Assume that the caller has fetched appropriately.
        // TODO: Do we want to double-check the runner?  Not sure how reliable likelyInMemory is
        // on all platforms.
        verify(member->hasLoc());
        verify(!member->hasObj());

        // Make the (unowned) object.
        Record* record = member->loc.rec();
        const char* data = record->dataNoThrowing();
        member->obj = BSONObj(data);

        // Don't need index data anymore as we have an obj.
        member->keyData.clear();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        verify(!member->obj.isOwned());

        // Return the obj if it passes our filter.
        WorkingSetID memberID = _idBeingPagedIn;
        _idBeingPagedIn = WorkingSet::INVALID_ID;
        return returnIfMatches(member, memberID, out);
    }

    PlanStage::StageState FetchStage::returnIfMatches(WorkingSetMember* member,
                                                      WorkingSetID memberID,
                                                      WorkingSetID* out) {
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

    PlanStageStats* FetchStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->setSpecific<FetchStats>(_specificStats);
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

}  // namespace mongo
