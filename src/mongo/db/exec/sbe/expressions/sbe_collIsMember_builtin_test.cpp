// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <tuple>
#include <utility>


namespace mongo::sbe {

class SBEBuiltinInTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::SlotId inputSlot,
                                std::pair<value::TypeTags, value::Value> inArray,
                                bool expectedRes) {
        auto inExpr = makeE<EFunction>(
            EFn::kIsMember,
            makeEs(makeE<EVariable>(inputSlot), makeE<EConstant>(inArray.first, inArray.second)));
        auto compiledExpr = compileExpression(*inExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    void runAndAssertExpressionWithCollator(value::SlotId inputSlot,
                                            std::pair<value::TypeTags, value::Value> inArray,
                                            value::SlotId collatorSlot,
                                            bool expectedRes) {
        auto inExpr = makeE<EFunction>(EFn::kCollIsMember,
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
