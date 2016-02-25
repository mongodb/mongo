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
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* OrStage::kStageType = "OR";

OrStage::OrStage(OperationContext* opCtx, WorkingSet* ws, bool dedup, const MatchExpression* filter)
    : PlanStage(kStageType, opCtx), _ws(ws), _filter(filter), _currentChild(0), _dedup(dedup) {}

void OrStage::addChild(PlanStage* child) {
    _children.emplace_back(child);
}

bool OrStage::isEOF() {
    return _currentChild >= _children.size();
}

PlanStage::StageState OrStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState childStatus = _children[_currentChild]->work(&id);

    if (PlanStage::ADVANCED == childStatus) {
        WorkingSetMember* member = _ws->get(id);

        // If we're deduping (and there's something to dedup by)
        if (_dedup && member->hasRecordId()) {
            ++_specificStats.dupsTested;

            // ...and we've seen the RecordId before
            if (_seen.end() != _seen.find(member->recordId)) {
                // ...drop it.
                ++_specificStats.dupsDropped;
                _ws->free(id);
                return PlanStage::NEED_TIME;
            } else {
                // Otherwise, note that we've seen it.
                _seen.insert(member->recordId);
            }
        }

        if (Filter::passes(member, _filter)) {
            // Match!  return it.
            *out = id;
            return PlanStage::ADVANCED;
        } else {
            // Does not match, try again.
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }
    } else if (PlanStage::IS_EOF == childStatus) {
        // Done with _currentChild, move to the next one.
        ++_currentChild;

        // Maybe we're out of children.
        if (isEOF()) {
            return PlanStage::IS_EOF;
        } else {
            return PlanStage::NEED_TIME;
        }
    } else if (PlanStage::FAILURE == childStatus || PlanStage::DEAD == childStatus) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it
        // failed, in which case 'id' is valid.  If ID is invalid, we
        // create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            mongoutils::str::stream ss;
            ss << "OR stage failed to read in results from child " << _currentChild;
            Status status(ErrorCodes::InternalError, ss);
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        }
        return childStatus;
    } else if (PlanStage::NEED_YIELD == childStatus) {
        *out = id;
    }

    // NEED_TIME, ERROR, NEED_YIELD, pass them up.
    return childStatus;
}

void OrStage::doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    // TODO remove this since calling isEOF is illegal inside of doInvalidate().
    if (isEOF()) {
        return;
    }

    // If we see DL again it is not the same record as it once was so we still want to
    // return it.
    if (_dedup && INVALIDATION_DELETION == type) {
        unordered_set<RecordId, RecordId::Hasher>::iterator it = _seen.find(dl);
        if (_seen.end() != it) {
            ++_specificStats.recordIdsForgotten;
            _seen.erase(dl);
        }
    }
}

unique_ptr<PlanStageStats> OrStage::getStats() {
    _commonStats.isEOF = isEOF();

    // Add a BSON representation of the filter to the stats tree, if there is one.
    if (NULL != _filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_OR);
    ret->specific = make_unique<OrStats>(_specificStats);
    for (size_t i = 0; i < _children.size(); ++i) {
        ret->children.emplace_back(_children[i]->getStats());
    }

    return ret;
}

const SpecificStats* OrStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
