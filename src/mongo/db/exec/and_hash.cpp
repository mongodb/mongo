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
 */

#include "mongo/db/exec/and_hash.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    AndHashStage::AndHashStage(WorkingSet* ws, Matcher* matcher)
        : _ws(ws), _matcher(matcher), _resultIterator(_dataMap.end()),
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
        if (NULL == _matcher || _matcher->matches(member)) {
            *out = idToReturn;
            return PlanStage::ADVANCED;
        }
        else {
            _ws->free(idToReturn);
            // Skip over the non-matching thing we currently point at.
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
            return PlanStage::NEED_TIME;
        }
        else {
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

            return PlanStage::NEED_TIME;
        }
        else {
            // NEED_YIELD or FAILURE.
            return childStatus;
        }
    }

    void AndHashStage::prepareToYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void AndHashStage::recoverFromYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void AndHashStage::invalidate(const DiskLoc& dl) {
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

            // The loc is about to be invalidated.  Fetch it and clear the loc.
            WorkingSetCommon::fetchAndInvalidateLoc(member);

            // Add the WSID to the to-be-reviewed list in the WS.
            _ws->flagForReview(id);
            _dataMap.erase(it);
        }
    }

}  // namespace mongo
