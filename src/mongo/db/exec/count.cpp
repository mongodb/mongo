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

#include "mongo/db/exec/count.h"

#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    // static
    const char* Count::kStageType = "COUNT";

    Count::Count(const CountParams& params, WorkingSet* workingSet)
        : _workingSet(workingSet),
          _descriptor(params.descriptor),
          _iam(params.descriptor->getIndexCatalog()->getIndex(params.descriptor)),
          _btreeCursor(NULL),
          _params(params),
          _hitEnd(false),
          _shouldDedup(params.descriptor->isMultikey()),
          _commonStats(kStageType) { }

    void Count::initIndexCursor() {
        CursorOptions cursorOptions;
        cursorOptions.direction = CursorOptions::INCREASING;

        IndexCursor *cursor;
        Status s = _iam->newCursor(cursorOptions, &cursor);
        verify(s.isOK());
        verify(cursor);

        // Is this assumption always valid?  See SERVER-12397
        _btreeCursor.reset(static_cast<BtreeIndexCursor*>(cursor));

        // _btreeCursor points at our start position.  We move it forward until it hits a cursor
        // that points at the end.
        _btreeCursor->seek(_params.startKey, !_params.startKeyInclusive);

        // Create the cursor that points at our end position.
        IndexCursor* endCursor;
        verify(_iam->newCursor(cursorOptions, &endCursor).isOK());
        verify(endCursor);

        // Is this assumption always valid?  See SERVER-12397
        _endCursor.reset(static_cast<BtreeIndexCursor*>(endCursor));

        // If the end key is inclusive we want to point *past* it since that's the end.
        _endCursor->seek(_params.endKey, _params.endKeyInclusive);

        // See if we've hit the end already.
        checkEnd();
    }

    void Count::checkEnd() {
        if (isEOF()) { return; }

        if (_endCursor->isEOF()) {
            // If the endCursor is EOF we're only done when our 'current count position' hits EOF.
            _hitEnd = _btreeCursor->isEOF();
        }
        else {
            // If not, we're only done when we hit the end cursor's (valid) position.
            _hitEnd = _btreeCursor->pointsAt(*_endCursor.get());
        }
    }

    PlanStage::StageState Count::work(WorkingSetID* out) {
        if (NULL == _btreeCursor.get()) {
            // First call to work().  Perform cursor init.
            initIndexCursor();
            checkEnd();
            return PlanStage::NEED_TIME;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        DiskLoc loc = _btreeCursor->getValue();
        _btreeCursor->next();
        checkEnd();

        if (_shouldDedup) {
            if (_returned.end() != _returned.find(loc)) {
                return PlanStage::NEED_TIME;
            }
            else {
                _returned.insert(loc);
            }
        }

        *out = WorkingSet::INVALID_ID;
        return PlanStage::ADVANCED;
    }

    bool Count::isEOF() {
        if (NULL == _btreeCursor.get()) {
            // Have to call work() at least once.
            return false;
        }

        return _hitEnd || _btreeCursor->isEOF();
    }

    void Count::prepareToYield() {
        if (_hitEnd || (NULL == _btreeCursor.get())) { return; }

        _btreeCursor->savePosition();
        _endCursor->savePosition();
    }

    void Count::recoverFromYield() {
        if (_hitEnd || (NULL == _btreeCursor.get())) { return; }

        if (!_btreeCursor->restorePosition().isOK()) {
            _hitEnd = true;
            return;
        }

        if (_btreeCursor->isEOF()) {
            _hitEnd = true;
            return;
        }

        // See if we're somehow already past our end key (maybe the thing we were pointing at got
        // deleted...)
        int cmp = _btreeCursor->getKey().woCompare(_params.endKey, _descriptor->keyPattern(), false);
        if (cmp > 0 || (cmp == 0 && !_params.endKeyInclusive)) {
            _hitEnd = true;
            return;
        }

        if (!_endCursor->restorePosition().isOK()) {
            _hitEnd = true;
            return;
        }

        // If we were EOF when we yielded we don't always want to have _btreeCursor run until
        // EOF.  New documents may have been inserted after our endKey and our end marker
        // may be before them.
        //
        // As an example, say we're counting from 5 to 10 and the index only has keys
        // for 6, 7, 8, and 9.  btreeCursor will point at a 6 key at the start and the
        // endCursor will be EOF.  If we insert documents with keys 11 during a yield we
        // need to relocate the endCursor to point at them as the "end key" of our count.
        //
        // If we weren't EOF our end position might have moved around.  Relocate it.
        _endCursor->seek(_params.endKey, _params.endKeyInclusive);

        // This can change during yielding.
        _shouldDedup = _descriptor->isMultikey();

        checkEnd();
    }

    void Count::invalidate(const DiskLoc& dl, InvalidationType type) {
        // The only state we're responsible for holding is what DiskLocs to drop.  If a document
        // mutates the underlying index cursor will deal with it.
        if (INVALIDATION_MUTATION == type) {
            return;
        }

        // If we see this DiskLoc again, it may not be the same document it was before, so we want
        // to return it if we see it again.
        unordered_set<DiskLoc, DiskLoc::Hasher>::iterator it = _returned.find(dl);
        if (it != _returned.end()) {
            _returned.erase(it);
        }
    }

    PlanStageStats* Count::getStats() {
        // We don't collect stats since this stage is only used by the count command.
        // If count ever collects stats we must implement this.
        invariant(0);
        return NULL;
    }

}  // namespace mongo
