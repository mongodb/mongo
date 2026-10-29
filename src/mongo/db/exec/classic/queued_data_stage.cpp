// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/queued_data_stage.h"

#include <memory>

namespace mongo {

using std::unique_ptr;


QueuedDataStage::QueuedDataStage(ExpressionContext* expCtx, WorkingSet*)
    : PlanStage(kStageType, expCtx) {}

PlanStage::StageState QueuedDataStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    *out = _members.front();
    _members.pop();
    return PlanStage::ADVANCED;
}

bool QueuedDataStage::isEOF() const {
    return _members.empty();
}

unique_ptr<PlanStageStats> QueuedDataStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_QUEUED_DATA);
    ret->specific = std::make_unique<MockStats>(_specificStats);
    return ret;
}


const SpecificStats* QueuedDataStage::getSpecificStats() const {
    return &_specificStats;
}

void QueuedDataStage::pushBack(const WorkingSetID& id) {
    _members.push(id);
}

}  // namespace mongo
