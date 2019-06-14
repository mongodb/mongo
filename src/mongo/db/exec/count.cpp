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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/count.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* CountStage::kStageType = "COUNT";

CountStage::CountStage(OperationContext* opCtx,
                       Collection* collection,
                       long long limit,
                       long long skip,
                       WorkingSet* ws,
                       PlanStage* child)
    : PlanStage(kStageType, opCtx), _limit(limit), _skip(skip), _leftToSkip(_skip), _ws(ws) {
    invariant(_skip >= 0);
    invariant(_limit >= 0);
    invariant(child);
    _children.emplace_back(child);
}

bool CountStage::isEOF() {
    if (_limit > 0 && _specificStats.nCounted >= _limit) {
        return true;
    }

    return child()->isEOF();
}

PlanStage::StageState CountStage::doWork(WorkingSetID* out) {
    // This stage never returns a working set member.
    *out = WorkingSet::INVALID_ID;

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
    } else if (PlanStage::FAILURE == state) {
        // The stage which produces a failure is responsible for allocating a working set member
        // with error details.
        invariant(WorkingSet::INVALID_ID != id);
        *out = id;
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
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_COUNT);
    ret->specific = std::make_unique<CountStats>(_specificStats);
    if (!_children.empty()) {
        ret->children.emplace_back(child()->getStats());
    }
    return ret;
}

const SpecificStats* CountStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
