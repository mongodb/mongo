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
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // static
    const char* OrStage::kStageType = "OR";

    OrStage::OrStage(WorkingSet* ws, bool dedup, const MatchExpression* filter)
        : _ws(ws), _filter(filter), _currentChild(0), _dedup(dedup), _commonStats(kStageType) { }

    OrStage::~OrStage() {
        for (size_t i = 0; i < _children.size(); ++i) {
            delete _children[i];
        }
    }

    void OrStage::addChild(PlanStage* child) { _children.push_back(child); }

    bool OrStage::isEOF() { return _currentChild >= _children.size(); }

    PlanStage::StageState OrStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        if (0 == _specificStats.matchTested.size()) {
            _specificStats.matchTested = vector<size_t>(_children.size(), 0);
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState childStatus = _children[_currentChild]->work(&id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);

            // If we're deduping (and there's something to dedup by)
            if (_dedup && member->hasLoc()) {
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
        else if (PlanStage::FAILURE == childStatus) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "OR stage failed to read in results from child " << _currentChild;
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
            return childStatus;
        }
        else if (PlanStage::NEED_TIME == childStatus) {
            ++_commonStats.needTime;
        }
        else if (PlanStage::NEED_FETCH == childStatus) {
            ++_commonStats.needFetch;
            *out = id;
        }

        // NEED_TIME, ERROR, NEED_FETCH, pass them up.
        return childStatus;
    }

    void OrStage::saveState() {
        ++_commonStats.yields;
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->saveState();
        }
    }

    void OrStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->restoreState(opCtx);
        }
    }

    void OrStage::invalidate(OperationContext* txn, const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(txn, dl, type);
        }

        // If we see DL again it is not the same record as it once was so we still want to
        // return it.
        if (_dedup && INVALIDATION_DELETION == type) {
            unordered_set<DiskLoc, DiskLoc::Hasher>::iterator it = _seen.find(dl);
            if (_seen.end() != it) {
                ++_specificStats.locsForgotten;
                _seen.erase(dl);
            }
        }
    }

    vector<PlanStage*> OrStage::getChildren() const {
        return _children;
    }

    PlanStageStats* OrStage::getStats() {
        _commonStats.isEOF = isEOF();

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_OR));
        ret->specific.reset(new OrStats(_specificStats));
        for (size_t i = 0; i < _children.size(); ++i) {
            ret->children.push_back(_children[i]->getStats());
        }

        return ret.release();
    }

    const CommonStats* OrStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* OrStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
