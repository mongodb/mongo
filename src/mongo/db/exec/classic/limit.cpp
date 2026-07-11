// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/limit.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using std::unique_ptr;


LimitStage::LimitStage(ExpressionContext* expCtx,
                       long long limit,
                       WorkingSet*,
                       std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, expCtx), _numToReturn(limit) {
    _specificStats.limit = _numToReturn;
    _children.emplace_back(std::move(child));
}

LimitStage::~LimitStage() {}

bool LimitStage::isEOF() const {
    return (0 == _numToReturn) || child()->isEOF();
}

PlanStage::StageState LimitStage::doWork(WorkingSetID* out) {
    if (0 == _numToReturn) {
        // We've returned as many results as we're limited to.
        return PlanStage::IS_EOF;
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        *out = id;
        --_numToReturn;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

unique_ptr<PlanStageStats> LimitStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_LIMIT);
    ret->specific = std::make_unique<LimitStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* LimitStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
