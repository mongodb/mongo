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

#include "mongo/db/exec/or.h"

namespace mongo {

    OrStage::OrStage(WorkingSet* ws, bool dedup, Matcher* matcher)
        : _ws(ws), _matcher(matcher), _currentChild(0), _dedup(dedup) { }

    OrStage::~OrStage() {
        for (size_t i = 0; i < _children.size(); ++i) {
            delete _children[i];
        }
    }

    void OrStage::addChild(PlanStage* child) { _children.push_back(child); }

    bool OrStage::isEOF() { return _currentChild >= _children.size(); }

    PlanStage::StageState OrStage::work(WorkingSetID* out) {
        if (isEOF()) { return PlanStage::IS_EOF; }

        WorkingSetID id;
        StageState childStatus = _children[_currentChild]->work(&id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);
            verify(member->hasLoc());

            // If we're deduping...
            if (_dedup) {
                // ...and we've seen the DiskLoc before
                if (_seen.end() != _seen.find(member->loc)) {
                    // ...drop it.
                    _ws->free(id);
                    return PlanStage::NEED_TIME;
                }
                else {
                    // Otherwise, note that we've seen it.
                    _seen.insert(member->loc);
                }
            }

            if (NULL == _matcher || _matcher->matches(member)) {
                // Match!  return it.
                *out = id;
                return PlanStage::ADVANCED;
            }
            else {
                // Does not match, try again.
                _ws->free(id);
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
                return PlanStage::NEED_TIME;
            }
        }
        else {
            // NEED_TIME, ERROR, NEED_YIELD, pass them up.
            return childStatus;
        }
    }

    void OrStage::prepareToYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->prepareToYield();
        }
    }

    void OrStage::recoverFromYield() {
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->recoverFromYield();
        }
    }

    void OrStage::invalidate(const DiskLoc& dl) {
        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl);
        }

        // If we see DL again it is not the same record as it once was so we still want to
        // return it.
        if (_dedup) { _seen.erase(dl); }
    }

}  // namespace mongo
