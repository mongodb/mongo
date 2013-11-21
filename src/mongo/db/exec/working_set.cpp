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

#include "mongo/db/exec/working_set.h"

#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    WorkingSet::MemberHolder::MemberHolder() : flagged(false), member(NULL) { }
    WorkingSet::MemberHolder::~MemberHolder() {}

    WorkingSet::WorkingSet() : _freeList(INVALID_ID) { }

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
        _data[id].nextFreeOrSelf = id; // set to self to mark as in-use
        return id;
    }

    void WorkingSet::free(const WorkingSetID& i) {
        MemberHolder& holder = _data[i];
        verify(i < _data.size()); // ID has been allocated.
        verify(holder.nextFreeOrSelf == i); // ID currently in use.

        // Free resources and push this WSM to the head of the freelist.
        holder.member->clear();
        holder.nextFreeOrSelf = _freeList;
        _freeList = i;
    }

    void WorkingSet::flagForReview(const WorkingSetID& i) {
        WorkingSetMember* member = get(i);
        verify(WorkingSetMember::OWNED_OBJ == member->state);
        _data[i].flagged = true;
    }

    unordered_set<WorkingSetID> WorkingSet::getFlagged() const {
        // This is slow, but it is only for tests.
        unordered_set<WorkingSetID> out;
        for (size_t i = 0; i < _data.size(); i++) {
            if (_data[i].flagged) {
                out.insert(i);
            }
        }
        return out;
    }

    bool WorkingSet::isFlagged(WorkingSetID id) const {
        verify(id < _data.size());
        return _data[id].flagged;
    }

    WorkingSetMember::WorkingSetMember() : state(WorkingSetMember::INVALID) { }

    WorkingSetMember::~WorkingSetMember() { }

    void WorkingSetMember::clear() {
        for (size_t i = 0; i < WSM_COMPUTED_NUM_TYPES; i++) {
            _computed[i].reset();
        }

        keyData.clear();
        obj = BSONObj();
        state = WorkingSetMember::INVALID;
    }

    bool WorkingSetMember::hasLoc() const {
        return state == LOC_AND_IDX || state == LOC_AND_UNOWNED_OBJ;
    }

    bool WorkingSetMember::hasObj() const {
        return hasOwnedObj() || hasUnownedObj();
    }

    bool WorkingSetMember::hasOwnedObj() const {
        return state == OWNED_OBJ;
    }

    bool WorkingSetMember::hasUnownedObj() const {
        return state == LOC_AND_UNOWNED_OBJ;
    }

    bool WorkingSetMember::hasComputed(const WorkingSetComputedDataType type) const {
        return _computed[type];
    }

    const WorkingSetComputedData* WorkingSetMember::getComputed(const WorkingSetComputedDataType type) const {
        verify(_computed[type]);
        return _computed[type].get();
    }

    void WorkingSetMember::addComputed(WorkingSetComputedData* data) {
        verify(!hasComputed(data->type()));
        _computed[data->type()].reset(data);
    }

    bool WorkingSetMember::getFieldDotted(const string& field, BSONElement* out) const {
        // If our state is such that we have an object, use it.
        if (hasObj()) {
            *out = obj.getFieldDotted(field);
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

}  // namespace mongo
