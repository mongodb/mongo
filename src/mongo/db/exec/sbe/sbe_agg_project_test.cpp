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

/**
 * This file contains tests for sbe::AggProjectStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

using AggProjectStageTest = PlanStageTestFixture;

TEST_F(AggProjectStageTest, SimpleCountTest) {
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] =
        stage_builder::makeValue(BSON_ARRAY(1LL << 2LL << 3LL << 4LL << 5LL));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(outSlot,
                              nullptr,
                              makeFunction("sum",
                                           makeE<EConstant>(value::TypeTags::NumberInt64,
                                                            value::bitcastFrom<int64_t>(1)))),
            kEmptyPlanNodeId);

        return std::make_pair(outSlot, std::move(aggProject));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(AggProjectStageTest, SimpleSumTest) {
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] =
        stage_builder::makeValue(BSON_ARRAY(10LL << 30LL << 60LL << 100LL << 150LL));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(outSlot, nullptr, makeFunction("sum", makeVariable(scanSlot))),
            kEmptyPlanNodeId);
        return std::make_pair(outSlot, std::move(aggProject));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(AggProjectStageTest, SumWithInitTest) {
    auto [inputTag, inputVal] =
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] =
        stage_builder::makeValue(BSON_ARRAY(110LL << 130LL << 160LL << 200LL << 250LL));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(
                outSlot,
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(100)),
                makeFunction("sum", makeVariable(scanSlot))),
            kEmptyPlanNodeId);
        return std::make_pair(outSlot, std::move(aggProject));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

}  // namespace mongo::sbe
