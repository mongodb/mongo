// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinIsMemberTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::SlotId inputSlot,
                                std::pair<value::TypeTags, value::Value> inArray,
                                bool expectedRes) {
        auto isMemberExpr = makeE<EFunction>(
            EFn::kIsMember,
            makeEs(makeE<EVariable>(inputSlot), makeE<EConstant>(inArray.first, inArray.second)));
        auto compiledExpr = compileExpression(*isMemberExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    std::pair<value::TypeTags, value::Value> makeViewOfObject(const BSONObj& obj) {
        return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(obj.objdata())};
    }
};

TEST_F(SBEBuiltinIsMemberTest, IsMemberArraySet) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that isMember can find basic values.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY("foo" << "bar")), false);

    // Test that isMember can find composite values.
    auto [arrTag, arrVal] = makeArray(BSON_ARRAY(2 << 3));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(BSON_ARRAY(2 << 3) << 1)), true);

    std::tie(arrTag, arrVal) = makeArray(BSON_ARRAY(1));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(BSON_ARRAY(1 << 2) << 3)), false);

    value::ViewOfValueAccessor bsonObjAccessor;
    auto bsonObjSlot = bindAccessor(&bsonObjAccessor);

    auto targetObj = BSON("a" << 1);
    auto [targetTag, targetVal] = makeViewOfObject(targetObj);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeArraySet(BSON_ARRAY(1 << BSON("a" << 1))), true);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeArraySet(BSON_ARRAY(10 << BSON("b" << 1))), false);
}

TEST_F(SBEBuiltinIsMemberTest, IsMemberArray) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that isMember can find basic values.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY("foo" << "bar")), false);

    // Test that isMember can find composite values.
    auto [arrTag, arrVal] = makeArray(BSON_ARRAY(2 << 3));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(BSON_ARRAY(2 << 3) << 1)), true);

    std::tie(arrTag, arrVal) = makeArray(BSON_ARRAY(1));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(BSON_ARRAY(1 << 2) << 3)), false);

    value::ViewOfValueAccessor bsonObjAccessor;
    auto bsonObjSlot = bindAccessor(&bsonObjAccessor);

    auto targetObj = BSON("a" << 1);
    auto [objTag, objVal] = makeViewOfObject(targetObj);
    bsonObjAccessor.reset(objTag, objVal);
    runAndAssertExpression(bsonObjSlot, makeArray(BSON_ARRAY(BSON("a" << 1) << 2)), true);

    targetObj = BSON("a" << 1);
    std::tie(objTag, objVal) = makeViewOfObject(targetObj);
    bsonObjAccessor.reset(objTag, objVal);
    runAndAssertExpression(bsonObjSlot, makeArray(BSON_ARRAY(BSON("b" << 1))), false);
}

TEST_F(SBEBuiltinIsMemberTest, IsMemberBSONArray) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that isMember can find basic values.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1));
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3));
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeNewString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY("foo" << "bar")), true);

    std::tie(smallStrTag, smallStrVal) = value::makeNewString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY("foo" << "bar")), false);

    // Test that isMember can find composite values.
    auto [arrTag, arrVal] = makeArray(BSON_ARRAY(2 << 3));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(BSON_ARRAY(2 << 3) << 1)), true);

    std::tie(arrTag, arrVal) = makeArray(BSON_ARRAY(1));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeBsonArray(BSON_ARRAY(BSON_ARRAY(1 << 2) << 3)), false);

    value::ViewOfValueAccessor bsonObjAccessor;
    auto bsonObjSlot = bindAccessor(&bsonObjAccessor);

    auto targetObj = BSON("a" << 1);
    auto [targetTag, targetVal] = makeViewOfObject(targetObj);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeBsonArray(BSON_ARRAY(1 << BSON("a" << 1))), true);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeBsonArray(BSON_ARRAY(10 << BSON("b" << 1))), false);
}


TEST_F(SBEBuiltinIsMemberTest, IsMemberReturnsNothing) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that invocation of 'isMember' returns nothing if the second argument isn't an array.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    runAndAssertExpression(
        inputSlot, {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)}, false);
}
}  // namespace mongo::sbe
