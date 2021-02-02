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
 * This file contains tests for sbe::FilterStage.
 */

#include "mongo/platform/basic.h"

#include <string_view>

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

namespace mongo::sbe {

using HashAggStageTest = PlanStageTestFixture;

TEST_F(HashAggStageTest, HashAggMinMaxTest) {
    using namespace std::literals;

    BSONArrayBuilder bab1;
    bab1.append("D").append("a").append("F").append("e").append("B").append("c");
    auto [inputTag, inputVal] = stage_builder::makeValue(bab1.arr());
    value::ValueGuard inputGuard{inputTag, inputVal};

    BSONArrayBuilder bab2;
    bab2.append("B").append("e").append("a").append("F");
    auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(bab2.arr()));
    value::ValueGuard expectedGuard{expectedTag, expectedVal};

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto makeStageFn = [this, &collator](value::SlotId scanSlot,
                                         std::unique_ptr<PlanStage> scanStage) {
        auto collExpr = makeE<EConstant>(value::TypeTags::collator,
                                         value::bitcastFrom<CollatorInterface*>(collator.get()));

        // Build a HashAggStage that exercises the collMin() and collMax() aggregate functions.
        auto minSlot = generateSlotId();
        auto maxSlot = generateSlotId();
        auto collMinSlot = generateSlotId();
        auto collMaxSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeEM(minSlot,
                   stage_builder::makeFunction("min", makeE<EVariable>(scanSlot)),
                   maxSlot,
                   stage_builder::makeFunction("max", makeE<EVariable>(scanSlot)),
                   collMinSlot,
                   stage_builder::makeFunction(
                       "collMin", collExpr->clone(), makeE<EVariable>(scanSlot)),
                   collMaxSlot,
                   stage_builder::makeFunction(
                       "collMax", collExpr->clone(), makeE<EVariable>(scanSlot))),
            kEmptyPlanNodeId);

        auto outSlot = generateSlotId();
        auto projectStage =
            makeProjectStage(std::move(hashAggStage),
                             kEmptyPlanNodeId,
                             outSlot,
                             stage_builder::makeFunction("newArray",
                                                         makeE<EVariable>(minSlot),
                                                         makeE<EVariable>(maxSlot),
                                                         makeE<EVariable>(collMinSlot),
                                                         makeE<EVariable>(collMaxSlot)));

        return std::make_pair(outSlot, std::move(projectStage));
    };

    inputGuard.reset();
    expectedGuard.reset();
    runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(HashAggStageTest, HashAggAddToSetTest) {
    using namespace std::literals;

    BSONArrayBuilder bab;
    bab.append("cc").append("BB").append("Aa").append("Bb").append("dD").append("aA");
    bab.append("CC").append("AA").append("Dd").append("cC").append("bb").append("DD");
    auto [inputTag, inputVal] = stage_builder::makeValue(bab.arr());
    value::ValueGuard inputGuard{inputTag, inputVal};

    auto [expectedTag, expectedVal] = value::makeNewArray();
    value::ValueGuard expectedGuard{expectedTag, expectedVal};
    for (auto&& sv : std::array<std::string_view, 4>{"Aa", "BB", "cc", "dD"}) {
        auto [tag, val] = value::makeNewString(sv);
        value::getArrayView(expectedVal)->push_back(tag, val);
    }

    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    auto makeStageFn = [this, &collator](value::SlotId scanSlot,
                                         std::unique_ptr<PlanStage> scanStage) {
        auto collExpr = makeE<EConstant>(value::TypeTags::collator,
                                         value::bitcastFrom<CollatorInterface*>(collator.get()));

        // Build a HashAggStage that exercises the collAddToSet() aggregate function.
        auto hashAggSlot = generateSlotId();
        auto hashAggStage = makeS<HashAggStage>(
            std::move(scanStage),
            makeSV(),
            makeEM(hashAggSlot,
                   stage_builder::makeFunction(
                       "collAddToSet", std::move(collExpr), makeE<EVariable>(scanSlot))),
            kEmptyPlanNodeId);

        return std::make_pair(hashAggSlot, std::move(hashAggStage));
    };

    // Generate a mock scan from 'input' with a single output slot.
    inputGuard.reset();
    auto [scanSlot, scanStage] = generateVirtualScan(inputTag, inputVal);

    // Call the 'makeStage' callback to create the PlanStage that we want to test, passing in
    // the mock scan subtree and its output slot.
    auto [outputSlot, stage] = makeStageFn(scanSlot, std::move(scanStage));

    // Prepare the tree and get the SlotAccessor for the output slot.
    auto ctx = makeCompileCtx();
    auto resultAccessor = prepareTree(ctx.get(), stage.get(), outputSlot);

    // Get all the results produced by the PlanStage we want to test.
    auto [resultsTag, resultsVal] = getAllResults(stage.get(), resultAccessor);
    value::ValueGuard resultGuard{resultsTag, resultsVal};

    // Retrieve the first element from the results array.
    value::ArrayEnumerator resultsEnumerator{resultsTag, resultsVal};
    ASSERT_TRUE(!resultsEnumerator.atEnd());
    auto [elemTag, elemVal] = resultsEnumerator.getViewOfValue();

    // Convert the element into an ArraySet 'as' (with no collation).
    auto [asTag, asVal] = value::arrayToSet(elemTag, elemVal);
    value::ValueGuard asGuard{asTag, asVal};
    ASSERT_TRUE(asTag == value::TypeTags::ArraySet);

    // Assert that 'as' and 'expected' are the same size and contain the same values.
    auto as = value::getArraySetView(asVal);
    size_t expectedSize = 0;
    value::ArrayEnumerator expectedEnumerator{expectedTag, expectedVal};

    for (; !expectedEnumerator.atEnd(); expectedEnumerator.advance()) {
        ASSERT_TRUE(as->values().count(expectedEnumerator.getViewOfValue()));
        ++expectedSize;
    }

    ASSERT_TRUE(as->size() == expectedSize);

    // Assert that the results array does not contain more than one element.
    resultsEnumerator.advance();
    ASSERT_TRUE(resultsEnumerator.atEnd());
}

}  // namespace mongo::sbe
