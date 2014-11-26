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

#include "mongo/db/exec/distinct_scan.h"

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

    // static
    const char* DistinctScan::kStageType = "DISTINCT";

    DistinctScan::DistinctScan(OperationContext* txn, const DistinctParams& params, WorkingSet* workingSet)
        : _txn(txn),
          _workingSet(workingSet),
          _descriptor(params.descriptor),
          _iam(params.descriptor->getIndexCatalog()->getIndex(params.descriptor)),
          _btreeCursor(NULL),
          _scanState(INITIALIZING),
          _params(params),
          _commonStats(kStageType) {
        _specificStats.keyPattern = _params.descriptor->keyPattern();
    }

    void DistinctScan::initIndexCursor() {
        // This function transitions from the initializing state to CHECKING_END. If
        // the initialization fails, however, then the state transitions to HIT_END.
        invariant(INITIALIZING == _scanState);

        // Create an IndexCursor over the btree we're distinct-ing over.
        CursorOptions cursorOptions;

        if (1 == _params.direction) {
            cursorOptions.direction = CursorOptions::INCREASING;
        }
        else {
            cursorOptions.direction = CursorOptions::DECREASING;
        }

        IndexCursor *cursor;
        Status s = _iam->newCursor(_txn, cursorOptions, &cursor);
        verify(s.isOK());
        verify(cursor);
        // Is this assumption always valid?  See SERVER-12397
        _btreeCursor.reset(static_cast<BtreeIndexCursor*>(cursor));

        // Create a new bounds checker.  The bounds checker gets our start key and assists in
        // executing the scan and staying within the required bounds.
        _checker.reset(new IndexBoundsChecker(&_params.bounds,
                                              _descriptor->keyPattern(),
                                              _params.direction));

        int nFields = _descriptor->keyPattern().nFields();
        // The start key is dumped into these two.
        vector<const BSONElement*> key;
        vector<bool> inc;
        key.resize(nFields);
        inc.resize(nFields);
        if (_checker->getStartKey(&key, &inc)) {
            _btreeCursor->seek(key, inc);
            _keyElts.resize(nFields);
            _keyEltsInc.resize(nFields);
        }
        else {
            _scanState = HIT_END;
        }

        // This method may throw an exception while it's doing initialization. If we've gotten
        // here, then we've done all the initialization without an exception being thrown. This
        // means it is safe to transition to the CHECKING_END state. In error cases, we transition
        // to HIT_END, so we should not change state again here.
        if (HIT_END != _scanState) {
            _scanState = CHECKING_END;
        }
    }

    PlanStage::StageState DistinctScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (INITIALIZING == _scanState) {
            invariant(NULL == _btreeCursor.get());
            initIndexCursor();
        }

        if (CHECKING_END == _scanState) {
            checkEnd();
        }

        if (isEOF()) {
            _commonStats.isEOF = true;
            return PlanStage::IS_EOF;
        }

        if (GETTING_NEXT == _scanState) {
            // Grab the next (key, value) from the index.
            BSONObj ownedKeyObj = _btreeCursor->getKey().getOwned();
            RecordId loc = _btreeCursor->getValue();

            // The underlying IndexCursor points at the *next* thing we want to return.  We do this
            // so that if we're scanning an index looking for docs to delete we don't continually
            // clobber the thing we're pointing at.

            // We skip to the next value of the _params.fieldNo-th field in the index key pattern.
            // This is the field we're distinct-ing over.
            _btreeCursor->skip(_btreeCursor->getKey(),
                               _params.fieldNo + 1,
                               true,
                               _keyElts,
                               _keyEltsInc);

            // On the next call to work, make sure that the cursor is still within the bounds.
            _scanState = CHECKING_END;

            // Package up the result for the caller.
            WorkingSetID id = _workingSet->allocate();
            WorkingSetMember* member = _workingSet->get(id);
            member->loc = loc;
            member->keyData.push_back(IndexKeyDatum(_descriptor->keyPattern(), ownedKeyObj));
            member->state = WorkingSetMember::LOC_AND_IDX;

            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }

        ++_commonStats.needTime;
        return PlanStage::NEED_TIME;
    }

    bool DistinctScan::isEOF() {
        if (INITIALIZING == _scanState) {
            // Have to call work() at least once.
            return false;
        }

        return HIT_END == _scanState || _btreeCursor->isEOF();
    }

    void DistinctScan::saveState() {
        _txn = NULL;
        ++_commonStats.yields;

        if (HIT_END == _scanState || INITIALIZING == _scanState) { return; }
        // We save these so that we know if the cursor moves during the yield.  If it moves, we have
        // to make sure its ending position is valid w.r.t. our bounds.
        if (!_btreeCursor->isEOF()) {
            _savedKey = _btreeCursor->getKey().getOwned();
            _savedLoc = _btreeCursor->getValue();
        }
        _btreeCursor->savePosition();
    }

    void DistinctScan::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;

        if (HIT_END == _scanState || INITIALIZING == _scanState) { return; }

        // We can have a valid position before we check isEOF(), restore the position, and then be
        // EOF upon restore.
        if (!_btreeCursor->restorePosition( opCtx ).isOK() || _btreeCursor->isEOF()) {
            _scanState = HIT_END;
            return;
        }

        if (!_savedKey.binaryEqual(_btreeCursor->getKey()) || _savedLoc != _btreeCursor->getValue()) {
            // Our restored position might be past endKey, see if we've hit the end.
            _scanState = CHECKING_END;
        }
    }

    void DistinctScan::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;
    }

    void DistinctScan::checkEnd() {
        if (isEOF()) {
            _commonStats.isEOF = true;
            return;
        }

        // Use _checker to see how things are.
        IndexBoundsChecker::KeyState keyState;
        keyState = _checker->checkKey(_btreeCursor->getKey(),
                                      &_keyEltsToUse,
                                      &_movePastKeyElts,
                                      &_keyElts,
                                      &_keyEltsInc);

        if (IndexBoundsChecker::DONE == keyState) {
            _scanState = HIT_END;
            return;
        }

        // This seems weird but it's the old definition of nscanned.
        ++_specificStats.keysExamined;

        if (IndexBoundsChecker::VALID == keyState) {
            _scanState = GETTING_NEXT;
            return;
        }

        verify(IndexBoundsChecker::MUST_ADVANCE == keyState);
        _btreeCursor->skip(_btreeCursor->getKey(), _keyEltsToUse, _movePastKeyElts,
                           _keyElts, _keyEltsInc);

        // Must check underlying cursor EOF after every cursor movement.
        if (_btreeCursor->isEOF()) {
            _scanState = HIT_END;
        }
    }

    vector<PlanStage*> DistinctScan::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    PlanStageStats* DistinctScan::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_DISTINCT));
        ret->specific.reset(new DistinctScanStats(_specificStats));
        return ret.release();
    }

    const CommonStats* DistinctScan::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* DistinctScan::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
