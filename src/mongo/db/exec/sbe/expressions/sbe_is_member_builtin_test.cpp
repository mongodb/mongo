/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::sbe {

class SBEBuiltinIsMemberTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::SlotId inputSlot,
                                std::pair<value::TypeTags, value::Value> inArray,
                                bool expectedRes) {
        auto isMemberExpr = makeE<EFunction>(
            "isMember",
            makeEs(makeE<EVariable>(inputSlot), makeE<EConstant>(inArray.first, inArray.second)));
        auto compiledExpr = compileExpression(*isMemberExpr);
        auto actualRes = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT_EQ(actualRes, expectedRes);
    }

    std::pair<value::TypeTags, value::Value> makeValue(const BSONArray& ba) {
        int numBytes = ba.objsize();
        uint8_t* data = new uint8_t[numBytes];
        memcpy(data, reinterpret_cast<const uint8_t*>(ba.objdata()), numBytes);
        return {value::TypeTags::bsonArray, value::bitcastFrom(data)};
    }

    std::pair<value::TypeTags, value::Value> makeViewOfObject(const BSONObj& obj) {
        return {value::TypeTags::bsonObject, value::bitcastFrom(obj.objdata())};
    }

    std::pair<value::TypeTags, value::Value> makeArraySet(const BSONArray& arr) {
        auto [tmpTag, tmpVal] = makeValue(arr);
        value::ValueGuard tmpGuard{tmpTag, tmpVal};

        value::ArrayEnumerator enumerator{tmpTag, tmpVal};

        auto [arrTag, arrVal] = value::makeNewArraySet();
        value::ValueGuard guard{arrTag, arrVal};

        auto arrView = value::getArraySetView(arrVal);

        while (!enumerator.atEnd()) {
            auto [tag, val] = enumerator.getViewOfValue();
            enumerator.advance();

            auto [copyTag, copyVal] = value::copyValue(tag, val);
            arrView->push_back(copyTag, copyVal);
        }
        guard.reset();

        return {arrTag, arrVal};
    }

    std::pair<value::TypeTags, value::Value> makeArray(const BSONArray& arr) {
        auto [tmpTag, tmpVal] = makeValue(arr);
        value::ValueGuard tmpGuard{tmpTag, tmpVal};

        value::ArrayEnumerator enumerator{tmpTag, tmpVal};

        auto [arrTag, arrVal] = value::makeNewArray();
        value::ValueGuard guard{arrTag, arrVal};

        auto arrView = value::getArrayView(arrVal);

        while (!enumerator.atEnd()) {
            auto [tag, val] = enumerator.getViewOfValue();
            enumerator.advance();

            auto [copyTag, copyVal] = value::copyValue(tag, val);
            arrView->push_back(copyTag, copyVal);
        }
        guard.reset();

        return {arrTag, arrVal};
    }
};

TEST_F(SBEBuiltinIsMemberTest, IsMemberArraySet) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that isMember can find basic values.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 1);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 1);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 3);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArraySet(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeSmallString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeArraySet(BSON_ARRAY("foo"
                                                   << "bar")),
                           true);

    std::tie(smallStrTag, smallStrVal) = value::makeSmallString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeArraySet(BSON_ARRAY("foo"
                                                   << "bar")),
                           false);

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
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 1);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 1);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 3);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeArray(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeSmallString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeArray(BSON_ARRAY("foo"
                                                << "bar")),
                           true);

    std::tie(smallStrTag, smallStrVal) = value::makeSmallString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeArray(BSON_ARRAY("foo"
                                                << "bar")),
                           false);

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
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 1);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 2)), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 3);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(BSONObj())), false);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 1);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 2)), true);

    inputSlotAccessor.reset(value::TypeTags::NumberInt64, 3);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 2)), false);

    auto [decimalTag, decimalVal] = value::makeCopyDecimal(Decimal128{9});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 9.0)), true);

    std::tie(decimalTag, decimalVal) = value::makeCopyDecimal(Decimal128{0.1});
    inputSlotAccessor.reset(decimalTag, decimalVal);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(1 << 9.0)), false);

    auto [smallStrTag, smallStrVal] = value::makeSmallString("foo");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeValue(BSON_ARRAY("foo"
                                                << "bar")),
                           true);

    std::tie(smallStrTag, smallStrVal) = value::makeSmallString("baz");
    inputSlotAccessor.reset(smallStrTag, smallStrVal);
    runAndAssertExpression(inputSlot,
                           makeValue(BSON_ARRAY("foo"
                                                << "bar")),
                           false);

    // Test that isMember can find composite values.
    auto [arrTag, arrVal] = makeArray(BSON_ARRAY(2 << 3));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(BSON_ARRAY(2 << 3) << 1)), true);

    std::tie(arrTag, arrVal) = makeArray(BSON_ARRAY(1));
    inputSlotAccessor.reset(arrTag, arrVal);
    runAndAssertExpression(inputSlot, makeValue(BSON_ARRAY(BSON_ARRAY(1 << 2) << 3)), false);

    value::ViewOfValueAccessor bsonObjAccessor;
    auto bsonObjSlot = bindAccessor(&bsonObjAccessor);

    auto targetObj = BSON("a" << 1);
    auto [targetTag, targetVal] = makeViewOfObject(targetObj);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeValue(BSON_ARRAY(1 << BSON("a" << 1))), true);

    bsonObjAccessor.reset(targetTag, targetVal);
    runAndAssertExpression(bsonObjSlot, makeValue(BSON_ARRAY(10 << BSON("b" << 1))), false);
}


TEST_F(SBEBuiltinIsMemberTest, IsMemberReturnsNothing) {
    value::OwnedValueAccessor inputSlotAccessor;
    auto inputSlot = bindAccessor(&inputSlotAccessor);

    // Test that invocation of 'isMember' returns nothing if the second argument isn't an array.
    inputSlotAccessor.reset(value::TypeTags::NumberInt32, 1);
    runAndAssertExpression(inputSlot, {value::TypeTags::NumberInt32, 0}, false);
}
}  // namespace mongo::sbe
