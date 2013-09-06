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

#include "mongo/db/exec/and_sorted.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    AndSortedStage::AndSortedStage(WorkingSet* ws, const MatchExpression* filter)
        : _ws(ws), _filter(filter), _targetNode(numeric_limits<size_t>::max()),
          _targetId(WorkingSet::INVALID_ID), _isEOF(false) { }

    AndSortedStage::~AndSortedStage() {
        for (size_t i = 0; i < _children.size(); ++i) { delete _children[i]; }
    }

    void AndSortedStage::addChild(PlanStage* child) {
        _children.push_back(child);
    }

    bool AndSortedStage::isEOF() { return _isEOF; }

    PlanStage::StageState AndSortedStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        if (0 == _specificStats.failedAnd.size()) {
            _specificStats.failedAnd.resize(_children.size());
        }

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
        verify(numeric_limits<size_t>::max() == _targetNode);
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
            _targetNode = 0;
            _targetId = id;
            _targetLoc = member->loc;

            // We have to AND with all other children.
            for (size_t i = 1; i < _children.size(); ++i) {
                _workingTowardRep.push(i);
            }

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == state || PlanStage::FAILURE == state) {
            _isEOF = true;
            return state;
        }
        else {
            if (PlanStage::NEED_FETCH == state) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == state) {
                ++_commonStats.needTime;
            }

            // NEED_TIME, NEED_YIELD.
            return state;
        }
    }

    PlanStage::StageState AndSortedStage::moveTowardTargetLoc(WorkingSetID* out) {
        verify(numeric_limits<size_t>::max() != _targetNode);
        verify(WorkingSet::INVALID_ID != _targetId);

        // We have nodes that haven't hit _targetLoc yet.
        size_t workingChildNumber = _workingTowardRep.front();
        PlanStage* next = _children[workingChildNumber];
        WorkingSetID id;
        StageState state = next->work(&id);

        if (PlanStage::ADVANCED == state) {
            WorkingSetMember* member = _ws->get(id);

            verify(member->hasLoc());

            if (member->loc == _targetLoc) {
                // The front element has hit _targetLoc.  Don't move it forward anymore/work on
                // another element.
                _workingTowardRep.pop();
                AndCommon::mergeFrom(_ws->get(_targetId), member);
                _ws->free(id);

                if (0 == _workingTowardRep.size()) {
                    WorkingSetID toReturn = _targetId;
                    WorkingSetMember* toMatchTest = _ws->get(toReturn);

                    _targetNode = numeric_limits<size_t>::max();
                    _targetId = WorkingSet::INVALID_ID;
                    _targetLoc = DiskLoc();

                    // Everyone hit it, hooray.  Return it, if it matches.
                    if (Filter::passes(toMatchTest, _filter)) {
                        if (NULL != _filter) {
                            ++_specificStats.matchTested;
                        }

                        *out = toReturn;
                        ++_commonStats.advanced;
                        return PlanStage::ADVANCED;
                    }
                    else {
                        _ws->free(toReturn);
                        ++_commonStats.needTime;
                        return PlanStage::NEED_TIME;
                    }
                }
                // More children need to be advanced to _targetLoc.
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (member->loc < _targetLoc) {
                // The front element of _workingTowardRep hasn't hit the thing we're AND-ing with
                // yet.  Try again later.
                _ws->free(id);
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else {
                // member->loc > _targetLoc.
                // _targetLoc wasn't successfully AND-ed with the other sub-plans.  We toss it and
                // try AND-ing with the next value.
                _specificStats.failedAnd[_targetNode]++;

                _ws->free(_targetId);
                _targetNode = workingChildNumber;
                _targetLoc = member->loc;
                _targetId = id;
                _workingTowardRep = queue<size_t>();
                for (size_t i = 0; i < _children.size(); ++i) {
                    if (workingChildNumber != i) {
                        _workingTowardRep.push(i);
                    }
                }
                // Need time to chase after the new _targetLoc.
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }
        else if (PlanStage::IS_EOF == state || PlanStage::FAILURE == state) {
            _isEOF = true;
            _ws->free(_targetId);
            return state;
        }
        else {
            if (PlanStage::NEED_FETCH == state) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == state) {
                ++_commonStats.needTime;
            }
            return state;
        }
    }

    void AndSortedStage::prepareToYield() {
        ++_commonStats.yields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void AndSortedStage::recoverFromYield() {
        ++_commonStats.unyields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void AndSortedStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;

        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl);
        }

        if (dl == _targetLoc) {
            // We're in the middle of moving children forward until they hit _targetLoc, which is no
            // longer a valid target.  Fetch it, flag for review, and find another _targetLoc.
            ++_specificStats.flagged;

            WorkingSetCommon::fetchAndInvalidateLoc(_ws->get(_targetId));
            _ws->flagForReview(_targetId);
            _targetId = WorkingSet::INVALID_ID;
            _targetNode = numeric_limits<size_t>::max();
            _targetLoc = DiskLoc();
            _workingTowardRep = queue<size_t>();
        }
    }

    PlanStageStats* AndSortedStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->setSpecific<AndSortedStats>(_specificStats);
        for (size_t i = 0; i < _children.size(); ++i) {
            ret->children.push_back(_children[i]->getStats());
        }

        return ret.release();
    }

}  // namespace mongo
