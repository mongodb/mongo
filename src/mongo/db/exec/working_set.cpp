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

    const WorkingSetID WorkingSet::INVALID_ID = -1;

    WorkingSet::WorkingSet() : _nextId(0) { }

    WorkingSet::~WorkingSet() {
        for (DataMap::const_iterator i = _data.begin(); i != _data.end(); ++i) {
            delete i->second;
        }
    }

    WorkingSetID WorkingSet::allocate() {
        verify(_data.end() == _data.find(_nextId));
        _data[_nextId] = new WorkingSetMember();
        return _nextId++;
    }

    WorkingSetMember* WorkingSet::get(const WorkingSetID& i) {
        DataMap::iterator it = _data.find(i);
        verify(_data.end() != it);
        return it->second;
    }

    void WorkingSet::free(const WorkingSetID& i) {
        DataMap::iterator it = _data.find(i);
        verify(_data.end() != it);
        delete it->second;
        _data.erase(it);
    }

    void WorkingSet::flagForReview(const WorkingSetID& i) {
        WorkingSetMember* member = get(i);
        verify(WorkingSetMember::OWNED_OBJ == member->state);
        _flagged.push_back(i);
    }

    const vector<WorkingSetID>& WorkingSet::getFlagged() const {
        return _flagged;
    }

    WorkingSetMember::WorkingSetMember() : state(WorkingSetMember::INVALID) { }

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
