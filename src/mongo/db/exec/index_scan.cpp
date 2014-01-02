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

#include "mongo/db/exec/index_scan.h"

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"

namespace {

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn(int i) {
        if (i == 0)
            return 0;
        return i > 0 ? 1 : -1;
    }

}  // namespace

namespace mongo {

    IndexScan::IndexScan(const IndexScanParams& params, WorkingSet* workingSet,
                         const MatchExpression* filter)
        : _workingSet(workingSet), _descriptor(params.descriptor), _hitEnd(false), _filter(filter), 
          _shouldDedup(params.descriptor->isMultikey()), _yieldMovedCursor(false), _params(params),
          _btreeCursor(NULL) {

        string amName;

        // If the query is using complex bounds, we must use a Btree access method, since that's the
        // only one that handles complex bounds.
        if (params.forceBtreeAccessMethod || !_params.bounds.isSimpleRange) {
            _iam = _descriptor->getIndexCatalog()->getBtreeIndex(_descriptor);
            amName = "";
        }
        else {
            amName = _descriptor->getIndexCatalog()->getAccessMethodName(_descriptor->keyPattern());
            _iam = _descriptor->getIndexCatalog()->getIndex(_descriptor);
        }

        if (IndexNames::GEO_2D == amName || IndexNames::GEO_2DSPHERE == amName) {
            // _endKey is meaningless for 2d and 2dsphere.
            verify(_params.bounds.isSimpleRange);
            verify(_params.bounds.endKey.isEmpty());
        }

        if (_params.doNotDedup) {
            _shouldDedup = false;
        }

        _specificStats.indexType = "BtreeCursor"; // TODO amName;
        _specificStats.indexName = _descriptor->infoObj()["name"].String();
        _specificStats.indexBounds = _params.bounds.toBSON();
        _specificStats.direction = _params.direction;
        _specificStats.isMultiKey = _descriptor->isMultikey();
        _specificStats.keyPattern = _descriptor->keyPattern();
    }

    void IndexScan::initIndexCursor() {
        CursorOptions cursorOptions;

        if (1 == _params.direction) {
            cursorOptions.direction = CursorOptions::INCREASING;
        }
        else {
            cursorOptions.direction = CursorOptions::DECREASING;
        }

        IndexCursor *cursor;
        Status s = _iam->newCursor(&cursor);
        verify(s.isOK());
        _indexCursor.reset(cursor);
        _indexCursor->setOptions(cursorOptions);

        if (_params.bounds.isSimpleRange) {
            // Start at one key, end at another.
            Status status = _indexCursor->seek(_params.bounds.startKey);
            if (!status.isOK()) {
                warning() << "Seek failed: " << status.toString();
                _hitEnd = true;
            }
            if (!isEOF()) {
                _specificStats.keysExamined = 1;
            }
        }
        else {
            // "Fast" Btree-specific navigation.
            _btreeCursor = static_cast<BtreeIndexCursor*>(_indexCursor.get());
            _checker.reset(new IndexBoundsChecker(&_params.bounds,
                                                  _descriptor->keyPattern(),
                                                  _params.direction));

            int nFields = _descriptor->keyPattern().nFields();
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
                _hitEnd = true;
            }
        }
    }

    PlanStage::StageState IndexScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (NULL == _indexCursor.get()) {
            // First call to work().  Perform cursor init.
            initIndexCursor();
            checkEnd();
        }
        else if (_yieldMovedCursor) {
            _yieldMovedCursor = false;
            // Note that we're not calling next() here.  We got the next thing when we recovered
            // from yielding.
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Grab the next (key, value) from the index.
        BSONObj ownedKeyObj = _indexCursor->getKey().getOwned();
        DiskLoc loc = _indexCursor->getValue();

        // Move to the next result.
        // The underlying IndexCursor points at the *next* thing we want to return.  We do this so
        // that if we're scanning an index looking for docs to delete we don't continually clobber
        // the thing we're pointing at.
        _indexCursor->next();
        checkEnd();

        if (_shouldDedup) {
            ++_specificStats.dupsTested;
            if (_returned.end() != _returned.find(loc)) {
                ++_specificStats.dupsDropped;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else {
                _returned.insert(loc);
            }
        }

        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = loc;
        member->keyData.push_back(IndexKeyDatum(_descriptor->keyPattern(), ownedKeyObj));
        member->state = WorkingSetMember::LOC_AND_IDX;

        if (Filter::passes(member, _filter)) {
            if (NULL != _filter) {
                ++_specificStats.matchTested;
            }
            if (_params.addKeyMetadata) {
                BSONObjBuilder bob;
                bob.appendKeys(_descriptor->keyPattern(), ownedKeyObj);
                member->addComputed(new IndexKeyComputedData(bob.obj()));
            }
            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }

        _workingSet->free(id);
        ++_commonStats.needTime;
        return PlanStage::NEED_TIME;
    }

    bool IndexScan::isEOF() {
        if (NULL == _indexCursor.get()) {
            // Have to call work() at least once.
            return false;
        }

        // If there's a limit on how many keys we can scan, we may be EOF when we hit that.
        if (0 != _params.maxScan) {
            if (_specificStats.keysExamined >= _params.maxScan) {
                return true;
            }
        }

        return _hitEnd || _indexCursor->isEOF();
    }

    void IndexScan::prepareToYield() {
        ++_commonStats.yields;

        if (isEOF() || (NULL == _indexCursor.get())) { return; }
        _savedKey = _indexCursor->getKey().getOwned();
        _savedLoc = _indexCursor->getValue();
        _indexCursor->savePosition();
    }

    void IndexScan::recoverFromYield() {
        ++_commonStats.unyields;

        if (isEOF() || (NULL == _indexCursor.get())) { return; }

        // We can have a valid position before we check isEOF(), restore the position, and then be
        // EOF upon restore.
        if (!_indexCursor->restorePosition().isOK() || _indexCursor->isEOF()) {
            _hitEnd = true;
            return;
        }

        if (!_savedKey.binaryEqual(_indexCursor->getKey())
            || _savedLoc != _indexCursor->getValue()) {
            // Our restored position isn't the same as the saved position.  When we call work()
            // again we want to return where we currently point, not past it.
            _yieldMovedCursor = true;

            ++_specificStats.yieldMovedCursor;

            // Our restored position might be past endKey, see if we've hit the end.
            checkEnd();
        }
    }

    void IndexScan::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;

        // If we see this DiskLoc again, it may not be the same doc. it was before, so we want to
        // return it.
        unordered_set<DiskLoc, DiskLoc::Hasher>::iterator it = _returned.find(dl);
        if (it != _returned.end()) {
            ++_specificStats.seenInvalidated;
            _returned.erase(it);
        }
    }

    void IndexScan::checkEnd() {
        if (isEOF()) {
            _commonStats.isEOF = true;
            return;
        }

        if (_params.bounds.isSimpleRange) {
            // "Normal" start -> end scanning.
            verify(NULL == _btreeCursor);
            verify(NULL == _checker.get());

            // If there is an empty endKey we will scan until we run out of index to scan over.
            if (_params.bounds.endKey.isEmpty()) { return; }

            int cmp = sgn(_params.bounds.endKey.woCompare(_indexCursor->getKey(),
                _descriptor->keyPattern()));

            if ((cmp != 0 && cmp != _params.direction)
                || (cmp == 0 && !_params.bounds.endKeyInclusive)) {

                _hitEnd = true;
                _commonStats.isEOF = true;
            }

            if (!isEOF() && _params.bounds.isSimpleRange) {
                ++_specificStats.keysExamined;
            }
        }
        else {
            verify(NULL != _btreeCursor);
            verify(NULL != _checker.get());

            // Use _checker to see how things are.
            for (;;) {
                //cout << "current index key is " << _indexCursor->getKey().toString() << endl;
                //cout << "keysExamined is " << _specificStats.keysExamined << endl;
                IndexBoundsChecker::KeyState keyState;
                keyState = _checker->checkKey(_indexCursor->getKey(),
                                              &_keyEltsToUse,
                                              &_movePastKeyElts,
                                              &_keyElts,
                                              &_keyEltsInc);

                if (IndexBoundsChecker::DONE == keyState) {
                    _hitEnd = true;
                    break;
                }

                // This seems weird but it's the old definition of nscanned.
                ++_specificStats.keysExamined;

                if (IndexBoundsChecker::VALID == keyState) {
                    break;
                }

                //cout << "skipping...\n";
                verify(IndexBoundsChecker::MUST_ADVANCE == keyState);
                _btreeCursor->skip(_indexCursor->getKey(), _keyEltsToUse, _movePastKeyElts,
                                   _keyElts, _keyEltsInc);

                // Must check underlying cursor EOF after every cursor movement.
                if (_btreeCursor->isEOF()) {
                    _hitEnd = true;
                    break;
                }

                // TODO: Can we do too much scanning here?  Old BtreeCursor stops scanning after a
                // while and relies on a Matcher to make sure the result is ok.
            }
        }
    }

    PlanStageStats* IndexScan::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_IXSCAN));
        ret->specific.reset(new IndexScanStats(_specificStats));
        return ret.release();
    }

}  // namespace mongo
