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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/count.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* CountStage::kStageType = "COUNT";

CountStage::CountStage(OperationContext* txn,
                       Collection* collection,
                       CountStageParams params,
                       WorkingSet* ws,
                       PlanStage* child)
    : PlanStage(kStageType, txn),
      _collection(collection),
      _params(std::move(params)),
      _leftToSkip(_params.skip),
      _ws(ws) {
    if (child)
        _children.emplace_back(child);
}

bool CountStage::isEOF() {
    if (_specificStats.recordStoreCount) {
        return true;
    }

    if (_params.limit > 0 && _specificStats.nCounted >= _params.limit) {
        return true;
    }

    return !_children.empty() && child()->isEOF();
}

void CountStage::recordStoreCount() {
    invariant(_collection);
    long long nCounted = _collection->numRecords(getOpCtx());

    if (0 != _params.skip) {
        nCounted -= _params.skip;
        if (nCounted < 0) {
            nCounted = 0;
        }
    }

    long long limit = _params.limit;
    if (limit < 0) {
        limit = -limit;
    }

    if (limit < nCounted && 0 != limit) {
        nCounted = limit;
    }

    _specificStats.nCounted = nCounted;
    _specificStats.nSkipped = _params.skip;
    _specificStats.recordStoreCount = true;
}

PlanStage::StageState CountStage::doWork(WorkingSetID* out) {
    // This stage never returns a working set member.
    *out = WorkingSet::INVALID_ID;

    if (_params.useRecordStoreCount) {
        invariant(_collection);
        recordStoreCount();
        return PlanStage::IS_EOF;
    }

    if (isEOF()) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    // For cases where we can't ask the record store directly, we should always have a child stage
    // from which we can retrieve results.
    invariant(child());
    WorkingSetID id = WorkingSet::INVALID_ID;
    PlanStage::StageState state = child()->work(&id);

    if (PlanStage::IS_EOF == state) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    } else if (PlanStage::DEAD == state) {
        return state;
    } else if (PlanStage::FAILURE == state) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it failed, in which
        // case 'id' is valid. If ID is invalid, we create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            const std::string errmsg = "count stage failed to read result from child";
            Status status = Status(ErrorCodes::InternalError, errmsg);
            *out = WorkingSetCommon::allocateStatusMember(_ws, status);
        }
        return state;
    } else if (PlanStage::ADVANCED == state) {
        // We got a result. If we're still skipping, then decrement the number left to skip.
        // Otherwise increment the count until we hit the limit.
        if (_leftToSkip > 0) {
            _leftToSkip--;
            _specificStats.nSkipped++;
        } else {
            _specificStats.nCounted++;
        }

        // Count doesn't need the actual results, so we just discard any valid working
        // set members that got returned from the child.
        if (WorkingSet::INVALID_ID != id) {
            _ws->free(id);
        }
    } else if (PlanStage::NEED_YIELD == state) {
        *out = id;
        return PlanStage::NEED_YIELD;
    }

    return PlanStage::NEED_TIME;
}

unique_ptr<PlanStageStats> CountStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_COUNT);
    ret->specific = make_unique<CountStats>(_specificStats);
    if (!_children.empty()) {
        ret->children.emplace_back(child()->getStats());
    }
    return ret;
}

const SpecificStats* CountStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
