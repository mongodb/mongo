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
 * This file contains tests for sbe::AndHashStage.
 */

#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/and_hash.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

using AndHashStageTest = PlanStageTestFixture;

TEST_F(AndHashStageTest, AndHashCollationTest) {
    using namespace std::literals;
    for (auto useCollator : {false, true}) {
        auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY("a" << "b"
                                                                            << "c"));
        value::ValueGuard innerGuard{innerTag, innerVal};

        auto [outerTag, outerVal] = stage_builder::makeValue(BSON_ARRAY("a" << "b"
                                                                            << "A"));
        value::ValueGuard outerGuard{outerTag, outerVal};

        // After running the join we expect to get back pairs of the keys that were
        // matched up.
        std::vector<std::pair<std::string, std::string>> expectedVec;
        if (useCollator) {
            expectedVec = {{"a", "A"}, {"a", "a"}, {"b", "b"}};
        } else {
            expectedVec = {{"a", "a"}, {"b", "b"}};
        }

        auto collatorSlot = generateSlotId();

        auto makeStageFn = [this, collatorSlot, useCollator](
                               value::SlotId outerCondSlot,
                               value::SlotId innerCondSlot,
                               std::unique_ptr<PlanStage> outerStage,
                               std::unique_ptr<PlanStage> innerStage) {
            auto andHashStage =
                makeS<AndHashStage>(std::move(outerStage),
                                    std::move(innerStage),
                                    makeSV(outerCondSlot),
                                    makeSV(),
                                    makeSV(innerCondSlot),
                                    makeSV(),
                                    boost::optional<value::SlotId>{useCollator, collatorSlot},
                                    nullptr /* yieldPolicy */,
                                    kEmptyPlanNodeId);

            return std::make_pair(makeSV(innerCondSlot, outerCondSlot), std::move(andHashStage));
        };

        auto ctx = makeCompileCtx();

        // Setup collator and insert it into the ctx.
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        value::OwnedValueAccessor collatorAccessor;
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Two separate virtual scans are needed since AndHashStage needs two child stages.
        outerGuard.reset();
        auto [outerCondSlot, outerStage] = generateVirtualScan(outerTag, outerVal);

        innerGuard.reset();
        auto [innerCondSlot, innerStage] = generateVirtualScan(innerTag, innerVal);

        // Call the `makeStage` callback to create the AndHashStage, passing in the mock scan
        // subtrees and the subtree's output slots.
        auto [outputSlots, stage] =
            makeStageFn(outerCondSlot, innerCondSlot, std::move(outerStage), std::move(innerStage));

        // Prepare the tree and get the SlotAccessor for the output slots.
        auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

        // Get all the results produced by AndHash.
        auto [resultsTag, resultsVal] = getAllResultsMulti(stage.get(), resultAccessors);
        value::ValueGuard resultsGuard{resultsTag, resultsVal};
        ASSERT_EQ(resultsTag, value::TypeTags::Array);
        auto resultsView = value::getArrayView(resultsVal);

        // make sure all the expected pairs occur in the result
        ASSERT_EQ(resultsView->size(), expectedVec.size());
        for (const auto& [outer, inner] : expectedVec) {
            auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(outer << inner));
            bool found = false;
            for (size_t i = 0; i < resultsView->size(); i++) {
                auto [tag, val] = resultsView->getAt(i);
                auto [cmpTag, cmpVal] = compareValue(expectedTag, expectedVal, tag, val);
                if (cmpTag == value::TypeTags::NumberInt32 &&
                    value::bitcastTo<int32_t>(cmpVal) == 0) {
                    found = true;
                    break;
                }
            }

            ASSERT_TRUE(found);

            releaseValue(expectedTag, expectedVal);
        }
    }
}

TEST_F(AndHashStageTest, TestHashValueIsCopied) {
    // Outer side: one row with key="a" and projected value "projectedValue".
    // The string is >7 bytes to ensure heap allocation (StringBig), exercising
    // the ownership transfer in copyOrMoveValue().
    auto [outerTag, outerVal] =
        stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY("a" << "projectedValue")));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: two rows both with key="a". Both match the single outer row,
    // so AndHash produces two results from the same hash table entry.
    auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY("a" << "a"));
    value::ValueGuard innerGuard{innerTag, innerVal};

    auto ctx = makeCompileCtx();

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    auto outerKeySlot = outerSlots[0];
    auto outerProjectSlot = outerSlots[1];

    innerGuard.reset();
    auto [innerKeySlot, innerStage] = generateVirtualScan(innerTag, innerVal);

    auto andHashStage = makeS<AndHashStage>(std::move(outerStage),
                                            std::move(innerStage),
                                            makeSV(outerKeySlot),
                                            makeSV(outerProjectSlot),
                                            makeSV(innerKeySlot),
                                            makeSV(),
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);

    auto resultAccessors = prepareTree(ctx.get(), andHashStage.get(), makeSV(outerProjectSlot));
    auto* projectAccessor = resultAccessors[0];

    // First getNext(): inner "a" matches outer "a".
    ASSERT_EQ(andHashStage->getNext(), PlanState::ADVANCED);

    auto [tag1, val1] = projectAccessor->copyOrMoveValue().releaseToRaw();

    ASSERT_EQ(tag1, value::TypeTags::StringBig);

    // Releasing the old value before processing the next row.
    value::releaseValue(tag1, val1);

    // Second getNext(): another inner "a" matches the same outer hash table entry.
    ASSERT_EQ(andHashStage->getNext(), PlanState::ADVANCED);

    auto [tag2, val2] = projectAccessor->getViewOfValue();

    auto [expectedTag, expectedVal] = value::makeNewString("projectedValue");
    value::ValueGuard expectedGuard{expectedTag, expectedVal};
    ASSERT_TRUE(valueEquals(tag2, val2, expectedTag, expectedVal));

    ASSERT_EQ(andHashStage->getNext(), PlanState::IS_EOF);

    andHashStage->close();
}

TEST_F(AndHashStageTest, AndHashMemoryLimitExceeded) {
    // Set a 1-byte limit so the first document inserted into the hash table exceeds it.
    RAIIServerParameterControllerForTest maxMemoryLimit(
        "internalSlotBasedExecutionAndHashStageMaxMemoryBytes", 1);

    // Outer side: one row with key=1 and projected value 1.
    auto [outerTag, outerVal] =
        stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1 << BSON_ARRAY(1))));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: one row with key=1.
    auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY(1));
    value::ValueGuard innerGuard{innerTag, innerVal};

    auto ctx = makeCompileCtx();

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);
    innerGuard.reset();
    auto [innerKeySlot, innerStage] = generateVirtualScan(innerTag, innerVal);

    auto andHashStage = makeS<AndHashStage>(std::move(outerStage),
                                            std::move(innerStage),
                                            makeSV(outerSlots[0]),
                                            makeSV(outerSlots[1]),
                                            makeSV(innerKeySlot),
                                            makeSV(),
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);

    // prepareTree() calls open() internally; with a 1-byte limit the first row inserted into
    // the hash table exceeds the limit and open() throws before returning.
    ASSERT_THROWS_CODE(
        prepareTree(ctx.get(), andHashStage.get(), makeSV(outerSlots[0])), DBException, 12321801);
}

TEST_F(AndHashStageTest, AndHashMemoryTracking) {
    // Outer side: three rows, all with key=1 but different scalar projected values 10, 20, 30.
    auto [outerTag, outerVal] = stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 10) << BSON_ARRAY(1 << 20) << BSON_ARRAY(1 << 30)));
    value::ValueGuard outerGuard{outerTag, outerVal};

    // Inner side: two rows, both with key=1. Each inner probe matches all 3 outer rows,
    // so the join produces 3 outer rows x 2 inner probes = 6 output rows total.
    auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY(1 << 1));
    value::ValueGuard innerGuard{innerTag, innerVal};

    auto ctx = makeCompileCtx();

    outerGuard.reset();
    auto [outerSlots, outerStage] = generateVirtualScanMulti(2, outerTag, outerVal);

    innerGuard.reset();
    auto [innerKeySlot, innerStage] = generateVirtualScan(innerTag, innerVal);

    auto andHashStage = makeS<AndHashStage>(std::move(outerStage),
                                            std::move(innerStage),
                                            makeSV(outerSlots[0]),
                                            makeSV(outerSlots[1]),
                                            makeSV(innerKeySlot),
                                            makeSV(),
                                            boost::none,
                                            nullptr /* yieldPolicy */,
                                            kEmptyPlanNodeId);

    auto resultAccessors =
        prepareTree(ctx.get(), andHashStage.get(), makeSV(outerSlots[0], outerSlots[1]));

    andHashStage->open(false);

    // After open() the hash table holds all outer rows; memory must be non-zero.
    ASSERT_GT(andHashStage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);

    // Collect all results and verify correctness.
    int resultCount = 0;
    std::vector<int32_t> projectedValues;
    while (andHashStage->getNext() == PlanState::ADVANCED) {
        ++resultCount;

        // Key slot must be 1 for every result row.
        auto [keyTag, keyVal] = resultAccessors[0]->getViewOfValue();
        ASSERT_EQ(keyTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(keyVal), 1);

        // Project slot is a scalar integer (10, 20, or 30); collect it.
        auto [projTag, projVal] = resultAccessors[1]->getViewOfValue();
        ASSERT_EQ(projTag, value::TypeTags::NumberInt32);
        projectedValues.push_back(value::bitcastTo<int32_t>(projVal));
    }

    // 3 outer rows x 2 inner probes = 6 results.
    ASSERT_EQ(resultCount, 6);
    // Each projected value {10, 20, 30} must appear exactly twice (once per inner probe).
    std::sort(projectedValues.begin(), projectedValues.end());
    ASSERT_EQ(projectedValues, (std::vector<int32_t>{10, 10, 20, 20, 30, 30}));

    andHashStage->close();

    // After close() the hash table is freed and the tracker is reset.
    ASSERT_EQ(andHashStage->getMemoryTracker()->inUseTrackedMemoryBytes(), 0);

    // The peak from when the hash table was fully built must be captured in specific stats.
    auto* stats = static_cast<const AndHashStats*>(andHashStage->getSpecificStats());
    ASSERT_GT(stats->peakTrackedMemBytes, 0);
}

}  // namespace mongo::sbe
