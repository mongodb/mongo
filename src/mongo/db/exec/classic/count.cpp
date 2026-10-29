// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/count.h"

#include "mongo/util/assert_util.h"

#include <memory>
#include <vector>

namespace mongo {

using std::unique_ptr;


CountStage::CountStage(
    ExpressionContext* expCtx, long long limit, long long skip, WorkingSet* ws, PlanStage* child)
    : PlanStage(kStageType, expCtx), _limit(limit), _skip(skip), _leftToSkip(_skip), _ws(ws) {
    tassert(11051653, "Expecting non-negative skip parameter", _skip >= 0);
    tassert(11051652, "Expecting non-negative limit parameter", _limit >= 0);
    tassert(11051651, "Expecting child stage", child);
    _children.emplace_back(child);
}

bool CountStage::isEOF() const {
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
    WorkingSetID id = WorkingSet::INVALID_ID;
    PlanStage::StageState state = child()->work(&id);

    if (PlanStage::IS_EOF == state) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
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
