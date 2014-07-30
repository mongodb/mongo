/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/exec/multi_iterator.h"

namespace mongo {

    void MultiIteratorStage::addIterator(RecordIterator* it) {
        _iterators.push_back(it);
    }

    PlanStage::StageState MultiIteratorStage::work(WorkingSetID* out) {
        if ( _collection == NULL )
            return PlanStage::DEAD;

        DiskLoc next = _advance();
        if (next.isNull())
            return PlanStage::IS_EOF;

        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->loc = next;
        member->obj = _collection->docFor(next);
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        return PlanStage::ADVANCED;
    }

    bool MultiIteratorStage::isEOF() {
        return _collection == NULL || _iterators.empty();
    }

    void MultiIteratorStage::kill() {
        _collection = NULL;
        _iterators.clear();
    }

    void MultiIteratorStage::saveState() {
        for (size_t i = 0; i < _iterators.size(); i++) {
            _iterators[i]->saveState();
        }
    }

    void MultiIteratorStage::restoreState(OperationContext* opCtx) {
        for (size_t i = 0; i < _iterators.size(); i++) {
            if (!_iterators[i]->restoreState()) {
                kill();
            }
        }
    }

    void MultiIteratorStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        switch ( type ) {
        case INVALIDATION_DELETION:
            for (size_t i = 0; i < _iterators.size(); i++) {
                _iterators[i]->invalidate(dl);
            }
            break;
        case INVALIDATION_MUTATION:
            // no-op
            break;
        }
    }

    vector<PlanStage*> MultiIteratorStage::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    DiskLoc MultiIteratorStage::_advance() {
        while (!_iterators.empty()) {
            DiskLoc out = _iterators.back()->getNext();
            if (!out.isNull())
                return out;

            _iterators.popAndDeleteBack();
        }

        return DiskLoc();
    }

} // namespace mongo
