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
#include "mongo/db/query/datetime/date_time_support.h"

namespace mongo::sbe {

class SBEToUpperToLowerTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const std::string& expectedString) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(value::getStringView(runTag, runVal), expectedString);
    }
};

TEST_F(SBEToUpperToLowerTest, BasicToUpper) {
    value::OwnedValueAccessor toUpperAccessor;
    auto toUpperSlot = bindAccessor(&toUpperAccessor);
    auto toUpperExpr =
        sbe::makeE<sbe::EFunction>("toUpper", sbe::makeEs(makeE<EVariable>(toUpperSlot)));
    auto compiledExpr = compileExpression(*toUpperExpr);

    // SmallString test.
    auto [strTag, strVal] = sbe::value::makeNewString("hello");
    toUpperAccessor.reset(strTag, strVal);
    runAndAssertExpression(compiledExpr.get(), "HELLO");

    // BigString test.
    auto [strTag2, strVal2] = sbe::value::makeNewString("abcdefgHIJKLMNOPQRStuvwxyz123456789");
    toUpperAccessor.reset(strTag2, strVal2);
    runAndAssertExpression(compiledExpr.get(), "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456789");

    // BSONString test.
    auto bsonStringObj = BSON("string"
                              << "hello");
    auto bsonStringVal = value::bitcastFrom(bsonStringObj["string"].value());
    toUpperAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "HELLO");

    // Int32_t test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Int64_t test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Double test.
    toUpperAccessor.reset(sbe::value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledExpr.get());

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    toUpperAccessor.reset(decTag, decVal);
    runAndAssertNothing(compiledExpr.get());

    // Date test.
    toUpperAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<uint64_t>(4400));
    runAndAssertNothing(compiledExpr.get());

    // Timestamp test.
    toUpperAccessor.reset(sbe::value::TypeTags::Timestamp,
                          value::bitcastFrom<uint64_t>(17179869186));
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonNum = value::bitcastFrom(bsonObj["number"].value());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonNum);
    toUpperAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    toUpperAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    toUpperAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

TEST_F(SBEToUpperToLowerTest, BasicToLower) {
    value::OwnedValueAccessor toLowerAccessor;
    auto toLowerSlot = bindAccessor(&toLowerAccessor);
    auto toLowerExpr =
        sbe::makeE<sbe::EFunction>("toLower", sbe::makeEs(makeE<EVariable>(toLowerSlot)));
    auto compiledExpr = compileExpression(*toLowerExpr);

    // SmallString test.
    auto [strTag, strVal] = sbe::value::makeNewString("HELLO");
    toLowerAccessor.reset(strTag, strVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // BigString test.
    auto [strTag2, strVal2] = sbe::value::makeNewString("abcdefgHIJKLMNOPQRStuvwxyz123456789");
    toLowerAccessor.reset(strTag2, strVal2);
    runAndAssertExpression(compiledExpr.get(), "abcdefghijklmnopqrstuvwxyz123456789");

    // BSONString test.
    auto bsonStringObj = BSON("string"
                              << "HELLO");
    auto bsonStringVal = value::bitcastFrom(bsonStringObj["string"].value());
    toLowerAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // Int32_t test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Int64_t test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledExpr.get());

    // Double test.
    toLowerAccessor.reset(sbe::value::TypeTags::NumberDouble, value::bitcastFrom<double>(42.213));
    runAndAssertNothing(compiledExpr.get());

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    toLowerAccessor.reset(decTag, decVal);
    runAndAssertNothing(compiledExpr.get());

    // Date test.
    toLowerAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<uint64_t>(4400));
    runAndAssertNothing(compiledExpr.get());

    // Timestamp test.
    toLowerAccessor.reset(sbe::value::TypeTags::Timestamp,
                          value::bitcastFrom<uint64_t>(17179869186));
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonNum = value::bitcastFrom(bsonObj["number"].value());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonNum);
    toLowerAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    toLowerAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    toLowerAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
