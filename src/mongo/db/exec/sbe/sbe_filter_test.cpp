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
 * This file contains tests for sbe::FilterStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>

namespace mongo::sbe {

using FilterStageTest = PlanStageTestFixture;

TEST_F(FilterStageTest, ConstantFilterAlwaysTrueTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23)));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::copyValue(inputTag, inputVal);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a constant FilterStage whose filter expression is always boolean true.
        auto filter = makeS<FilterStage<true>>(
            std::move(scanStage),
            makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(true)),
            kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(FilterStageTest, ConstantFilterAlwaysFalseTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23)));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::makeNewArray();
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a constant FilterStage whose filter expression is always boolean false.
        auto filter = makeS<FilterStage<true>>(
            std::move(scanStage),
            makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(false)),
            kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(FilterStageTest, FilterAlwaysTrueTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23)));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::copyValue(inputTag, inputVal);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a non-constant FilterStage whose filter expression is always boolean true.
        auto filter = makeS<FilterStage<false>>(
            std::move(scanStage),
            makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(true)),
            kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(FilterStageTest, FilterAlwaysFalseTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23)));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::makeNewArray();
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a non-constant FilterStage whose filter expression is always boolean false.
        auto filter = makeS<FilterStage<false>>(
            std::move(scanStage),
            makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(false)),
            kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(FilterStageTest, FilterIsNumberTest) {
    using namespace std::literals;

    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "42" << BSON_ARRAY(2.5) << 7.5 << BSON("34" << 56)));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(12LL << 7.5));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        // Build a FilterStage whose filter expression is "isNumber(scanSlot)".
        auto filter = makeS<FilterStage<false>>(
            std::move(scanStage),
            makeE<EFunction>("isNumber"_sd, makeEs(makeE<EVariable>(scanSlot))),
            kEmptyPlanNodeId);

        return std::make_pair(scanSlot, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(FilterStageTest, FilterLessThanTest) {
    auto [inputTag, inputVal] = stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(2.8 << 3) << BSON_ARRAY(7LL << 5.0) << BSON_ARRAY(4LL << 4.3)
                             << BSON_ARRAY(8 << 8) << BSON_ARRAY("1" << 2) << BSON_ARRAY(1 << "2")
                             << BSON_ARRAY(4.9 << 5) << BSON_ARRAY(6.0 << BSON_ARRAY(11.0))));
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(2.8 << 3) << BSON_ARRAY(4LL << 4.3) << BSON_ARRAY(4.9 << 5)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        // Build a FilterStage whose filter expression is "slot0 < slot1".
        auto filter = makeS<FilterStage<false>>(std::move(scanStage),
                                                makeE<EPrimBinary>(EPrimBinary::less,
                                                                   makeE<EVariable>(scanSlots[0]),
                                                                   makeE<EVariable>(scanSlots[1])),
                                                kEmptyPlanNodeId);
        return std::make_pair(scanSlots, std::move(filter));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTestMulti(2, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

}  // namespace mongo::sbe
