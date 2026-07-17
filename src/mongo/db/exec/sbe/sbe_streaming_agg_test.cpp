/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/streaming_agg.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
/**
 * Tests for sbe::StreamingAggStage.
 */
class StreamingAggStageTest : public PlanStageTestFixture {
protected:
    static std::unique_ptr<PlanStage> makeStreamingAgg(
        std::unique_ptr<PlanStage> scanStage,
        value::SlotVector keySlots,
        boost::optional<value::SlotId> collatorSlot = boost::none) {
        return makeS<StreamingAggStage>(std::move(scanStage),
                                        std::move(keySlots),
                                        collatorSlot,
                                        std::vector<std::unique_ptr<HashAggAccumulator>>{},
                                        kEmptyPlanNodeId);
    }

    static BSONArray wrapEachElement(const BSONArray& values) {
        BSONArrayBuilder builder;
        for (const auto& value : values) {
            builder.append(BSON_ARRAY(value));
        }
        return builder.arr();
    }

    void runSingleKeyTest(const BSONArray& input, const BSONArray& expected) {
        runMultiKeyTest(1, wrapEachElement(input), wrapEachElement(expected));
    }

    void runMultiKeyTest(int numSlots, const BSONArray& input, const BSONArray& expected) {
        value::TagValueOwned inputValue{stage_builder::makeValue(input)};
        value::TagValueOwned expectedValue{stage_builder::makeValue(expected)};

        auto makeStageFn = [](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            return std::make_pair(scanSlots, makeStreamingAgg(std::move(scanStage), scanSlots));
        };

        runTestMulti(numSlots,
                     inputValue.tag(),
                     inputValue.value(),
                     expectedValue.tag(),
                     expectedValue.value(),
                     makeStageFn);
        inputValue.disown();
        expectedValue.disown();
    }

    void assertResults(PlanStage* stage,
                       value::SlotAccessor* accessor,
                       value::TypeTags expectedTag,
                       value::Value expectedVal) {
        value::TagValueOwned results{getAllResults(stage, accessor)};
        ASSERT_TRUE(valueEquals(results.tag(), results.value(), expectedTag, expectedVal));
    }
};

TEST_F(StreamingAggStageTest, DeduplicatesSortedInputSingleKey) {
    runSingleKeyTest(BSON_ARRAY(1 << 1 << 2 << 3 << 3 << 3), BSON_ARRAY(1 << 2 << 3));
}

TEST_F(StreamingAggStageTest, PreservesOrderWhenNoDuplicates) {
    runSingleKeyTest(BSON_ARRAY(1 << 2 << 3), BSON_ARRAY(1 << 2 << 3));
}

TEST_F(StreamingAggStageTest, PreservesOrderWithUnsortedUniqueKeys) {
    runSingleKeyTest(BSON_ARRAY(3 << 1 << 2), BSON_ARRAY(3 << 1 << 2));
}

TEST_F(StreamingAggStageTest, CollapsesAllIdenticalKeys) {
    runSingleKeyTest(BSON_ARRAY(5 << 5 << 5), BSON_ARRAY(5));
}

TEST_F(StreamingAggStageTest, HandlesEmptyInput) {
    runSingleKeyTest(BSONArray{}, BSONArray{});
}

TEST_F(StreamingAggStageTest, DeduplicatesMultipleSlotsInKey) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)));
}

TEST_F(StreamingAggStageTest, DoesNotDedupWhenOnlyFirstSlotMatches) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3)));
}

TEST_F(StreamingAggStageTest, DoesNotDedupWhenOnlySecondSlotMatches) {
    runMultiKeyTest(2,
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(2 << 2)),
                    BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(2 << 2)));
}

TEST_F(StreamingAggStageTest, RespectsCollation) {
    value::TagValueOwned inputValue{stage_builder::makeValue(BSON_ARRAY("a" << "A" << "b"))};
    value::TagValueOwned expectedValue{stage_builder::makeValue(BSON_ARRAY("a" << "b"))};

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto collatorSlot = generateSlotId();

    auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputValue));

    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot), collatorSlot);

    auto ctx = makeCompileCtx();

    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);
    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());
}

TEST_F(StreamingAggStageTest, ResetsStateAfterReopen) {
    value::TagValueOwned inputValue{stage_builder::makeValue(BSON_ARRAY(1 << 1 << 2 << 3 << 3))};
    value::TagValueOwned expectedValue{stage_builder::makeValue(BSON_ARRAY(1 << 2 << 3))};

    auto [scanSlot, scanStage] = generateVirtualScan(std::move(inputValue));

    auto stage = makeStreamingAgg(std::move(scanStage), sbe::makeSV(scanSlot));
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), scanSlot);

    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());

    stage->close();
    stage->open(true);

    assertResults(stage.get(), resultAccessor, expectedValue.tag(), expectedValue.value());
}

TEST_F(StreamingAggStageTest, SumAccumulatorGroupsByKey) {
    value::TagValueOwned inputValue{stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(1 << 2) << BSON_ARRAY(1 << 3) << BSON_ARRAY(2 << 2) << BSON_ARRAY(2 << 4)))};
    value::TagValueOwned expectedValue{
        stage_builder::makeValue(BSON_ARRAY(BSON_ARRAY(1 << 5) << BSON_ARRAY(2 << 6)))};

    auto [scanSlots, scanStage] = generateVirtualScanMulti(2, inputValue.tag(), inputValue.value());
    inputValue.disown();
    auto keySlot = scanSlots[0];
    auto valueSlot = scanSlots[1];

    auto sumSlot = generateSlotId();
    auto spillSlot = generateSlotId();
    std::vector<std::unique_ptr<HashAggAccumulator>> accumulators;
    accumulators.emplace_back(std::make_unique<CompiledHashAggAccumulator>(
        sumSlot,
        spillSlot,
        makeFunction(EFn::kSum, makeVariable(valueSlot)),
        makeE<EConstant>(value::TypeTags::Nothing, 0)));

    auto stage = makeS<StreamingAggStage>(std::move(scanStage),
                                          sbe::makeSV(keySlot),
                                          boost::none,
                                          std::move(accumulators),
                                          kEmptyPlanNodeId);

    auto ctx = makeCompileCtx();
    auto resultAccessors = prepareTree(ctx.get(), stage.get(), sbe::makeSV(keySlot, sumSlot));

    value::TagValueOwned results{getAllResultsMulti(stage.get(), resultAccessors)};
    ASSERT_TRUE(
        valueEquals(results.tag(), results.value(), expectedValue.tag(), expectedValue.value()));
}
}  // namespace mongo::sbe
