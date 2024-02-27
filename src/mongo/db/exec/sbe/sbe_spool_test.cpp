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

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

class SbeSpoolTest : public PlanStageTestFixture {
public:
    /**
     * Given an input subtree 'outerBranch' and a 'spoolId', constructs a plan of the following
     * shape:
     *   nlj
     *     left
     *       <outerBranch>
     *     right
     *       [c|s]spool spoolId
     *
     * The spool may be either a stack spool or regular (non-stack) spool, depending on the value of
     * the template parameter.
     */
    template <bool IsStack>
    std::pair<value::SlotId, std::unique_ptr<PlanStage>> makeSpoolConsumer(
        std::unique_ptr<PlanStage> outerBranch, SpoolId spoolId) {
        auto spoolOutputSlot = generateSlotId();
        auto spoolConsumer =
            makeS<SpoolConsumerStage<IsStack>>(spoolId, makeSV(spoolOutputSlot), kEmptyPlanNodeId);

        auto loopJoin = makeS<LoopJoinStage>(std::move(outerBranch),
                                             std::move(spoolConsumer),
                                             makeSV(),
                                             makeSV(),
                                             nullptr,
                                             kEmptyPlanNodeId);
        return std::make_pair(spoolOutputSlot, std::move(loopJoin));
    }

    /**
     * Constructs the following plan tree:
     *
     *   nlj
     *     left
     *       limit 1 -> espool -> mock scan
     *     right
     *       cspool
     *
     * In other words, the outer branch spools the mock input collection. The inner branch returns
     * the data after unspooling it.
     */
    std::pair<value::SlotId, std::unique_ptr<PlanStage>> makeSpoolUnspoolPlan(
        value::SlotId mockScanSlot, std::unique_ptr<PlanStage> mockScanStage) {
        auto spoolId = generateSpoolId();
        std::unique_ptr<PlanStage> spoolProducer = makeS<SpoolEagerProducerStage>(
            std::move(mockScanStage), spoolId, makeSV(mockScanSlot), kEmptyPlanNodeId);

        auto outerBranch = makeS<LimitSkipStage>(std::move(spoolProducer),
                                                 makeE<EConstant>(value::TypeTags::NumberInt64, 1),
                                                 nullptr,
                                                 kEmptyPlanNodeId);

        return makeSpoolConsumer<false>(std::move(outerBranch), spoolId);
    }
};

TEST_F(SbeSpoolTest, SpoolEagerProducerBasic) {
    auto inputArray = BSON_ARRAY("a"
                                 << "b"
                                 << "c");
    auto [inputTag, inputVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard inputGuard{inputTag, inputVal};

    // We expect the input to be returned unchanged after being buffered in the spool and then
    // returned in FIFO order.
    auto [expectedTag, expectedVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [this](value::SlotId mockScanSlot,
                              std::unique_ptr<PlanStage> mockScanStage) {
        auto eagerSpoolProducer = makeS<SpoolEagerProducerStage>(
            std::move(mockScanStage), generateSpoolId(), makeSV(mockScanSlot), kEmptyPlanNodeId);
        return std::make_pair(mockScanSlot, std::move(eagerSpoolProducer));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(SbeSpoolTest, SpoolLazyProducerBasic) {
    auto inputArray = BSON_ARRAY("a"
                                 << "b"
                                 << "c");
    auto [inputTag, inputVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard inputGuard{inputTag, inputVal};

    // We expect the input to be returned unchanged since it is returned as it is being buffered in
    // the spool.
    auto [expectedTag, expectedVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto makeStageFn = [this](value::SlotId mockScanSlot,
                              std::unique_ptr<PlanStage> mockScanStage) {
        auto lazySpoolProducer = makeS<SpoolLazyProducerStage>(std::move(mockScanStage),
                                                               generateSpoolId(),
                                                               makeSV(mockScanSlot),
                                                               nullptr,
                                                               kEmptyPlanNodeId);
        return std::make_pair(mockScanSlot, std::move(lazySpoolProducer));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(SbeSpoolTest, SpoolAndConsumeNonStack) {
    auto inputArray = BSON_ARRAY("a"
                                 << "b"
                                 << "c");
    auto [inputTag, inputVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard inputGuard{inputTag, inputVal};

    // We expect the input to be returned unchanged after being buffered in the spool and then
    // consumed in FIFO order.
    auto [expectedTag, expectedVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag,
            inputVal,
            expectedTag,
            expectedVal,
            [this](value::SlotId mockScanSlot, std::unique_ptr<PlanStage> mockScanStage) {
                return makeSpoolUnspoolPlan(mockScanSlot, std::move(mockScanStage));
            });
}

TEST_F(SbeSpoolTest, SpoolAndConsumeStack) {
    auto inputArray = BSON_ARRAY("a"
                                 << "b"
                                 << "c");
    auto [inputTag, inputVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard inputGuard{inputTag, inputVal};

    // We expect the input to be returned unchanged after being buffered in the spool and then
    // consumed in FIFO order.
    auto [expectedTag, expectedVal] = stage_builder::makeValue(inputArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    inputGuard.reset();
    expectedGuard.reset();
    runTest(
        inputTag,
        inputVal,
        expectedTag,
        expectedVal,
        [this](value::SlotId mockScanSlot, std::unique_ptr<PlanStage> mockScanStage) {
            // Constructs a plan like the following which uses a lazy spool producer and stack spool
            // consumer to feed the data through the spool before returning it.
            //
            //  nlj
            //    left
            //      lspool -> mock scan
            //    right
            //      sspool
            auto spoolId = generateSpoolId();
            auto spoolProducer = makeS<SpoolLazyProducerStage>(
                std::move(mockScanStage), spoolId, makeSV(mockScanSlot), nullptr, kEmptyPlanNodeId);
            return makeSpoolConsumer<true>(std::move(spoolProducer), spoolId);
        });
}

/**
 * Tests the following execution plan:
 *
 * nlj [] []
 *   left
 *       lspool sp1 [unionOutputSlot]
 *       union [unionOutputSlot] [
 *         branch1
 *           mock scan of ["a", "b", "c"]
 *         branch2
 *           mock scan of ["d", "e", "f"]
 *      ]
 *   right
 *       sspool sp1 [outputSlot]
 *
 * The plan should return the sequence "a", "b", "c", "d", "e", "f" from 'outputSlot', but it does
 * so by feeding each of these values through the spool 'sp1'. On each getNext(), the lazy spool
 * consumer adds the next element to the spool, which is then pulled out of the spool by the stack
 * spool consumer.
 *
 * We test that this plan works as expected even after being closed and re-opened mid-execution.
 * This test was designed to reproduce SERVER-56132.
 */
TEST_F(SbeSpoolTest, SpoolAndConsumeCloseAndReopen) {
    auto ctx = makeCompileCtx();

    auto inputArray1 = BSON_ARRAY("a"
                                  << "b"
                                  << "c");
    auto [inputTag1, inputVal1] = stage_builder::makeValue(inputArray1);
    value::ValueGuard inputGuard1{inputTag1, inputVal1};

    auto inputArray2 = BSON_ARRAY("d"
                                  << "e"
                                  << "f");
    auto [inputTag2, inputVal2] = stage_builder::makeValue(inputArray2);
    value::ValueGuard inputGuard2{inputTag2, inputVal2};

    // Generate mock scans for each of the two inputs.
    inputGuard1.reset();
    auto [scanSlot1, scanStage1] = generateVirtualScan(inputTag1, inputVal1);
    inputGuard2.reset();
    auto [scanSlot2, scanStage2] = generateVirtualScan(inputTag2, inputVal2);

    // Union the two mock scans.
    auto unionOutputSlot = generateSlotId();
    auto unionStage =
        makeS<UnionStage>(makeSs(std::move(scanStage1), std::move(scanStage2)),
                          makeVector<value::SlotVector>(makeSV(scanSlot1), makeSV(scanSlot2)),
                          makeSV(unionOutputSlot),
                          kEmptyPlanNodeId);

    // The union stage feeds a lazy spool consumer.
    auto spoolId = generateSpoolId();
    std::unique_ptr<PlanStage> spoolProducer = makeS<SpoolLazyProducerStage>(
        std::move(unionStage), spoolId, makeSV(unionOutputSlot), nullptr, kEmptyPlanNodeId);

    auto [outputSlot, rootStage] = makeSpoolConsumer<true>(std::move(spoolProducer), spoolId);

    auto accessor = prepareTree(ctx.get(), rootStage.get(), outputSlot);

    // Partially execute the plan, ensuring that we have started to execute the second branch of the
    // union.
    for (int i = 0; i < 4; ++i) {
        auto planState = rootStage->getNext();
        ASSERT(planState == PlanState::ADVANCED);
        auto [tag, val] = accessor->getViewOfValue();
        char expectedChar = 'a' + i;
        auto [expectedTag, expectedValue] =
            value::makeNewString(StringData{std::string(1, expectedChar)});
        value::ValueGuard expectedGuard{expectedTag, expectedValue};
        ASSERT_TRUE(valueEquals(tag, val, expectedTag, expectedValue));
    }

    // Before the plan reaches EOF, close and re-open it.
    rootStage->close();
    rootStage->open(false);

    // This time, execute the plan until it reaches EOF. It should return the results as expected
    // after being closed and re-opened.
    auto [resultsTag, resultsVal] = getAllResults(rootStage.get(), accessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    auto expectedResultsArray = BSON_ARRAY("a"
                                           << "b"
                                           << "c"
                                           << "d"
                                           << "e"
                                           << "f");
    auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedResultsArray);
    value::ValueGuard expectedGuard{expectedTag, expectedVal};
    assertValuesEqual(resultsTag, resultsVal, expectedTag, expectedVal);
}

}  // namespace mongo::sbe
