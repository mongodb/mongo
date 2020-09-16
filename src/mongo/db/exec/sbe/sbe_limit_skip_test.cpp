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
 * This file contains tests for sbe::LimitSkipStage.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"

namespace mongo::sbe {

using LimitSkipStageTest = PlanStageTestFixture;

TEST_F(LimitSkipStageTest, LimitSimpleTest) {
    // Make a "limit 1000" stage.
    auto limit = makeS<LimitSkipStage>(
        makeS<CoScanStage>(kEmptyPlanNodeId), 1000, boost::none, kEmptyPlanNodeId);

    prepareTree(limit.get());

    // Verify that `limit` produces at least 1000 values.
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(limit->getNext() == PlanState::ADVANCED);
    }

    // Verify that `limit` does not produce more than 1000 values.
    ASSERT_TRUE(limit->getNext() == PlanState::IS_EOF);
}

TEST_F(LimitSkipStageTest, LimitSkipSimpleTest) {
    // Make an input array containing 64-integers 0 thru 999, inclusive.
    auto [inputTag, inputVal] = value::makeNewArray();
    value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = value::getArrayView(inputVal);
    int i;
    for (i = 0; i < 1000; ++i) {
        inputView->push_back(value::TypeTags::NumberInt64, i);
    }

    // Make a "limit 200 skip 300" stage.
    inputGuard.reset();
    auto [scanSlot, scanStage] = generateMockScan(inputTag, inputVal);
    auto limit = makeS<LimitSkipStage>(std::move(scanStage), 200, 300, kEmptyPlanNodeId);

    auto resultAccessor = prepareTree(limit.get(), scanSlot);

    // Verify that `limit` produces exactly 300 thru 499, inclusive.
    for (i = 0; i < 200; ++i) {
        ASSERT_TRUE(limit->getNext() == PlanState::ADVANCED);

        auto [tag, val] = resultAccessor->getViewOfValue();
        ASSERT_TRUE(tag == value::TypeTags::NumberInt64);
        ASSERT_TRUE(value::bitcastTo<int64_t>(val) == i + 300);
    }

    ASSERT_TRUE(limit->getNext() == PlanState::IS_EOF);
}

}  // namespace mongo::sbe
