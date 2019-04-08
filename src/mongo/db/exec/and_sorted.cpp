/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/and_sorted.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/str.h"

namespace mongo {

using std::unique_ptr;
using std::numeric_limits;
using std::vector;
using stdx::make_unique;

// static
const char* AndSortedStage::kStageType = "AND_SORTED";

AndSortedStage::AndSortedStage(OperationContext* opCtx, WorkingSet* ws)
    : PlanStage(kStageType, opCtx),
      _ws(ws),
      _targetNode(numeric_limits<size_t>::max()),
      _targetId(WorkingSet::INVALID_ID),
      _isEOF(false) {}


void AndSortedStage::addChild(PlanStage* child) {
    _children.emplace_back(child);
}

bool AndSortedStage::isEOF() {
    return _isEOF;
}

PlanStage::StageState AndSortedStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (0 == _specificStats.failedAnd.size()) {
        _specificStats.failedAnd.resize(_children.size());
    }

    // If we don't have any nodes that we're work()-ing until they hit a certain RecordId...
    if (0 == _workingTowardRep.size()) {
        // Get a target RecordId.
        return getTargetRecordId(out);
    }

    // Move nodes toward the target RecordId.
    // If all nodes reach the target RecordId, return it.  The next call to work() will set a new
    // target.
    return moveTowardTargetRecordId(out);
}

PlanStage::StageState AndSortedStage::getTargetRecordId(WorkingSetID* out) {
    verify(numeric_limits<size_t>::max() == _targetNode);
    verify(WorkingSet::INVALID_ID == _targetId);
    verify(RecordId() == _targetRecordId);

    // Pick one, and get a RecordId to work toward.
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState state = _children[0]->work(&id);

    if (PlanStage::ADVANCED == state) {
        WorkingSetMember* member = _ws->get(id);

        // The child must give us a WorkingSetMember with a record id, since we intersect index keys
        // based on the record id. The planner ensures that the child stage can never produce an WSM
        // with no record id.
        invariant(member->hasRecordId());

        // We have a value from one child to AND with.
        _targetNode = 0;
        _targetId = id;
        _targetRecordId = member->recordId;

        // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
        member->makeObjOwnedIfNeeded();

        // We have to AND with all other children.
        for (size_t i = 1; i < _children.size(); ++i) {
            _workingTowardRep.push(i);
        }

        return PlanStage::NEED_TIME;
    } else if (PlanStage::IS_EOF == state) {
        _isEOF = true;
        return state;
    } else if (PlanStage::FAILURE == state) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it
        // failed, in which case 'id' is valid.  If ID is invalid, we
        // create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            str::stream ss;
            ss << "sorted AND stage failed to read in results from first child";
            Status status(ErrorCodes::InternalError, ss);
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        }
        _isEOF = true;
        return state;
    } else {
        if (PlanStage::NEED_YIELD == state) {
            *out = id;
        }

        // NEED_TIME, NEED_YIELD.
        return state;
    }
}

PlanStage::StageState AndSortedStage::moveTowardTargetRecordId(WorkingSetID* out) {
    verify(numeric_limits<size_t>::max() != _targetNode);
    verify(WorkingSet::INVALID_ID != _targetId);

    // We have nodes that haven't hit _targetRecordId yet.
    size_t workingChildNumber = _workingTowardRep.front();
    auto& next = _children[workingChildNumber];
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState state = next->work(&id);

    if (PlanStage::ADVANCED == state) {
        WorkingSetMember* member = _ws->get(id);

        // The child must give us a WorkingSetMember with a record id, since we intersect index keys
        // based on the record id. The planner ensures that the child stage can never produce an WSM
        // with no record id.
        invariant(member->hasRecordId());

        if (member->recordId == _targetRecordId) {
            // The front element has hit _targetRecordId.  Don't move it forward anymore/work on
            // another element.
            _workingTowardRep.pop();
            AndCommon::mergeFrom(_ws, _targetId, *member);
            _ws->free(id);

            if (0 == _workingTowardRep.size()) {
                WorkingSetID toReturn = _targetId;

                _targetNode = numeric_limits<size_t>::max();
                _targetId = WorkingSet::INVALID_ID;
                _targetRecordId = RecordId();

                *out = toReturn;
                return PlanStage::ADVANCED;
            }
            // More children need to be advanced to _targetRecordId.
            return PlanStage::NEED_TIME;
        } else if (member->recordId < _targetRecordId) {
            // The front element of _workingTowardRep hasn't hit the thing we're AND-ing with
            // yet.  Try again later.
            _ws->free(id);
            return PlanStage::NEED_TIME;
        } else {
            // member->recordId > _targetRecordId.
            // _targetRecordId wasn't successfully AND-ed with the other sub-plans.  We toss it and
            // try AND-ing with the next value.
            _specificStats.failedAnd[_targetNode]++;

            _ws->free(_targetId);
            _targetNode = workingChildNumber;
            _targetRecordId = member->recordId;
            _targetId = id;

            // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
            member->makeObjOwnedIfNeeded();

            _workingTowardRep = std::queue<size_t>();
            for (size_t i = 0; i < _children.size(); ++i) {
                if (workingChildNumber != i) {
                    _workingTowardRep.push(i);
                }
            }
            // Need time to chase after the new _targetRecordId.
            return PlanStage::NEED_TIME;
        }
    } else if (PlanStage::IS_EOF == state) {
        _isEOF = true;
        _ws->free(_targetId);
        return state;
    } else if (PlanStage::FAILURE == state) {
        // The stage which produces a failure is responsible for allocating a working set member
        // with error details.
        invariant(WorkingSet::INVALID_ID != id);
        *out = id;
        _isEOF = true;
        _ws->free(_targetId);
        return state;
    } else {
        if (PlanStage::NEED_YIELD == state) {
            *out = id;
        }

        return state;
    }
}

unique_ptr<PlanStageStats> AndSortedStage::getStats() {
    _commonStats.isEOF = isEOF();

    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_AND_SORTED);
    ret->specific = make_unique<AndSortedStats>(_specificStats);
    for (size_t i = 0; i < _children.size(); ++i) {
        ret->children.emplace_back(_children[i]->getStats());
    }

    return ret;
}

const SpecificStats* AndSortedStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
