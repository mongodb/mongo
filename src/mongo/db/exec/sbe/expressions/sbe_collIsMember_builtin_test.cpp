/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

#include <boost/optional.hpp>

namespace mongo::sbe {

class SBEBuiltinInTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::SlotId inputSlot,
                                std::pair<value::TypeTags, value::Value> inArray,
                                bool expectedRes) {
        auto inExpr = makeE<EFunction>(
            "isMember",
            makeEs(makeE<EVariable>(inputSlot), makeE<EConstant>(inArray.first, inArray.second)));
        auto compiledExpr = compileExpression(*inExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    void runAndAssertExpressionWithCollator(value::SlotId inputSlot,
                                            std::pair<value::TypeTags, value::Value> inArray,
                                            value::SlotId collatorSlot,
                                            bool expectedRes) {
        auto inExpr = makeE<EFunction>("collIsMember",
                                       makeEs(makeE<EVariable>(inputSlot),
                                              makeE<EConstant>(inArray.first, inArray.second),
                                              makeE<EVariable>(collatorSlot)));
        auto compiledExpr = compileExpression(*inExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    std::pair<value::TypeTags, value::Value> makeViewOfObject(const BSONObj& obj) {
        return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())};
    }
};

TEST_F(SBEBuiltinInTest, inArraySet) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY("foo" << "bar")), false);
}

TEST_F(SBEBuiltinInTest, inArraySetWithCollator) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);
    value::ViewOfValueAccessor collatorAccessor;
    auto collatorSlot = bindAccessor(&collatorAccessor);
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.get()));

    auto collator1 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    auto [smallStrTag, smallStrVal] = value::makeNewString("FOO");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator1.get()),
        collatorSlot,
        true);

    auto collator2 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(inputSlot,
                                       makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"),

                                                                collator2.get()),
                                       collatorSlot,
                                       true);

    auto collator3 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator3.get()),
        collatorSlot,
        false);

    auto collator4 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("oof" << "bar"), collator4.get()),
        collatorSlot,
        false);
}

TEST_F(SBEBuiltinInTest, inArray) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY("foo" << "bar")), false);
}

TEST_F(SBEBuiltinInTest, inArrayWithCollator) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);
    value::ViewOfValueAccessor collatorAccessor;
    auto collatorSlot = bindAccessor(&collatorAccessor);
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.get()));

    auto collator1 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    auto [smallStrTag, smallStrVal] = value::makeNewString("FOO");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator1.get()),
        collatorSlot,
        true);

    auto collator2 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator2.get()),
        collatorSlot,
        true);

    auto collator3 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator3.get()),
        collatorSlot,
        false);

    auto collator4 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("oof" << "bar"), collator4.get()),
        collatorSlot,
        false);
}

TEST_F(SBEBuiltinInTest, inBSONArray) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY("foo" << "bar")), false);
}

TEST_F(SBEBuiltinInTest, inBSONArrayWithCollator) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);
    value::ViewOfValueAccessor collatorAccessor;
    auto collatorSlot = bindAccessor(&collatorAccessor);
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    collatorAccessor.reset(value::TypeTags::collator,
                           value::bitcastFrom<CollatorInterface*>(collator.get()));

    auto collator1 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    auto [smallStrTag, smallStrVal] = value::makeNewString("FOO");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator1.get()),
        collatorSlot,
        true);

    auto collator2 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator2.get()),
        collatorSlot,
        true);

    auto collator3 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("foo" << "bar"), collator3.get()),
        collatorSlot,
        false);

    auto collator4 =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    std::tie(smallStrTag, smallStrVal) = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpressionWithCollator(
        inputSlot,
        makeArraySetWithCollator(BSON_ARRAY("oof" << "bar"), collator4.get()),
        collatorSlot,
        false);
}
}  // namespace mongo::sbe
