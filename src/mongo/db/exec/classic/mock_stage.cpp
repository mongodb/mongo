/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/mock_stage.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo {

MockStage::MockStage(ExpressionContext* expCtx, WorkingSet*)
    : PlanStage(kStageType.data(), expCtx) {}

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
