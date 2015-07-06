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
                       const CountRequest& request,
                       WorkingSet* ws,
                       PlanStage* child)
    : PlanStage(kStageType),
      _txn(txn),
      _collection(collection),
      _request(request),
      _leftToSkip(request.getSkip()),
      _ws(ws) {
    if (child)
        _children.emplace_back(child);
}

bool CountStage::isEOF() {
    if (_specificStats.trivialCount) {
        return true;
    }

    if (_request.getLimit() > 0 && _specificStats.nCounted >= _request.getLimit()) {
        return true;
    }

    return !_children.empty() && child()->isEOF();
}

void CountStage::trivialCount() {
    invariant(_collection);
    long long nCounted = _collection->numRecords(_txn);

    if (0 != _request.getSkip()) {
        nCounted -= _request.getSkip();
        if (nCounted < 0) {
            nCounted = 0;
        }
    }

    long long limit = _request.getLimit();
    if (limit < 0) {
        limit = -limit;
    }

    if (limit < nCounted && 0 != limit) {
        nCounted = limit;
    }

    _specificStats.nCounted = nCounted;
    _specificStats.nSkipped = _request.getSkip();
    _specificStats.trivialCount = true;
}

PlanStage::StageState CountStage::work(WorkingSetID* out) {
    ++_commonStats.works;

    // Adds the amount of time taken by work() to executionTimeMillis.
    ScopedTimer timer(&_commonStats.executionTimeMillis);

    // This stage never returns a working set member.
    *out = WorkingSet::INVALID_ID;

    // If we don't have a query and we have a non-NULL collection, then we can execute this
    // as a trivial count (just ask the collection for how many records it has).
    if (_request.getQuery().isEmpty() && NULL != _collection) {
        trivialCount();
        return PlanStage::IS_EOF;
    }

    if (isEOF()) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    // For non-trivial counts, we should always have a child stage from which we can retrieve
    // results.
    invariant(child());
    WorkingSetID id = WorkingSet::INVALID_ID;
    PlanStage::StageState state = child()->work(&id);

    if (PlanStage::IS_EOF == state) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    } else if (PlanStage::DEAD == state) {
        return state;
    } else if (PlanStage::FAILURE == state || PlanStage::DEAD == state) {
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
        _commonStats.needYield++;
        return PlanStage::NEED_YIELD;
    }

    _commonStats.needTime++;
    return PlanStage::NEED_TIME;
}

void CountStage::doReattachToOperationContext(OperationContext* opCtx) {
    _txn = opCtx;
}

unique_ptr<PlanStageStats> CountStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_COUNT);
    ret->specific = make_unique<CountStats>(_specificStats);
    if (!_children.empty()) {
        ret->children.push_back(child()->getStats().release());
    }
    return ret;
}

const SpecificStats* CountStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
