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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::sbe {

// Equivalent bson objects and arrays
// {"field1": 1, "field2" : {"innerField": 2}}
const BSONObj bsonObj = BSON("field1" << 1 << "field2" << BSON("innerField" << 2));
// [{"k" : "field1", "v" : 1}, {"k" : "field2", "v" :{"innerField": 2}}]
const BSONArray bsonArr1 = BSON_ARRAY(BSON("k"
                                           << "field1"
                                           << "v" << 1)
                                      << BSON("k"
                                              << "field2"
                                              << "v" << BSON("innerField" << 2)));
// [["field1", 1], ["field2", {"innerField": 2}]]
const BSONArray bsonArr2 =
    BSON_ARRAY(BSON_ARRAY("field1" << 1) << BSON_ARRAY("field2" << BSON("innerField" << 2)));
;

class SBEObjectArrayConversionTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                value::TypeTags expTag,
                                value::TypeTags equivalentExpTag,
                                value::Value equivalentExpVal) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQ(expTag, runTag);

        auto [compareTag, compareVal] =
            value::compareValue(equivalentExpTag, equivalentExpVal, runTag, runVal);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(value::bitcastTo<int32_t>(compareVal), 0);
    }

    void runAndAssertErrorCode(const vm::CodeFragment* compiledExpr, int expErrCode) {
        Status status = [&]() {
            try {
                auto [runTag, runVal] = runCompiledExpression(compiledExpr);
                value::ValueGuard guard(runTag, runVal);
                return Status::OK();
            } catch (AssertionException& ex) {
                return ex.toStatus();
            }
        }();
        ASSERT_FALSE(status.isOK());
        ASSERT_EQ(status.code(), expErrCode);
    }

    std::pair<value::TypeTags, value::Value> convertFromBSONObj(BSONObj obj) {
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : obj) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            objView->push_back(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }

    std::pair<value::TypeTags, value::Value> convertFromBSONArray(BSONArray arr) {
        auto [arrTag, arrVal] = value::makeNewArray();
        auto arrView = value::getArrayView(arrVal);

        for (auto elem : arr) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            arrView->push_back(tag, val);
        }
        return {arrTag, arrVal};
    }
};

TEST_F(SBEObjectArrayConversionTest, ObjectToArrayExpression) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto objectToArrayExpr =
        sbe::makeE<sbe::EFunction>("objectToArray", sbe::makeEs(makeE<EVariable>(inputSlot)));
    auto compiledObjectToArray = compileExpression(*objectToArrayExpr);

    // Test on Object input
    auto [objTag, objVal] = convertFromBSONObj(bsonObj);

    inputAccessor.reset(true, objTag, objVal);
    runAndAssertExpression(compiledObjectToArray.get(),
                           value::TypeTags::Array,
                           value::TypeTags::bsonArray,
                           value::bitcastFrom<const char*>(bsonArr1.objdata()));

    // Test similarly on a bsonObject input
    inputAccessor.reset(
        false, value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bsonObj.objdata()));
    runAndAssertExpression(compiledObjectToArray.get(),
                           value::TypeTags::Array,
                           value::TypeTags::bsonArray,
                           value::bitcastFrom<const char*>(bsonArr1.objdata()));

    // Test with empty object
    auto [emptyObjTag, emptyObjVal] = value::makeNewObject();
    inputAccessor.reset(true, emptyObjTag, emptyObjVal);
    auto [emptyArrTag, emptyArrVal] = value::makeNewArray();
    value::ValueGuard guard(emptyArrTag, emptyArrVal);
    runAndAssertExpression(
        compiledObjectToArray.get(), value::TypeTags::Array, emptyArrTag, emptyArrVal);

    // Test when input is not object Type
    inputAccessor.reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledObjectToArray.get());
}

TEST_F(SBEObjectArrayConversionTest, ArrayToObjectExpression) {
    value::OwnedValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);

    auto arrayToObjectExpr =
        sbe::makeE<sbe::EFunction>("arrayToObject", sbe::makeEs(makeE<EVariable>(inputSlot)));
    auto compiledArrayToObject = compileExpression(*arrayToObjectExpr);

    // Test with Array on first variant
    auto [arr1Tag, arr1Val] = convertFromBSONArray(bsonArr1);
    inputAccessor.reset(true, arr1Tag, arr1Val);
    runAndAssertExpression(compiledArrayToObject.get(),
                           value::TypeTags::Object,
                           value::TypeTags::bsonObject,
                           value::bitcastFrom<const char*>(bsonObj.objdata()));

    // Test with bsonArray on first variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr1.objdata()));
    runAndAssertExpression(compiledArrayToObject.get(),
                           value::TypeTags::Object,
                           value::TypeTags::bsonObject,
                           value::bitcastFrom<const char*>(bsonObj.objdata()));

    // Test with Array on second variant
    auto [arr2Tag, arr2Val] = convertFromBSONArray(bsonArr2);
    inputAccessor.reset(true, arr2Tag, arr2Val);
    runAndAssertExpression(compiledArrayToObject.get(),
                           value::TypeTags::Object,
                           value::TypeTags::bsonObject,
                           value::bitcastFrom<const char*>(bsonObj.objdata()));

    // Test with bsonArray on second variant
    inputAccessor.reset(
        false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bsonArr2.objdata()));
    runAndAssertExpression(compiledArrayToObject.get(),
                           value::TypeTags::Object,
                           value::TypeTags::bsonObject,
                           value::bitcastFrom<const char*>(bsonObj.objdata()));

    // Test with empty array
    auto [emptyArrTag, emptyArrVal] = value::makeNewArray();
    inputAccessor.reset(true, emptyArrTag, emptyArrVal);
    auto [emptyObjTag, emptyObjVal] = value::makeNewObject();
    value::ValueGuard guard(emptyObjTag, emptyObjVal);
    runAndAssertExpression(
        compiledArrayToObject.get(), value::TypeTags::Object, emptyObjTag, emptyObjVal);

    // Test when input is not array Type
    inputAccessor.reset(false, value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledArrayToObject.get());

    // Test error conditions
    std::string strWithNullByte{0x1B, 0x54, 0x00, 0x32};
    auto
        errInputs =
            {
                BSON_ARRAY("elem1"
                           << "elem2"),
                BSON_ARRAY(BSON_ARRAY("field1" << 1) << BSON("k"
                                                             << "field2"
                                                             << "v" << 2)),
                BSON_ARRAY(BSONArrayBuilder().arr()),
                BSON_ARRAY(BSON_ARRAY(1 << "field1")),
                BSON_ARRAY(BSON_ARRAY("field1")),
                BSON_ARRAY(BSON_ARRAY("field1" << 1 << "dummy")),
                BSON_ARRAY(BSON_ARRAY(strWithNullByte << 1)),
                BSON_ARRAY(BSON("k"
                                << "field2"
                                << "v" << 2)
                           << BSON_ARRAY("field1" << 1)),
                BSON_ARRAY(BSONObjBuilder().obj()),
                BSON_ARRAY(BSON("k"
                                << "field2")),
                BSON_ARRAY(BSON("k"
                                << "field2"
                                << "v" << 2 << "dummy" << 3)),
                BSON_ARRAY(BSON("x"
                                << "field2"
                                << "y" << 2)),
                BSON_ARRAY(BSON("k" << 1 << "v" << 2)),
                BSON_ARRAY(BSON("k" << strWithNullByte << "v" << 2)),
            };

    auto errCodes = {
        5153201,
        5153202,
        5153203,
        5153204,
        5153205,
        5153206,
        5153207,
        5153208,
        5153209,
        5153210,
        5153211,
        5153212,
        5153213,
        5153214,
    };

    auto errIn = errInputs.begin();
    auto errCode = errCodes.begin();
    for (; errIn != errInputs.end(); errIn++, errCode++) {
        inputAccessor.reset(
            false, value::TypeTags::bsonArray, value::bitcastFrom<const char*>(errIn->objdata()));
        runAndAssertErrorCode(compiledArrayToObject.get(), *errCode);
    }
}
}  // namespace mongo::sbe
