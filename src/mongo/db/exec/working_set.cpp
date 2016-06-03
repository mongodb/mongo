/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/exec/working_set.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_fetcher.h"

namespace mongo {

using std::string;

namespace dps = ::mongo::dotted_path_support;

WorkingSet::MemberHolder::MemberHolder() : member(NULL) {}
WorkingSet::MemberHolder::~MemberHolder() {}

WorkingSet::WorkingSet() : _freeList(INVALID_ID) {}

WorkingSet::~WorkingSet() {
    for (size_t i = 0; i < _data.size(); i++) {
        delete _data[i].member;
    }
}

WorkingSetID WorkingSet::allocate() {
    if (_freeList == INVALID_ID) {
        // The free list is empty so we need to make a single new WSM to return. This relies on
        // vector::resize being amortized O(1) for efficient allocation. Note that the free list
        // remains empty until something is returned by a call to free().
        WorkingSetID id = _data.size();
        _data.resize(_data.size() + 1);
        _data.back().nextFreeOrSelf = id;
        _data.back().member = new WorkingSetMember();
        return id;
    }

    // Pop the head off the free list and return it.
    WorkingSetID id = _freeList;
    _freeList = _data[id].nextFreeOrSelf;
    _data[id].nextFreeOrSelf = id;  // set to self to mark as in-use
    return id;
}

void WorkingSet::free(WorkingSetID i) {
    MemberHolder& holder = _data[i];
    verify(i < _data.size());            // ID has been allocated.
    verify(holder.nextFreeOrSelf == i);  // ID currently in use.

    // Free resources and push this WSM to the head of the freelist.
    holder.member->clear();
    holder.nextFreeOrSelf = _freeList;
    _freeList = i;
}

void WorkingSet::flagForReview(WorkingSetID i) {
    WorkingSetMember* member = get(i);
    verify(WorkingSetMember::OWNED_OBJ == member->_state);
    _flagged.insert(i);
}

const unordered_set<WorkingSetID>& WorkingSet::getFlagged() const {
    return _flagged;
}

bool WorkingSet::isFlagged(WorkingSetID id) const {
    invariant(id < _data.size());
    return _flagged.end() != _flagged.find(id);
}

void WorkingSet::clear() {
    for (size_t i = 0; i < _data.size(); i++) {
        delete _data[i].member;
    }
    _data.clear();

    // Since working set is now empty, the free list pointer should
    // point to nothing.
    _freeList = INVALID_ID;

    _flagged.clear();
    _yieldSensitiveIds.clear();
}

void WorkingSet::transitionToRecordIdAndIdx(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->_state = WorkingSetMember::RID_AND_IDX;
    _yieldSensitiveIds.push_back(id);
}

void WorkingSet::transitionToRecordIdAndObj(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->_state = WorkingSetMember::RID_AND_OBJ;
}

void WorkingSet::transitionToOwnedObj(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->transitionToOwnedObj();
}

std::vector<WorkingSetID> WorkingSet::getAndClearYieldSensitiveIds() {
    std::vector<WorkingSetID> out;
    // Clear '_yieldSensitiveIds' by swapping it into the set to be returned.
    _yieldSensitiveIds.swap(out);
    return out;
}

//
// WorkingSetMember
//

WorkingSetMember::WorkingSetMember() {}

WorkingSetMember::~WorkingSetMember() {}

void WorkingSetMember::clear() {
    for (size_t i = 0; i < WSM_COMPUTED_NUM_TYPES; i++) {
        _computed[i].reset();
    }

    keyData.clear();
    obj.reset();
    _state = WorkingSetMember::INVALID;
}

WorkingSetMember::MemberState WorkingSetMember::getState() const {
    return _state;
}

void WorkingSetMember::transitionToOwnedObj() {
    invariant(obj.value().isOwned());
    _state = OWNED_OBJ;
}


bool WorkingSetMember::hasRecordId() const {
    return _state == RID_AND_IDX || _state == RID_AND_OBJ;
}

bool WorkingSetMember::hasObj() const {
    return _state == OWNED_OBJ || _state == RID_AND_OBJ;
}

bool WorkingSetMember::hasOwnedObj() const {
    return _state == OWNED_OBJ || (_state == RID_AND_OBJ && obj.value().isOwned());
}

void WorkingSetMember::makeObjOwnedIfNeeded() {
    if (supportsDocLocking() && _state == RID_AND_OBJ && !obj.value().isOwned()) {
        obj.setValue(obj.value().getOwned());
    }
}

bool WorkingSetMember::hasComputed(const WorkingSetComputedDataType type) const {
    return _computed[type].get();
}

const WorkingSetComputedData* WorkingSetMember::getComputed(
    const WorkingSetComputedDataType type) const {
    verify(_computed[type]);
    return _computed[type].get();
}

void WorkingSetMember::addComputed(WorkingSetComputedData* data) {
    verify(!hasComputed(data->type()));
    _computed[data->type()].reset(data);
}

void WorkingSetMember::setFetcher(RecordFetcher* fetcher) {
    _fetcher.reset(fetcher);
}

RecordFetcher* WorkingSetMember::releaseFetcher() {
    return _fetcher.release();
}

bool WorkingSetMember::hasFetcher() const {
    return NULL != _fetcher.get();
}

bool WorkingSetMember::getFieldDotted(const string& field, BSONElement* out) const {
    // If our state is such that we have an object, use it.
    if (hasObj()) {
        *out = dps::extractElementAtPath(obj.value(), field);
        return true;
    }

    // Our state should be such that we have index data/are covered.
    for (size_t i = 0; i < keyData.size(); ++i) {
        BSONObjIterator keyPatternIt(keyData[i].indexKeyPattern);
        BSONObjIterator keyDataIt(keyData[i].keyData);

        while (keyPatternIt.more()) {
            BSONElement keyPatternElt = keyPatternIt.next();
            verify(keyDataIt.more());
            BSONElement keyDataElt = keyDataIt.next();

            if (field == keyPatternElt.fieldName()) {
                *out = keyDataElt;
                return true;
            }
        }
    }

    return false;
}

size_t WorkingSetMember::getMemUsage() const {
    size_t memUsage = 0;

    if (hasRecordId()) {
        memUsage += sizeof(RecordId);
    }

    // XXX: Unowned objects count towards current size.
    //      See SERVER-12579
    if (hasObj()) {
        memUsage += obj.value().objsize();
    }

    for (size_t i = 0; i < keyData.size(); ++i) {
        const IndexKeyDatum& keyDatum = keyData[i];
        memUsage += keyDatum.keyData.objsize();
    }

    return memUsage;
}

}  // namespace mongo
