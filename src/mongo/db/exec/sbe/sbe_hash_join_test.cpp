/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
 * This file contains tests for sbe::HashJoinStage.
 */

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

using HashJoinStageTest = PlanStageTestFixture;

/**
 * Helper function to verify that actual results contain all expected results (order-independent).
 * Each result is an array of [innerKey, outerKey, innerProj, outerProj].
 */
void assertResultsMatch(value::TypeTags actualTag,
                        value::Value actualVal,
                        value::TypeTags expectedTag,
                        value::Value expectedVal) {
    ASSERT_EQ(actualTag, value::TypeTags::Array);
    ASSERT_EQ(expectedTag, value::TypeTags::Array);

    auto actualView = value::getArrayView(actualVal);
    auto expectedView = value::getArrayView(expectedVal);

    ASSERT_EQ(actualView->size(), expectedView->size())
        << "Result size mismatch: got " << actualView->size() << ", expected "
        << expectedView->size();

    // For each expected result, verify it exists in actual results
    for (size_t i = 0; i < expectedView->size(); i++) {
        auto [expectedRowTag, expectedRowVal] = expectedView->getAt(i);
        bool found = false;

        for (size_t j = 0; j < actualView->size(); j++) {
            auto [actualRowTag, actualRowVal] = actualView->getAt(j);
            auto [cmpTag, cmpVal] =
                value::compareValue(expectedRowTag, expectedRowVal, actualRowTag, actualRowVal);
            if (cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0) {
                found = true;
                break;
            }
        }

        ASSERT_TRUE(found) << "Expected result not found at index " << i;
    }
}

/**
 * Helper function to create a HashJoinStage for testing.
 * This sets up a hash join where the inner side builds the hash table and
 * the outer side probes it.
 */
std::pair<value::SlotVector, std::unique_ptr<PlanStage>> makeHashJoinStage(
    PlanStageTestFixture* fixture,
    std::unique_ptr<PlanStage> outerStage,
    std::unique_ptr<PlanStage> innerStage,
    value::SlotVector outerCondSlots,
    value::SlotVector outerProjectSlots,
    value::SlotVector innerCondSlots,
    value::SlotVector innerProjectSlots,
    boost::optional<value::SlotId> collatorSlot = boost::none) {

    // Output slots are the condition and project slots from both sides
    value::SlotVector outputSlots;
    outputSlots.insert(outputSlots.end(), innerCondSlots.begin(), innerCondSlots.end());
    outputSlots.insert(outputSlots.end(), outerCondSlots.begin(), outerCondSlots.end());
    outputSlots.insert(outputSlots.end(), innerProjectSlots.begin(), innerProjectSlots.end());
    outputSlots.insert(outputSlots.end(), outerProjectSlots.begin(), outerProjectSlots.end());

    auto hashJoinStage = makeS<HashJoinStage>(std::move(outerStage),
                                              std::move(innerStage),
                                              outerCondSlots,
                                              outerProjectSlots,
                                              innerCondSlots,
                                              innerProjectSlots,
                                              collatorSlot,
                                              nullptr /* yieldPolicy */,
                                              kEmptyPlanNodeId);

    return std::make_pair(std::move(outputSlots), std::move(hashJoinStage));
}

TEST_F(HashJoinStageTest, BasicHashJoin) {
    auto ctx = makeCompileCtx();

    // Outer side (probes hash table): [[key, proj], ...] = [[1, "big_string_1"], [2,
    // "big_string_1"], [3, "big_string3"]]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "big_string_1") << BSON_ARRAY(2 << "big_string_2")
                                                             << BSON_ARRAY(3 << "big_string_3")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side (builds hash table): [[key, proj], ...] = [[2, "big_string_x"], [3,
    // "big_string_y"], [4, "big_string_z"]]
    auto [innerTag, innerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(2 << "big_string_x") << BSON_ARRAY(3 << "big_string_y")
                                                             << BSON_ARRAY(4 << "big_string_z")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    // Get all results
    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Key 2 matches: [2, 2, "big_string_x", "big_string_2"]
    // Key 3 matches: [3, 3, "big_string_y", "big_string_3"]
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(2 << 2 << "big_string_x" << "big_string_2")
                             << BSON_ARRAY(3 << 3 << "big_string_y" << "big_string_3")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinEmptyOuter) {
    auto ctx = makeCompileCtx();

    // Empty outer side
    auto [outerTag, outerVal] = makeArray(BSONArray());
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side with values: [[key, proj], ...]
    auto [innerTag, innerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b") << BSON_ARRAY(3 << "c")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // No matches when outer is empty - expect empty array
    auto [expectedTag, expectedVal] = makeArray(BSONArray());
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinEmptyInner) {
    auto ctx = makeCompileCtx();

    // Outer side with values: [[key, proj], ...]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 100) << BSON_ARRAY(2 << 200) << BSON_ARRAY(3 << 300)));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Empty inner side
    auto [innerTag, innerVal] = makeArray(BSONArray());
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // No matches when inner is empty - expect empty array
    auto [expectedTag, expectedVal] = makeArray(BSONArray());
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinDuplicateKeys) {
    auto ctx = makeCompileCtx();

    // Outer side with duplicate keys: [[key, proj], ...] = [[1, "a"], [1, "b"], [2, "c"]]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(2 << "c")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side with duplicate keys: [[key, proj], ...] = [[1, 10], [2, 20], [2, 30]]
    auto [innerTag, innerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(2 << 20) << BSON_ARRAY(2 << 30)));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 1 << 10 << "a")
                             << BSON_ARRAY(1 << 1 << 10 << "b") << BSON_ARRAY(2 << 2 << 20 << "c")
                             << BSON_ARRAY(2 << 2 << 30 << "c")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinNoMatches) {
    auto ctx = makeCompileCtx();

    // Outer side: [[key, proj], ...] = [[1, "a"], [2, "b"], [3, "c"]]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(2 << "b") << BSON_ARRAY(3 << "c")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: keys 4, 5, 6 (no overlap) [[key, proj], ...]
    auto [innerTag, innerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(4 << "x") << BSON_ARRAY(5 << "y") << BSON_ARRAY(6 << "z")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // No matches since keys don't overlap - expect empty array
    auto [expectedTag, expectedVal] = makeArray(BSONArray());
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinStringKeys) {
    auto ctx = makeCompileCtx();

    // Outer side: string keys with projections [[key, proj], ...]
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY("apple" << 1) << BSON_ARRAY("banana" << 2) << BSON_ARRAY("cherry" << 3)));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: string keys with projections [[key, proj], ...]
    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY("banana" << "b") << BSON_ARRAY("cherry" << "c") << BSON_ARRAY("date" << "d")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // "banana" matches: ["banana", "banana", "b", 2]
    // "cherry" matches: ["cherry", "cherry", "c", 3]
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY("banana" << "banana" << "b" << 2)
                             << BSON_ARRAY("cherry" << "cherry" << "c" << 3)));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinMixedTypes) {
    auto ctx = makeCompileCtx();

    // Outer side: mixed integer and double values that are equal [[key, proj], ...]
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << "int1") << BSON_ARRAY(2.0 << "dbl2") << BSON_ARRAY(3 << "int3")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: values that should match [[key, proj], ...]
    auto [innerTag, innerVal] = makeArray(
        BSON_ARRAY(BSON_ARRAY(1.0 << 100) << BSON_ARRAY(2 << 200) << BSON_ARRAY(4 << 400)));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // 1.0 (inner) matches 1 (outer): [1.0, 1, 100, "int1"]
    // 2 (inner) matches 2.0 (outer): [2, 2.0, 200, "dbl2"]
    auto [expectedTag, expectedVal] = makeArray(
        BSON_ARRAY(BSON_ARRAY(1.0 << 1 << 100 << "int1") << BSON_ARRAY(2 << 2.0 << 200 << "dbl2")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinSingleMatch) {
    auto ctx = makeCompileCtx();

    // Single element on each side that matches [[key, proj]]
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(BSON_ARRAY(42 << "outer")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(BSON_ARRAY(42 << "inner")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Single match: [42, 42, "inner", "outer"]
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(42 << 42 << "inner" << "outer")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinManyDuplicatesOnOuter) {
    auto ctx = makeCompileCtx();

    // Many duplicates on outer side with different projections [[key, proj], ...]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "c")
                                                  << BSON_ARRAY(1 << "d") << BSON_ARRAY(1 << "e")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Single match on inner side [[key, proj]]
    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(BSON_ARRAY(1 << 100)));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Inner (key=1, proj=100) matches all 5 outer rows with different projections
    auto [expectedTag, expectedVal] = makeArray(
        BSON_ARRAY(BSON_ARRAY(1 << 1 << 100 << "a")
                   << BSON_ARRAY(1 << 1 << 100 << "b") << BSON_ARRAY(1 << 1 << 100 << "c")
                   << BSON_ARRAY(1 << 1 << 100 << "d") << BSON_ARRAY(1 << 1 << 100 << "e")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinManyDuplicatesOnInner) {
    auto ctx = makeCompileCtx();

    // Single probe on outer [[key, proj]]
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(BSON_ARRAY(1 << "big_outer_string")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Many matches from inner with same key but different projections [[key, proj], ...]
    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << "big_inner_string_10")
        << BSON_ARRAY(1 << "big_inner_string_20") << BSON_ARRAY(1 << "big_inner_string_30")
        << BSON_ARRAY(1 << "big_inner_string_40") << BSON_ARRAY(1 << "big_inner_string_50")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // All 5 inner rows (each with different proj) match the single outer row
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 1 << "big_inner_string_10" << "big_outer_string")
                             << BSON_ARRAY(1 << 1 << "big_inner_string_20" << "big_outer_string")
                             << BSON_ARRAY(1 << 1 << "big_inner_string_30" << "big_outer_string")
                             << BSON_ARRAY(1 << 1 << "big_inner_string_40" << "big_outer_string")
                             << BSON_ARRAY(1 << 1 << "big_inner_string_50" << "big_outer_string")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinCartesianProduct) {
    auto ctx = makeCompileCtx();

    // 3 duplicates on outer with different projections [[key, proj], ...]
    auto [outerTag, outerVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << "a") << BSON_ARRAY(1 << "b") << BSON_ARRAY(1 << "c")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // 4 probes on inner with same key but different projections [[key, proj], ...]
    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 1) << BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3) << BSON_ARRAY(1 << 4)));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // Cartesian product: 4 inner rows x 3 outer rows = 12 results
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 1 << 1 << "a")
                             << BSON_ARRAY(1 << 1 << 1 << "b") << BSON_ARRAY(1 << 1 << 1 << "c")
                             << BSON_ARRAY(1 << 1 << 2 << "a") << BSON_ARRAY(1 << 1 << 2 << "b")
                             << BSON_ARRAY(1 << 1 << 2 << "c") << BSON_ARRAY(1 << 1 << 3 << "a")
                             << BSON_ARRAY(1 << 1 << 3 << "b") << BSON_ARRAY(1 << 1 << 3 << "c")
                             << BSON_ARRAY(1 << 1 << 4 << "a") << BSON_ARRAY(1 << 1 << 4 << "b")
                             << BSON_ARRAY(1 << 1 << 4 << "c")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinNullKeys) {
    auto ctx = makeCompileCtx();

    // Outer with null key [[key, proj], ...]
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(BSONNULL << "null") << BSON_ARRAY(1 << "one") << BSON_ARRAY(2 << "two")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner with null key [[key, proj], ...]
    auto [innerTag, innerVal] = makeArray(
        BSON_ARRAY(BSON_ARRAY(BSONNULL << 0) << BSON_ARRAY(2 << 200) << BSON_ARRAY(3 << 300)));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerCondSlot = outerSlots[0];
    auto outerProjSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
    auto innerCondSlot = innerSlots[0];
    auto innerProjSlot = innerSlots[1];

    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Expected results: [innerKey, outerKey, innerProj, outerProj]
    // null-null match: [null, null, 0, "null"]
    // 2-2 match: [2, 2, 200, "two"]
    auto [expectedTag, expectedVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(BSONNULL << BSONNULL << 0 << "null") << BSON_ARRAY(2 << 2 << 200 << "two")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

TEST_F(HashJoinStageTest, HashJoinCollationTest) {
    using namespace std::literals;
    for (auto useCollator : {false, true}) {
        // Inner side: [[key, proj], ...]
        auto [innerTag, innerVal] = makeArray(
            BSON_ARRAY(BSON_ARRAY("a" << 1) << BSON_ARRAY("b" << 2) << BSON_ARRAY("c" << 3)));
        value::ValueGuard innerGuard{innerTag, innerVal};

        // Outer side: [[key, proj], ...]
        auto [outerTag, outerVal] = makeArray(
            BSON_ARRAY(BSON_ARRAY("a" << "x") << BSON_ARRAY("b" << "y") << BSON_ARRAY("A" << "z")));
        value::ValueGuard outerGuard{outerTag, outerVal};

        auto collatorSlot = generateSlotId();

        auto makeStageFn =
            [this, collatorSlot, useCollator](value::SlotId outerCondSlot,
                                              value::SlotId outerProjSlot,
                                              value::SlotId innerCondSlot,
                                              value::SlotId innerProjSlot,
                                              std::unique_ptr<PlanStage> outerStage,
                                              std::unique_ptr<PlanStage> innerStage) {
                auto hashJoinStage =
                    makeS<HashJoinStage>(std::move(outerStage),
                                         std::move(innerStage),
                                         makeSV(outerCondSlot),
                                         makeSV(outerProjSlot),
                                         makeSV(innerCondSlot),
                                         makeSV(innerProjSlot),
                                         boost::optional<value::SlotId>{useCollator, collatorSlot},
                                         nullptr /* yieldPolicy */,
                                         kEmptyPlanNodeId);

                return std::make_pair(
                    makeSV(innerCondSlot, outerCondSlot, innerProjSlot, outerProjSlot),
                    std::move(hashJoinStage));
            };

        auto ctx = makeCompileCtx();

        // Setup collator and insert it into the ctx.
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        value::OwnedValueAccessor collatorAccessor;
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Two separate virtual scans are needed since HashJoinStage needs two child stages.
        outerGuard.reset();
        auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
        auto outerCondSlot = outerSlots[0];
        auto outerProjSlot = outerSlots[1];

        innerGuard.reset();
        auto [innerSlots, innerStage] = generateVirtualScanMulti(2, innerTag, innerVal);
        auto innerCondSlot = innerSlots[0];
        auto innerProjSlot = innerSlots[1];

        // Call the `makeStage` callback to create the HashJoinStage, passing in the mock scan
        // subtrees and the subtree's output slots.
        auto [outputSlots, stage] = makeStageFn(outerCondSlot,
                                                outerProjSlot,
                                                innerCondSlot,
                                                innerProjSlot,
                                                std::move(outerStage),
                                                std::move(innerStage));

        // Prepare the tree and get the SlotAccessor for the output slots.
        auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

        // Get all the results produced by HashJoin.
        auto [resultsTag, resultsVal] = getAllResultsMulti(stage.get(), resultAccessors);
        value::ValueGuard resultsGuard{resultsTag, resultsVal};

        // Expected results: [innerKey, outerKey, innerProj, outerProj]
        // With collator: "a" matches "a" and "A" (case-insensitive), "b" matches "b"
        // Without collator: "a" matches "a", "b" matches "b"
        value::TypeTags expectedTag;
        value::Value expectedVal;
        if (useCollator) {
            std::tie(expectedTag, expectedVal) = makeArray(BSON_ARRAY(
                BSON_ARRAY("a" << "a" << 1 << "x")
                << BSON_ARRAY("a" << "A" << 1 << "z") << BSON_ARRAY("b" << "b" << 2 << "y")));
        } else {
            std::tie(expectedTag, expectedVal) = makeArray(BSON_ARRAY(
                BSON_ARRAY("a" << "a" << 1 << "x") << BSON_ARRAY("b" << "b" << 2 << "y")));
        }
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
    }
}

TEST_F(HashJoinStageTest, HashJoinMultipleJoinKeys) {
    auto ctx = makeCompileCtx();

    // Outer side: rows are [key1, key2, proj]. Join on (key1, key2).
    // (1,10), (1,20), (2,10)
    auto [outerTag, outerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 10 << "a") << BSON_ARRAY(1 << 20 << "b") << BSON_ARRAY(2 << 10 << "c")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: rows are [key1, key2, proj]. (1,10), (2,10), (2,20)
    auto [innerTag, innerVal] = makeArray(BSON_ARRAY(
        BSON_ARRAY(1 << 10 << "x") << BSON_ARRAY(2 << 10 << "y") << BSON_ARRAY(2 << 20 << "z")));
    value::ValueGuard innerGuard{innerTag, innerVal};

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(3, outerTag, outerVal);
    auto outerCondSlot0 = outerSlots[0];
    auto outerCondSlot1 = outerSlots[1];
    auto outerProjSlot = outerSlots[2];

    innerGuard.reset();
    auto [innerSlots, innerStage] = generateVirtualScanMulti(3, innerTag, innerVal);
    auto innerCondSlot0 = innerSlots[0];
    auto innerCondSlot1 = innerSlots[1];
    auto innerProjSlot = innerSlots[2];

    // Two condition slots per side: join on (key1, key2) matching inner and outer.
    auto [outputSlots, hashJoinStage] = makeHashJoinStage(this,
                                                          std::move(outerStage),
                                                          std::move(innerStage),
                                                          makeSV(outerCondSlot0, outerCondSlot1),
                                                          makeSV(outerProjSlot),
                                                          makeSV(innerCondSlot0, innerCondSlot1),
                                                          makeSV(innerProjSlot));

    auto resultAccessors = prepareTree(ctx.get(), hashJoinStage.get(), outputSlots);

    auto [resultsTag, resultsVal] = getAllResultsMulti(hashJoinStage.get(), resultAccessors);
    value::ValueGuard resultsGuard{resultsTag, resultsVal};

    // Output order: innerCond (key1, key2), outerCond (key1, key2), innerProj, outerProj.
    // (1,10) matches: [1, 10, 1, 10, "x", "a"]
    // (2,10) matches: [2, 10, 2, 10, "y", "c"]
    // (1,20) and (2,20) have no match on the other side.
    auto [expectedTag, expectedVal] =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 10 << 1 << 10 << "x" << "a")
                             << BSON_ARRAY(2 << 10 << 2 << 10 << "y" << "c")));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    assertResultsMatch(resultsTag, resultsVal, expectedTag, expectedVal);
}

}  // namespace mongo::sbe
