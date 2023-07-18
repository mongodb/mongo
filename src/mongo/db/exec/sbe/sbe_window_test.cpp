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
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

class WindowStageTest : public PlanStageTestFixture {
public:
    using WindowOffset =
        std::tuple<value::SlotId, boost::optional<int32_t>, boost::optional<int32_t>>;

    std::pair<std::unique_ptr<PlanStage>, value::SlotVector> createSimpleWindowStage(
        std::unique_ptr<PlanStage> stage,
        value::SlotVector partitionSlots,
        value::SlotVector forwardSlots,
        value::SlotId valueSlot,
        std::vector<WindowOffset> windowOffsets) {
        using namespace stage_builder;
        value::SlotVector windowSlots;
        std::vector<WindowStage::Window> windows;
        for (auto [boundSlot, lowerOffset, higherOffset] : windowOffsets) {
            auto boundTestingSlot = generateSlotId();
            auto windowSlot = generateSlotId();
            windowSlots.push_back(windowSlot);

            WindowStage::Window window;
            window.boundSlot = boundSlot;
            window.boundTestingSlot = boundTestingSlot;
            window.windowSlot = windowSlot;
            window.lowBoundExpr = nullptr;
            if (lowerOffset) {
                window.lowBoundExpr = makeBinaryOp(
                    EPrimBinary::greaterEq,
                    makeVariable(boundTestingSlot),
                    makeBinaryOp(EPrimBinary::add,
                                 makeVariable(boundSlot),
                                 makeConstant(value::TypeTags::NumberInt32, *lowerOffset)));
            }
            window.highBoundExpr = nullptr;
            if (higherOffset) {
                window.highBoundExpr = makeBinaryOp(
                    EPrimBinary::lessEq,
                    makeVariable(boundTestingSlot),
                    makeBinaryOp(EPrimBinary::add,
                                 makeVariable(boundSlot),
                                 makeConstant(value::TypeTags::NumberInt32, *higherOffset)));
            }
            window.initExpr = nullptr;
            window.addExpr = makeFunction("aggDoubleDoubleSum", makeVariable(valueSlot));
            window.removeExpr = makeFunction(
                "aggDoubleDoubleSum", makeUnaryOp(EPrimUnary::negate, makeVariable(valueSlot)));

            windows.emplace_back(std::move(window));
        }
        stage = makeS<WindowStage>(std::move(stage),
                                   std::move(partitionSlots),
                                   std::move(forwardSlots),
                                   std::move(windows),
                                   kEmptyPlanNodeId);

        value::SlotVector resultSlots;
        value::SlotMap<std::unique_ptr<EExpression>> slotMap;
        for (auto windowSlot : windowSlots) {
            auto resultSlot = generateSlotId();
            resultSlots.push_back(resultSlot);
            slotMap[resultSlot] =
                makeE<EIf>(makeFunction("exists", makeVariable(windowSlot)),
                           makeFunction("doubleDoubleSumFinalize", makeVariable(windowSlot)),
                           makeConstant(value::TypeTags::NumberInt32, 0));
        }
        stage = makeS<ProjectStage>(std::move(stage), std::move(slotMap), kEmptyPlanNodeId);
        return {std::move(stage), std::move(resultSlots)};
    }
};

TEST_F(WindowStageTest, WindowTest) {
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
        {boundSlot1, -2, 2},
        {boundSlot2, -2, 2},
        {boundSlot1, boost::none, 0},
        {boundSlot1, 0, boost::none},
        {boundSlot1, -6, -2},
        {boundSlot1, 2, 6},
    };
    auto [resultStage, resultSlots] = createSimpleWindowStage(std::move(inputStage),
                                                              std::move(partitionSlots),
                                                              std::move(forwardSlots),
                                                              valueSlot,
                                                              std::move(windowOffsets));
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
        {100, 300, 600, 1000, 1500, 100, 300, 600, 1000, 1500},
        {1500, 1400, 1200, 900, 500, 1500, 1400, 1200, 900, 500},
        {0, 100, 300, 600, 900, 0, 100, 300, 600, 900},
        {900, 1200, 900, 500, 0, 900, 1200, 900, 500, 0}};
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
}  // namespace mongo::sbe
