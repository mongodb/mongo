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

#include "mongo/db/exec/and_hash.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    AndHashStage::AndHashStage(WorkingSet* ws, const MatchExpression* filter)
        : _ws(ws), _filter(filter), _resultIterator(_dataMap.end()),
          _shouldScanChildren(true), _currentChild(0) {}

    AndHashStage::~AndHashStage() {
        for (size_t i = 0; i < _children.size(); ++i) { delete _children[i]; }
    }

    void AndHashStage::addChild(PlanStage* child) { _children.push_back(child); }

    bool AndHashStage::isEOF() {
        if (_shouldScanChildren) { return false; }
        return _dataMap.end() == _resultIterator;
    }

    PlanStage::StageState AndHashStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        // An AND is either reading the first child into the hash table, probing against the hash
        // table with subsequent children, or returning results.

        // We read the first child into our hash table.
        if (_shouldScanChildren && (0 == _currentChild)) {
            return readFirstChild();
        }

        // Probing into our hash table with other children.
        if (_shouldScanChildren) {
            return hashOtherChildren();
        }

        // Returning results.
        verify(!_shouldScanChildren);

        // Keep the thing we're returning so we can remove it from our internal map later.
        DataMap::iterator returnedIt = _resultIterator;
        ++_resultIterator;

        WorkingSetID idToReturn = returnedIt->second;
        _dataMap.erase(returnedIt);
        WorkingSetMember* member = _ws->get(idToReturn);

        // We should check for matching at the end so the matcher can use information in the
        // indices of all our children.
        if (Filter::passes(member, _filter)) {
            *out = idToReturn;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            _ws->free(idToReturn);
            // Skip over the non-matching thing we currently point at.
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
    }

    PlanStage::StageState AndHashStage::readFirstChild() {
        verify(_currentChild == 0);

        WorkingSetID id;
        StageState childStatus = _children[0]->work(&id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);

            verify(member->hasLoc());
            verify(_dataMap.end() == _dataMap.find(member->loc));

            _dataMap[member->loc] = id;
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == childStatus) {
            // Done reading child 0.
            _currentChild = 1;

            // If our first child was empty, don't scan any others, no possible results.
            if (_dataMap.empty()) {
                _shouldScanChildren = false;
                return PlanStage::IS_EOF;
            }

            ++_commonStats.needTime;
            _specificStats.mapAfterChild.push_back(_dataMap.size());

            return PlanStage::NEED_TIME;
        }
        else {
            if (PlanStage::NEED_FETCH == childStatus) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == childStatus) {
                ++_commonStats.needTime;
            }

            return childStatus;
        }
    }

    PlanStage::StageState AndHashStage::hashOtherChildren() {
        verify(_currentChild > 0);

        WorkingSetID id;
        StageState childStatus = _children[_currentChild]->work(&id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);
            verify(member->hasLoc());
            if (_dataMap.end() == _dataMap.find(member->loc)) {
                // Ignore.  It's not in any previous child.
            }
            else {
                // We have a hit.  Copy data into the WSM we already have.
                _seenMap.insert(member->loc);
                WorkingSetMember* olderMember = _ws->get(_dataMap[member->loc]);
                AndCommon::mergeFrom(olderMember, member);
            }
            _ws->free(id);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == childStatus) {
            // Finished with a child.
            ++_currentChild;

            // Keep elements of _dataMap that are in _seenMap.
            DataMap::iterator it = _dataMap.begin();
            while (it != _dataMap.end()) {
                if (_seenMap.end() == _seenMap.find(it->first)) {
                    DataMap::iterator toErase = it;
                    ++it;
                    _ws->free(toErase->second);
                    _dataMap.erase(toErase);
                }
                else { ++it; }
            }

            _specificStats.mapAfterChild.push_back(_dataMap.size());

            _seenMap.clear();

            // _dataMap is now the intersection of the first _currentChild nodes.

            // If we have nothing to AND with after finishing any child, stop.
            if (_dataMap.empty()) {
                _shouldScanChildren = false;
                return PlanStage::IS_EOF;
            }

            // We've finished scanning all children.  Return results with the next call to work().
            if (_currentChild == _children.size()) {
                _shouldScanChildren = false;
                _resultIterator = _dataMap.begin();
            }

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else {
            if (PlanStage::NEED_FETCH == childStatus) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == childStatus) {
                ++_commonStats.needTime;
            }

            return childStatus;
        }
    }

    void AndHashStage::prepareToYield() {
        ++_commonStats.yields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void AndHashStage::recoverFromYield() {
        ++_commonStats.unyields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void AndHashStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;

        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl);
        }

        _seenMap.erase(dl);

        // If we're pointing at the DiskLoc, move past it.  It will be deleted.
        if (_dataMap.end() != _resultIterator && (_resultIterator->first == dl)) {
            ++_resultIterator;
        }

        DataMap::iterator it = _dataMap.find(dl);
        if (_dataMap.end() != it) {
            WorkingSetID id = it->second;
            WorkingSetMember* member = _ws->get(id);
            verify(member->loc == dl);

            if (_shouldScanChildren) {
                ++_specificStats.flaggedInProgress;
            }
            else {
                ++_specificStats.flaggedButPassed;
            }

            // The loc is about to be invalidated.  Fetch it and clear the loc.
            WorkingSetCommon::fetchAndInvalidateLoc(member);

            // Add the WSID to the to-be-reviewed list in the WS.
            _ws->flagForReview(id);
            _dataMap.erase(it);
        }
    }

    PlanStageStats* AndHashStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->setSpecific<AndHashStats>(_specificStats);
        for (size_t i = 0; i < _children.size(); ++i) {
            ret->children.push_back(_children[i]->getStats());
        }

        return ret.release();
    }

}  // namespace mongo
