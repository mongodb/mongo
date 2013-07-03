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

#include "mongo/db/exec/and_sorted.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    AndSortedStage::AndSortedStage(WorkingSet* ws, Matcher* matcher)
        : _ws(ws), _matcher(matcher), _targetNode(NULL), _targetId(WorkingSet::INVALID_ID), _isEOF(false)
          { }

    AndSortedStage::~AndSortedStage() {
        for (size_t i = 0; i < _children.size(); ++i) { delete _children[i]; }
    }

    void AndSortedStage::addChild(PlanStage* child) {
        _children.push_back(child);
    }

    bool AndSortedStage::isEOF() { return _isEOF; }

    PlanStage::StageState AndSortedStage::work(WorkingSetID* out) {
        if (isEOF()) { return PlanStage::IS_EOF; }

        // If we don't have any nodes that we're work()-ing until they hit a certain DiskLoc...
        if (0 == _workingTowardRep.size()) {
            // Get a target DiskLoc.
            return getTargetLoc();
        }

        // Move nodes toward the target DiskLoc.
        // If all nodes reach the target DiskLoc, return it.  The next call to work() will set a new
        // target.
        return moveTowardTargetLoc(out);
    }

    PlanStage::StageState AndSortedStage::getTargetLoc() {
        verify(NULL == _targetNode);
        verify(WorkingSet::INVALID_ID == _targetId);
        verify(DiskLoc() == _targetLoc);

        // Pick one, and get a loc to work toward.
        WorkingSetID id;
        StageState state = _children[0]->work(&id);

        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = _ws->get(id);

            // AND only works with DiskLocs.  If we don't have a loc, something went wrong with
            // query planning.
            verify(member->hasLoc());

            // We have a value from one child to AND with.
            _targetNode = _children[0];
            _targetId = id;
            _targetLoc = member->loc;

            // We have to AND with all other children.
            for (size_t i = 1; i < _children.size(); ++i) {
                _workingTowardRep.push(_children[i]);
            }

            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == state || PlanStage::FAILURE == state) {
            _isEOF = true;
            return state;
        }
        else {
            // NEED_TIME, NEED_YIELD.
            return state;
        }
    }

    PlanStage::StageState AndSortedStage::moveTowardTargetLoc(WorkingSetID* out) {
        verify(NULL != _targetNode);
        verify(WorkingSet::INVALID_ID != _targetId);

        // We have nodes that haven't hit _targetLoc yet.
        PlanStage* next = _workingTowardRep.front();
        WorkingSetID id;
        StageState state = next->work(&id);

        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = _ws->get(id);

            verify(member->hasLoc());

            if (member->loc == _targetLoc) {
                // The front element has hit _targetLoc.  Don't move it forward anymore/work on another
                // element.
                _workingTowardRep.pop();
                AndCommon::mergeFrom(_ws->get(_targetId), member);
                _ws->free(id);

                if (0 == _workingTowardRep.size()) {
                    WorkingSetID toReturn = _targetId;
                    WorkingSetMember* toMatchTest = _ws->get(toReturn);

                    _targetNode = NULL;
                    _targetId = WorkingSet::INVALID_ID;
                    _targetLoc = DiskLoc();

                    // Everyone hit it, hooray.  Return it, if it matches.
                    if (NULL == _matcher || _matcher->matches(toMatchTest)) {
                        *out = toReturn;
                        return PlanStage::ADVANCED;
                    }
                    else {
                        _ws->free(toReturn);
                        return PlanStage::NEED_TIME;
                    }
                }
                // More children need to be advanced to _targetLoc.
                return PlanStage::NEED_TIME;
            }
            else if (member->loc < _targetLoc) {
                // The front element of _workingTowardRep hasn't hit the thing we're AND-ing with
                // yet.  Try again later.
                _ws->free(id);
                return PlanStage::NEED_TIME;
            }
            else {
                // member->loc > _targetLoc.
                // _targetLoc wasn't successfully AND-ed with the other sub-plans.  We toss it and try
                // AND-ing with the next value.
                _ws->free(_targetId);
                _targetNode = next;
                _targetLoc = member->loc;
                _targetId = id;
                _workingTowardRep = queue<PlanStage*>();
                for (size_t i = 0; i < _children.size(); ++i) {
                    if (next != _children[i]) {
                        _workingTowardRep.push(_children[i]);
                    }
                }
                // Need time to chase after the new _targetLoc.
                return PlanStage::NEED_TIME;
            }
        }
        else if (PlanStage::IS_EOF == state || PlanStage::FAILURE == state) {
            _isEOF = true;
            _ws->free(_targetId);
            return state;
        }
        else {
            // NEED_TIME, NEED_YIELD.
            return state;
        }
    }

    void AndSortedStage::prepareToYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void AndSortedStage::recoverFromYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void AndSortedStage::invalidate(const DiskLoc& dl) {
        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl);
        }

        if (dl == _targetLoc) {
            // We're in the middle of moving children forward until they hit _targetLoc, which is no
            // longer a valid target.  Fetch it, flag for review, and find another _targetLoc.
            WorkingSetCommon::fetchAndInvalidateLoc(_ws->get(_targetId));
            _ws->flagForReview(_targetId);
            _targetId = WorkingSet::INVALID_ID;
            _targetNode = NULL;
            _targetLoc = DiskLoc();
            _workingTowardRep = queue<PlanStage*>();
        }
    }

}  // namespace mongo
