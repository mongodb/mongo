// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/skip.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using std::unique_ptr;


SkipStage::SkipStage(ExpressionContext* expCtx,
                     long long toSkip,
                     WorkingSet* ws,
                     std::unique_ptr<PlanStage> child)
    : PlanStage(kStageType, expCtx), _ws(ws), _leftToSkip(toSkip), _skipAmount(toSkip) {
    _children.emplace_back(std::move(child));
}

SkipStage::~SkipStage() {}

bool SkipStage::isEOF() const {
    return child()->isEOF();
}

PlanStage::StageState SkipStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    if (PlanStage::ADVANCED == status) {
        // If we're still skipping results...
        if (_leftToSkip > 0) {
            // ...drop the result.
            --_leftToSkip;
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }

        *out = id;
        return PlanStage::ADVANCED;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    // NEED_TIME, NEED_YIELD, ERROR, IS_EOF
    return status;
}

unique_ptr<PlanStageStats> SkipStage::getStats() {
    _commonStats.isEOF = isEOF();
    _specificStats.skip = _skipAmount;
    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_SKIP);
    ret->specific = std::make_unique<SkipStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* SkipStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
