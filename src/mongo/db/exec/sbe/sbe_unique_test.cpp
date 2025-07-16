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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
/**
 * This file contains tests for sbe::UniqueStage and sbe::UniqueRoaringStage.
 */

using UniqueStageTest = PlanStageTestFixture;
using UniqueRoaringStageTest = PlanStageTestFixture;

TEST_F(UniqueStageTest, DeduplicatesAndPreservesOrderSimple) {
    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 1));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique =
            makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueRoaringStageTest, DeduplicatesAndPreservesOrderSimple) {
    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3 << 1));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueRoaringStageTest, DeduplicatesAndPreservesOrderSimpleRecordId) {
    auto createRecordIdArray =
        [](std::vector<int64_t> rids) -> std::pair<value::TypeTags, value::Value> {
        auto [arrTag, arrVal] = value::makeNewArray();
        auto arr = value::getArrayView(arrVal);
        for (auto rid : rids) {
            auto [ridTag, ridVal] = value::makeNewRecordId(rid);
            arr->push_back(ridTag, ridVal);
        }
        return {arrTag, arrVal};
    };

    auto [inputTag, inputVal] = createRecordIdArray({1, 2, 3, 1, 4});
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = createRecordIdArray({1, 2, 3, 4});
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(unique));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(UniqueStageTest, DeduplicatesMultipleSlotsInKey) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlots, scan] = generateVirtualScanMulti(2,  // numSlots
                                                      tag,
                                                      val);

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto unique = makeS<UniqueStage>(std::move(scan), scanSlots, kEmptyPlanNodeId);

    auto ctx = makeCompileCtx();
    auto resultAccessors = prepareTree(ctx.get(), unique.get(), scanSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(unique.get(), resultAccessors);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));
}

TEST_F(UniqueStageTest, ResetsStateAfterClose) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlot, scanStage] = generateVirtualScan(tag, val);

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto unique = makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    auto [resultsTag, resultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));

    // Closing and opening the plan should have the effect of clearing the values that 'unique'
    // has seen.
    unique->close();
    unique->open(false);

    auto [resetResultsTag, resetResultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resetResultGuard{resetResultsTag, resetResultsVal};

    // The same result is seen again after closing and opening the plan tree. This proves the
    // seen set has been cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsTag, resetResultsVal, expectedTag, expectedVal));
}

TEST_F(UniqueRoaringStageTest, ResetsStateAfterClose) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 1 << 3));
    auto [scanSlot, scanStage] = generateVirtualScan(tag, val);

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    auto [resultsTag, resultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));

    // Closing and opening the plan should have the effect of clearing the values that 'unique'
    // has seen.
    unique->close();
    unique->open(false);

    auto [resetResultsTag, resetResultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resetResultGuard{resetResultsTag, resetResultsVal};

    // The same result is seen again after closing and opening the plan tree. This proves the
    // seen set has been cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsTag, resetResultsVal, expectedTag, expectedVal));
}

TEST_F(UniqueStageTest, ResetsStateAfterReopen) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(1 << 1) << BSON_ARRAY(3 << 3)));
    auto [scanSlot, scanStage] = generateVirtualScan(tag, val);

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 1) << BSON_ARRAY(2 << 2) << BSON_ARRAY(3 << 3)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto unique = makeS<UniqueStage>(std::move(scanStage), sbe::makeSV(scanSlot), kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    auto [resultsTag, resultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));

    // Calling open with reOpen set to 'true' should have the effect of clearing the values that
    // 'unique' has seen.
    unique->open(/* reOpen */ true);
    auto [resetResultsTag, resetResultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resetResultGuard{resetResultsTag, resetResultsVal};

    // The same result is seen after re-opening the plan tree. This proves the seen set has been
    // cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsTag, resetResultsVal, expectedTag, expectedVal));
}

TEST_F(UniqueRoaringStageTest, ResetsStateAfterReopen) {
    auto [tag, val] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 1 << 3));
    auto [scanSlot, scanStage] = generateVirtualScan(tag, val);

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto unique = makeS<UniqueRoaringStage>(std::move(scanStage), scanSlot, kEmptyPlanNodeId);
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), unique.get(), scanSlot);

    auto [resultsTag, resultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    ASSERT_TRUE(valueEquals(resultsTag, resultsVal, expectedTag, expectedVal));

    // Calling open with reOpen set to 'true' should have the effect of clearing the values that
    // 'unique' has seen.
    unique->open(/* reOpen */ true);
    auto [resetResultsTag, resetResultsVal] = getAllResults(unique.get(), resultAccessor);
    value::ValueGuard resetResultGuard{resetResultsTag, resetResultsVal};

    // The same result is seen after re-opening the plan tree. This proves the seen set has been
    // cleared, otherwise we would not get any result from the second run.
    ASSERT_TRUE(valueEquals(resetResultsTag, resetResultsVal, expectedTag, expectedVal));
}
}  // namespace mongo::sbe
