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
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

using SortStageTest = PlanStageTestFixture;

TEST_F(SortStageTest, SortNumbersTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(12LL << "A") << BSON_ARRAY(2.5 << "B") << BSON_ARRAY(7 << "C")
                                           << BSON_ARRAY(Decimal128(4) << "D")));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(2.5 << "B") << BSON_ARRAY(Decimal128(4) << "D")
                                          << BSON_ARRAY(7 << "C") << BSON_ARRAY(12LL << "A")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        // Create a SortStage that sorts by slot0 in ascending order.
        auto sortStage =
            makeS<SortStage>(std::move(scanStage),
                             makeSV(scanSlots[0]),
                             std::vector<value::SortDirection>{value::SortDirection::Ascending},
                             makeSV(scanSlots[1]),
                             makeE<EConstant>(value::TypeTags::NumberInt64,
                                              value::bitcastFrom<int64_t>(4)) /*limit*/,
                             204857600,
                             false /* allowDiskUse */,
                             nullptr /* yieldPolicy */,
                             kEmptyPlanNodeId);

        return std::make_pair(scanSlots, std::move(sortStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTestMulti(2, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

std::pair<std::pair<sbe::value::TypeTags, sbe::value::Value>,
          std::pair<sbe::value::TypeTags, sbe::value::Value>>
makeExpectedAndInputDataForSpillingTest() {
    std::string string512KB(512 * 1024, 'x');
    std::vector<BSONArray> data;
    for (size_t i = 0; i < 26; ++i) {
        std::string suffix;
        suffix.push_back('a' + i);
        data.push_back(BSON_ARRAY((string512KB + suffix) << suffix));
    }

    BSONArrayBuilder expectedBuilder;
    for (const auto& e : data) {
        expectedBuilder.append(e);
    }
    auto expected = stage_builder::makeValue(expectedBuilder.arr());
    value::ValueGuard expectedGuard{expected.first, expected.second};

    std::reverse(data.begin(), data.end());

    BSONArrayBuilder inputBuilder;
    for (const auto& e : data) {
        inputBuilder.append(e);
    }
    auto input = stage_builder::makeValue(inputBuilder.arr());
    value::ValueGuard inputGuard{input.first, input.second};

    expectedGuard.reset();
    inputGuard.reset();
    return {expected, input};
}

TEST_F(SortStageTest, SortStringsWithSpillingTest) {
    auto [expected, input] = makeExpectedAndInputDataForSpillingTest();
    value::ValueGuard expectedGuard{expected.first, expected.second};
    value::ValueGuard inputGuard{input.first, input.second};

    auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        auto sortStage =
            makeS<SortStage>(std::move(scanStage),
                             makeSV(scanSlots[0]),
                             std::vector<value::SortDirection>{value::SortDirection::Ascending},
                             makeSV(scanSlots[1]),
                             nullptr /*limit*/,
                             10 * 1024 * 1024 /* memoryLimit */,
                             true /* allowDiskUse */,
                             nullptr /* yieldPolicy */,
                             kEmptyPlanNodeId);

        return std::make_pair(scanSlots, std::move(sortStage));
    };

    auto assertStageStats = [](const SpecificStats* stats) {
        const auto* sortStats = dynamic_cast<const SortStats*>(stats);
        ASSERT_NE(sortStats, nullptr);
        ASSERT_EQ(sortStats->spillingStats.getSpills(), 2);
        ASSERT_EQ(sortStats->spillingStats.getSpilledRecords(), 26);
        ASSERT_EQ(sortStats->spillingStats.getSpilledBytes(), 13632138);
        ASSERT_GT(sortStats->spillingStats.getSpilledDataStorageSize(), 0);
        ASSERT_LT(sortStats->spillingStats.getSpilledDataStorageSize(),
                  sortStats->spillingStats.getSpilledBytes());
        ASSERT_GT(sortStats->peakTrackedMemBytes, 0);
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTestMulti(2,
                 input.first,
                 input.second,
                 expected.first,
                 expected.second,
                 makeStageFn,
                 false,
                 assertStageStats);
}

TEST_F(SortStageTest, SortStringsWithForceSpillingTest) {
    auto [expected, input] = makeExpectedAndInputDataForSpillingTest();
    value::ValueGuard expectedGuard{expected.first, expected.second};
    value::ValueGuard inputGuard{input.first, input.second};

    auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        auto sortStage =
            makeS<SortStage>(std::move(scanStage),
                             makeSV(scanSlots[0]),
                             std::vector<value::SortDirection>{value::SortDirection::Ascending},
                             makeSV(scanSlots[1]),
                             nullptr /*limit*/,
                             100 * 1024 * 1024 /* memoryLimit */,
                             true /* allowDiskUse */,
                             nullptr /* yieldPolicy */,
                             kEmptyPlanNodeId);

        return std::make_pair(scanSlots, std::move(sortStage));
    };

    auto assertStageStats = [](const SpecificStats* stats) {
        const auto* sortStats = dynamic_cast<const SortStats*>(stats);
        ASSERT_NE(sortStats, nullptr);
        ASSERT_EQ(sortStats->spillingStats.getSpills(), 1);
        ASSERT_EQ(sortStats->spillingStats.getSpilledRecords(), 23);
        ASSERT_EQ(sortStats->spillingStats.getSpilledBytes(), 12059199);
        ASSERT_GT(sortStats->spillingStats.getSpilledDataStorageSize(), 0);
        ASSERT_LT(sortStats->spillingStats.getSpilledDataStorageSize(),
                  sortStats->spillingStats.getSpilledBytes());
        ASSERT_GT(sortStats->peakTrackedMemBytes, 0);
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTestMulti(2,
                 input.first,
                 input.second,
                 expected.first,
                 expected.second,
                 makeStageFn,
                 true,
                 assertStageStats);
}

}  // namespace mongo::sbe
