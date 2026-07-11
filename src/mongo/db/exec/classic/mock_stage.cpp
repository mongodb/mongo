// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/mock_stage.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo {

MockStage::MockStage(ExpressionContext* expCtx, WorkingSet*) : PlanStage(kStageType, expCtx) {}

std::unique_ptr<PlanStageStats> MockStage::getStats() {
    _commonStats.isEOF = isEOF();
    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, StageType::STAGE_MOCK);
    ret->specific = std::make_unique<MockStats>(_specificStats);
    return ret;
}

PlanStage::StageState MockStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    auto nextResult = _results.front();
    _results.pop();

    auto returnState =
        visit(OverloadedVisitor{
                  [](WorkingSetID wsid) -> PlanStage::StageState { return PlanStage::ADVANCED; },
                  [](PlanStage::StageState state) -> PlanStage::StageState { return state; },
                  [](Status status) -> PlanStage::StageState {
                      uassertStatusOK(status);
                      MONGO_UNREACHABLE;
                  }},
              nextResult);
    if (returnState == PlanStage::ADVANCED) {
        *out = get<WorkingSetID>(nextResult);
    }
    return returnState;
}

}  // namespace mongo
