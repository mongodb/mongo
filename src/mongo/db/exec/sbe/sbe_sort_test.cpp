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

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/framework.h"

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
                             false,
                             nullptr /* yieldPolicy */,
                             kEmptyPlanNodeId);

        return std::make_pair(scanSlots, std::move(sortStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTestMulti(2, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

}  // namespace mongo::sbe
