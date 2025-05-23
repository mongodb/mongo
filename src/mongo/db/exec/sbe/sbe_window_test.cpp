/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/window.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class WindowStageTest : public PlanStageTestFixture {
public:
    using WindowOffset = std::
        tuple<value::SlotId, value::SlotId, boost::optional<int32_t>, boost::optional<int32_t>>;

    // Adds a window stage with the stage provided in the argument as its child.
    std::pair<std::unique_ptr<PlanStage>, value::SlotVector> addWindowStage(
        std::unique_ptr<PlanStage> stage,
        value::SlotVector partitionSlots,
        value::SlotVector forwardSlots,
        value::SlotId valueSlot,
        std::vector<WindowOffset> windowOffsets,
        boost::optional<value::SlotId> collatorSlot,
        bool allowDiskUse = false) {
        using namespace stage_builder;
        value::SlotVector windowSlots;
        std::vector<WindowStage::Window> windows;

        sbe::value::SlotVector currSlots;
        sbe::value::SlotVector boundTestingSlots;
        auto addSlotForDocument = [&](value::SlotId slot) {
            for (size_t i = 0; i < currSlots.size(); i++) {
                if (slot == currSlots[i]) {
                    return i;
                }
            }
            currSlots.push_back(slot);
            boundTestingSlots.push_back(generateSlotId());
            return currSlots.size() - 1;
        };
        for (auto slot : partitionSlots) {
            addSlotForDocument(slot);
        }
        for (auto slot : forwardSlots) {
            addSlotForDocument(slot);
        }
        addSlotForDocument(valueSlot);

        for (auto [lowBoundSlot, highBoundSlot, lowerOffset, higherOffset] : windowOffsets) {
            auto windowSlot = generateSlotId();
            auto lowBoundSlotIdx = addSlotForDocument(lowBoundSlot);
            auto lowBoundTestingSlot = boundTestingSlots[lowBoundSlotIdx];
            auto highBoundSlotIdx = addSlotForDocument(highBoundSlot);
            auto highBoundTestingSlot = boundTestingSlots[highBoundSlotIdx];
            windowSlots.push_back(windowSlot);

            WindowStage::Window window;
            window.windowExprSlots.push_back(windowSlot);
            window.lowBoundExpr = nullptr;
            if (lowerOffset) {
                window.lowBoundExpr = sbe::makeE<sbe::EPrimBinary>(
                    EPrimBinary::greaterEq,
                    makeVariable(lowBoundTestingSlot),
                    sbe::makeE<sbe::EPrimBinary>(EPrimBinary::add,
                                                 makeVariable(lowBoundSlot),
                                                 makeInt32Constant(*lowerOffset)));
            }
            window.highBoundExpr = nullptr;
            if (higherOffset) {
                window.highBoundExpr = sbe::makeE<sbe::EPrimBinary>(
                    EPrimBinary::lessEq,
                    makeVariable(highBoundTestingSlot),
                    sbe::makeE<sbe::EPrimBinary>(EPrimBinary::add,
                                                 makeVariable(highBoundSlot),
                                                 makeInt32Constant(*higherOffset)));
            }
            window.initExprs.push_back(nullptr);
            window.addExprs.push_back(makeFunction("aggDoubleDoubleSum", makeVariable(valueSlot)));
            window.removeExprs.push_back(makeFunction(
                "aggDoubleDoubleSum",
                sbe::makeE<sbe::EPrimUnary>(EPrimUnary::negate, makeVariable(valueSlot))));

            windows.emplace_back(std::move(window));
        }
        stage = makeS<WindowStage>(std::move(stage),
                                   std::move(currSlots),
                                   std::move(boundTestingSlots),
                                   partitionSlots.size(),
                                   std::move(windows),
                                   collatorSlot,
                                   allowDiskUse,
                                   kEmptyPlanNodeId);


        return {std::move(stage), std::move(windowSlots)};
    }

    std::pair<std::unique_ptr<PlanStage>, value::SlotVector> createSimpleWindowStage(
        std::unique_ptr<PlanStage> stage,
        value::SlotVector partitionSlots,
        value::SlotVector forwardSlots,
        value::SlotId valueSlot,
        std::vector<WindowOffset> windowOffsets,
        boost::optional<value::SlotId> collatorSlot) {
        using namespace stage_builder;

        auto [windowStage, windowSlots] = addWindowStage(
            std::move(stage), partitionSlots, forwardSlots, valueSlot, windowOffsets, collatorSlot);

        value::SlotVector resultSlots;
        SlotExprPairVector projects;
        for (auto windowSlot : windowSlots) {
            auto resultSlot = generateSlotId();
            resultSlots.push_back(resultSlot);
            projects.emplace_back(
                resultSlot,
                makeE<EIf>(makeFunction("exists", makeVariable(windowSlot)),
                           makeFunction("doubleDoubleSumFinalize", makeVariable(windowSlot)),
                           makeInt32Constant(0)));
        }
        stage = makeS<ProjectStage>(std::move(windowStage), std::move(projects), kEmptyPlanNodeId);
        return {std::move(stage), std::move(resultSlots)};
    }
};

TEST_F(WindowStageTest, IntWindowTest) {
    auto ctx = makeCompileCtx();
    // Create a test case of two partitions with [partitionSlot, boundSlot1, boundSlot2, valueSlot]
    auto [dataTag, dataVal] = stage_builder::makeValue(BSON_ARRAY(
        // First partition
        BSON_ARRAY(1 << 1 << 2 << 100)
        << BSON_ARRAY(1 << 3 << 4 << 200) << BSON_ARRAY(1 << 5 << 6 << 300)
        << BSON_ARRAY(1 << 7 << 8 << 400) << BSON_ARRAY(1 << 9 << 10 << 500) <<
        // Second partition
        BSON_ARRAY(2 << 11 << 12 << 100) << BSON_ARRAY(2 << 13 << 14 << 200)
        << BSON_ARRAY(2 << 15 << 16 << 300) << BSON_ARRAY(2 << 17 << 18 << 400)
        << BSON_ARRAY(2 << 19 << 20 << 500)));
    auto [slots, inputStage] = generateVirtualScanMulti(4, dataTag, dataVal);
    value::SlotVector partitionSlots{slots[0]};
    auto boundSlot1 = slots[1];
    auto boundSlot2 = slots[2];
    auto valueSlot = slots[3];
    value::SlotVector forwardSlots{valueSlot};
    std::vector<WindowOffset> windowOffsets{
        // Both boundSlot1 and boundSlot2 are evenly spaced 2 units apart, we expect a range of [-2,
        // +2] to cover
        // 1 document on either side of the current document, similarly for other ranges.
        {boundSlot1, boundSlot1, -2, 2},
        {boundSlot1, boundSlot2, -2, 2},
        {boundSlot2, boundSlot2, -2, 2},
        {boundSlot1, boundSlot1, boost::none, 0},
        {boundSlot1, boundSlot1, 0, boost::none},
        {boundSlot1, boundSlot1, -6, -2},
        {boundSlot1, boundSlot1, 2, 6},
        {boundSlot1, boundSlot1, boost::none, -3},
    };
    auto [resultStage, resultSlots] = createSimpleWindowStage(std::move(inputStage),
                                                              std::move(partitionSlots),
                                                              std::move(forwardSlots),
                                                              valueSlot,
                                                              std::move(windowOffsets),
                                                              boost::optional<value::SlotId>());

    prepareTree(ctx.get(), resultStage.get());
    std::vector<value::SlotAccessor*> resultAccessors;
    for (auto resultSlot : resultSlots) {
        resultAccessors.push_back(resultStage->getAccessor(*ctx, resultSlot));
    }
    resultStage->open(false);

    // The same results repeated twice for two partitions.
    std::vector<std::vector<int32_t>> expected{
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {100, 300, 600, 1000, 1500, 100, 300, 600, 1000, 1500},
        {1500, 1400, 1200, 900, 500, 1500, 1400, 1200, 900, 500},
        {0, 100, 300, 600, 900, 0, 100, 300, 600, 900},
        {900, 1200, 900, 500, 0, 900, 1200, 900, 500, 0},
        {0, 0, 100, 300, 600, 0, 0, 100, 300, 600},
    };
    for (size_t i = 0; i < expected[0].size(); i++) {
        auto planState = resultStage->getNext();
        ASSERT_EQ(PlanState::ADVANCED, planState);

        for (size_t j = 0; j < resultSlots.size(); j++) {
            auto [tag, val] = resultAccessors[j]->getViewOfValue();
            ASSERT_EQ(value::TypeTags::NumberInt32, tag);
            ASSERT_EQ(expected[j][i], val);
        }
    }
    auto planState = resultStage->getNext();
    ASSERT_EQ(PlanState::IS_EOF, planState);
}

TEST_F(WindowStageTest, StringWindowTest) {
    auto ctx = makeCompileCtx();
    // Create a test case of two partitions with [partitionSlot, boundSlot1, boundSlot2, valueSlot]
    auto [dataTag, dataVal] = stage_builder::makeValue(BSON_ARRAY(
        // First partition
        BSON_ARRAY("1" << 1 << 2 << 100)
        << BSON_ARRAY("1" << 3 << 4 << 200) << BSON_ARRAY("1" << 5 << 6 << 300)
        << BSON_ARRAY("1" << 7 << 8 << 400) << BSON_ARRAY("1" << 9 << 10 << 500) <<
        // Second partition
        BSON_ARRAY("2" << 11 << 12 << 100) << BSON_ARRAY("2" << 13 << 14 << 200)
        << BSON_ARRAY("2" << 15 << 16 << 300) << BSON_ARRAY("2" << 17 << 18 << 400)
        << BSON_ARRAY("2" << 19 << 20 << 500)));
    auto [slots, inputStage] = generateVirtualScanMulti(4, dataTag, dataVal);
    value::SlotVector partitionSlots{slots[0]};
    auto boundSlot1 = slots[1];
    auto boundSlot2 = slots[2];
    auto valueSlot = slots[3];
    value::SlotVector forwardSlots{valueSlot};
    std::vector<WindowOffset> windowOffsets{
        // Both boundSlot1 and boundSlot2 are evenly spaced 2 units apart, we expect a range of [-2,
        // +2] to cover
        // 1 document on either side of the current document, similarly for other ranges.
        {boundSlot1, boundSlot1, -2, 2},
        {boundSlot1, boundSlot2, -2, 2},
        {boundSlot2, boundSlot2, -2, 2},
        {boundSlot1, boundSlot1, boost::none, 0},
        {boundSlot1, boundSlot1, 0, boost::none},
        {boundSlot1, boundSlot1, -6, -2},
        {boundSlot1, boundSlot1, 2, 6},
        {boundSlot1, boundSlot1, boost::none, -3},
    };
    auto [resultStage, resultSlots] = createSimpleWindowStage(std::move(inputStage),
                                                              std::move(partitionSlots),
                                                              std::move(forwardSlots),
                                                              valueSlot,
                                                              std::move(windowOffsets),
                                                              boost::optional<value::SlotId>());

    prepareTree(ctx.get(), resultStage.get());
    std::vector<value::SlotAccessor*> resultAccessors;
    for (auto resultSlot : resultSlots) {
        resultAccessors.push_back(resultStage->getAccessor(*ctx, resultSlot));
    }
    resultStage->open(false);

    // The same results repeated twice for two partitions.
    std::vector<std::vector<int32_t>> expected{
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 900, 300, 600, 900, 1200, 900},
        {100, 300, 600, 1000, 1500, 100, 300, 600, 1000, 1500},
        {1500, 1400, 1200, 900, 500, 1500, 1400, 1200, 900, 500},
        {0, 100, 300, 600, 900, 0, 100, 300, 600, 900},
        {900, 1200, 900, 500, 0, 900, 1200, 900, 500, 0},
        {0, 0, 100, 300, 600, 0, 0, 100, 300, 600},
    };
    for (size_t i = 0; i < expected[0].size(); i++) {
        auto planState = resultStage->getNext();
        ASSERT_EQ(PlanState::ADVANCED, planState);

        for (size_t j = 0; j < resultSlots.size(); j++) {
            auto [tag, val] = resultAccessors[j]->getViewOfValue();
            ASSERT_EQ(value::TypeTags::NumberInt32, tag);
            ASSERT_EQ(expected[j][i], val);
        }
    }
    auto planState = resultStage->getNext();
    ASSERT_EQ(PlanState::IS_EOF, planState);
}

TEST_F(WindowStageTest, CollatorWindowTest) {
    auto ctx = makeCompileCtx();
    // Create a test case of two partitions with [partitionSlot, boundSlot1, boundSlot2, valueSlot]
    auto [dataTag, dataVal] = stage_builder::makeValue(BSON_ARRAY(
        // First partition
        BSON_ARRAY("1" << 1 << 2 << 100)
        << BSON_ARRAY("1" << 3 << 4 << 200) << BSON_ARRAY("1" << 5 << 6 << 300)
        << BSON_ARRAY("1" << 7 << 8 << 400) << BSON_ARRAY("1" << 9 << 10 << 500) <<
        // Second partition
        BSON_ARRAY("2" << 11 << 12 << 100) << BSON_ARRAY("2" << 13 << 14 << 200)
        << BSON_ARRAY("2" << 15 << 16 << 300) << BSON_ARRAY("2" << 17 << 18 << 400)
        << BSON_ARRAY("2" << 19 << 20 << 500)));
    auto [slots, inputStage] = generateVirtualScanMulti(4, dataTag, dataVal);
    value::SlotVector partitionSlots{slots[0]};
    auto boundSlot1 = slots[1];
    auto boundSlot2 = slots[2];
    auto valueSlot = slots[3];
    value::SlotVector forwardSlots{valueSlot};
    std::vector<WindowOffset> windowOffsets{
        // Both boundSlot1 and boundSlot2 are evenly spaced 2 units apart, we expect a range of [-2,
        // +2] to cover 1 document on either side of the current document, similarly for other
        // ranges.
        {boundSlot1, boundSlot1, -2, 2},
        {boundSlot1, boundSlot2, -2, 2},
        {boundSlot2, boundSlot2, -2, 2},
        {boundSlot1, boundSlot1, boost::none, 0},
        {boundSlot1, boundSlot1, 0, boost::none},
        {boundSlot1, boundSlot1, -6, -2},
        {boundSlot1, boundSlot1, 2, 6},
        {boundSlot1, boundSlot1, boost::none, -3},
    };

    auto collatorSlot = generateSlotId();
    // Setup collator and insert it into the ctx.
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    auto [resultStage, resultSlots] = createSimpleWindowStage(std::move(inputStage),
                                                              std::move(partitionSlots),
                                                              std::move(forwardSlots),
                                                              valueSlot,
                                                              std::move(windowOffsets),
                                                              collatorSlot);

    prepareTree(ctx.get(), resultStage.get());
    std::vector<value::SlotAccessor*> resultAccessors;
    for (auto resultSlot : resultSlots) {
        resultAccessors.push_back(resultStage->getAccessor(*ctx, resultSlot));
    }
    resultStage->open(false);
    std::vector<std::vector<int32_t>> expected{
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {100, 300, 600, 1000, 1500, 1600, 1800, 2100, 2500, 3000},
        {3000, 2900, 2700, 2400, 2000, 1500, 1400, 1200, 900, 500},
        {0, 100, 300, 600, 900, 1200, 1000, 800, 600, 900},
        {900, 1200, 1000, 800, 600, 900, 1200, 900, 500, 0},
        {0, 0, 100, 300, 600, 1000, 1500, 1600, 1800, 2100}};

    for (size_t i = 0; i < expected[0].size(); i++) {
        auto planState = resultStage->getNext();
        ASSERT_EQ(PlanState::ADVANCED, planState);

        for (size_t j = 0; j < resultSlots.size(); j++) {
            auto [tag, val] = resultAccessors[j]->getViewOfValue();
            ASSERT_EQ(value::TypeTags::NumberInt32, tag);
            ASSERT_EQ(expected[j][i], val);
        }
    }
    auto planState = resultStage->getNext();
    ASSERT_EQ(PlanState::IS_EOF, planState);
}

TEST_F(WindowStageTest, ForceSpillWindowTest) {
    auto ctx = makeCompileCtx();
    // Create a test case of two partitions with [partitionSlot, boundSlot1, boundSlot2, valueSlot]
    auto [dataTag, dataVal] = stage_builder::makeValue(BSON_ARRAY(
        // First partition
        BSON_ARRAY("1" << 1 << 2 << 100)
        << BSON_ARRAY("1" << 3 << 4 << 200) << BSON_ARRAY("1" << 5 << 6 << 300)
        << BSON_ARRAY("1" << 7 << 8 << 400) << BSON_ARRAY("1" << 9 << 10 << 500) <<
        // Second partition
        BSON_ARRAY("2" << 11 << 12 << 100) << BSON_ARRAY("2" << 13 << 14 << 200)
        << BSON_ARRAY("2" << 15 << 16 << 300) << BSON_ARRAY("2" << 17 << 18 << 400)
        << BSON_ARRAY("2" << 19 << 20 << 500)));
    auto [slots, inputStage] = generateVirtualScanMulti(4, dataTag, dataVal);
    value::SlotVector partitionSlots{slots[0]};
    auto boundSlot1 = slots[1];
    auto boundSlot2 = slots[2];
    auto valueSlot = slots[3];
    value::SlotVector forwardSlots{valueSlot};
    std::vector<WindowOffset> windowOffsets{
        // Both boundSlot1 and boundSlot2 are evenly spaced 2 units apart, we expect a range of [-2,
        // +2] to cover 1 document on either side of the current document, similarly for other
        // ranges.
        {boundSlot1, boundSlot1, -2, 2},
        {boundSlot1, boundSlot2, -2, 2},
        {boundSlot2, boundSlot2, -2, 2},
        {boundSlot1, boundSlot1, boost::none, 0},
        {boundSlot1, boundSlot1, 0, boost::none},
        {boundSlot1, boundSlot1, -6, -2},
        {boundSlot1, boundSlot1, 2, 6},
        {boundSlot1, boundSlot1, boost::none, -3},
    };

    auto collatorSlot = generateSlotId();
    // Setup collator and insert it into the ctx.
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    value::OwnedValueAccessor collatorAccessor;
    ctx->pushCorrelated(collatorSlot, &collatorAccessor);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.release()));

    auto [windowStage, windowSlots] = addWindowStage(std::move(inputStage),
                                                     std::move(partitionSlots),
                                                     std::move(forwardSlots),
                                                     valueSlot,
                                                     std::move(windowOffsets),
                                                     collatorSlot,
                                                     true /* allowDiskUse */);

    auto resultAccessors = prepareTree(ctx.get(), windowStage.get(), windowSlots);

    std::vector<std::vector<int32_t>> expected{
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {300, 600, 900, 1200, 1000, 800, 600, 900, 1200, 900},
        {100, 300, 600, 1000, 1500, 1600, 1800, 2100, 2500, 3000},
        {3000, 2900, 2700, 2400, 2000, 1500, 1400, 1200, 900, 500},
        {0, 100, 300, 600, 900, 1200, 1000, 800, 600, 900},
        {900, 1200, 1000, 800, 600, 900, 1200, 900, 500, 0},
        {0, 0, 100, 300, 600, 1000, 1500, 1600, 1800, 2100}};

    windowStage->open(false);
    int idx = 0;
    while (windowStage->getNext() == PlanState::ADVANCED) {
        for (size_t i = 0; i < resultAccessors.size(); ++i) {
            auto [resTag, resVal] = resultAccessors[i]->getViewOfValue();
            double actualValue = 0;
            if (resTag != value::TypeTags::Nothing) {
                ASSERT_EQ(value::TypeTags::Array, resTag);
                auto resArray = value::getArrayView(resVal);
                ASSERT_EQ(3, resArray->size());
                const auto [finalTag, finalVal] = resArray->getAt(1);
                ASSERT_EQ(value::TypeTags::NumberDouble, finalTag);
                actualValue = value::bitcastTo<double>(finalVal);
            }
            ASSERT_EQ(expected[i][idx], actualValue);
        }

        if (idx == 2) {
            // Make sure it has not spilled already.
            auto stats = static_cast<const WindowStats*>(windowStage->getSpecificStats());
            ASSERT_FALSE(stats->usedDisk);
            ASSERT_EQ(0, stats->spillingStats.getSpills());
            ASSERT_EQ(0, stats->spillingStats.getSpilledRecords());

            // Get ready to yield.
            windowStage->saveState();

            // Force spill.
            windowStage->forceSpill(nullptr /*yieldPolicy*/);

            // Check stats to make sure it spilled
            stats = static_cast<const WindowStats*>(windowStage->getSpecificStats());
            ASSERT_TRUE(stats->usedDisk);
            ASSERT_EQ(1, stats->spillingStats.getSpills());
            ASSERT_EQ(10, stats->spillingStats.getSpilledRecords());

            // Get ready to retrieve more records.
            windowStage->restoreState();
        }
        ++idx;
    }

    windowStage->close();
}
}  // namespace mongo::sbe
