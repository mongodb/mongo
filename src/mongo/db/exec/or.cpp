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

#include "mongo/db/exec/or.h"
#include "mongo/db/exec/filter.h"

namespace mongo {

    OrStage::OrStage(WorkingSet* ws, bool dedup, const MatchExpression* filter)
        : _ws(ws), _filter(filter), _currentChild(0), _dedup(dedup) { }

    OrStage::~OrStage() {
        for (size_t i = 0; i < _children.size(); ++i) {
            delete _children[i];
        }
    }

    void OrStage::addChild(PlanStage* child) { _children.push_back(child); }

    bool OrStage::isEOF() { return _currentChild >= _children.size(); }

    PlanStage::StageState OrStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        if (0 == _specificStats.matchTested.size()) {
            _specificStats.matchTested = vector<uint64_t>(_children.size(), 0);
        }

        WorkingSetID id;
        StageState childStatus = _children[_currentChild]->work(&id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);
            verify(member->hasLoc());

            // If we're deduping...
            if (_dedup) {
                ++_specificStats.dupsTested;

                // ...and we've seen the DiskLoc before
                if (_seen.end() != _seen.find(member->loc)) {
                    // ...drop it.
                    ++_specificStats.dupsDropped;
                    _ws->free(id);
                    ++_commonStats.needTime;
                    return PlanStage::NEED_TIME;
                }
                else {
                    // Otherwise, note that we've seen it.
                    _seen.insert(member->loc);
                }
            }

            if (Filter::passes(member, _filter)) {
                if (NULL != _filter) {
                    ++_specificStats.matchTested[_currentChild];
                }
                // Match!  return it.
                *out = id;
                ++_commonStats.advanced;
                return PlanStage::ADVANCED;
            }
            else {
                // Does not match, try again.
                _ws->free(id);
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }
        else if (PlanStage::IS_EOF == childStatus) {
            // Done with _currentChild, move to the next one.
            ++_currentChild;

            // Maybe we're out of children.
            if (isEOF()) {
                return PlanStage::IS_EOF;
            }
            else {
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }
        else {
            if (PlanStage::NEED_FETCH == childStatus) {
                ++_commonStats.needFetch;
            }
            else if (PlanStage::NEED_TIME == childStatus) {
                ++_commonStats.needTime;
            }

            // NEED_TIME, ERROR, NEED_YIELD, pass them up.
            return childStatus;
        }
    }

    void OrStage::prepareToYield() {
        ++_commonStats.yields;
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void OrStage::recoverFromYield() {
        ++_commonStats.unyields;
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void OrStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;

        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl);
        }

        // If we see DL again it is not the same record as it once was so we still want to
        // return it.
        if (_dedup) {
            unordered_set<DiskLoc, DiskLoc::Hasher>::iterator it = _seen.find(dl);
            if (_seen.end() != it) {
                ++_specificStats.locsForgotten;
                _seen.erase(dl);
            }
        }
    }

    PlanStageStats* OrStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->setSpecific<OrStats>(_specificStats);
        for (size_t i = 0; i < _children.size(); ++i) {
            ret->children.push_back(_children[i]->getStats());
        }

        return ret.release();
    }

}  // namespace mongo
