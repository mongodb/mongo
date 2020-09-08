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

class SBECoerceToStringTest : public EExpressionTestFixture {
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

TEST_F(SBECoerceToStringTest, BasicCoerceToString) {
    value::OwnedValueAccessor coerceToStringAccessor;
    auto coerceToStringSlot = bindAccessor(&coerceToStringAccessor);
    auto coerceToStringExpr = sbe::makeE<sbe::EFunction>(
        "coerceToString", sbe::makeEs(makeE<EVariable>(coerceToStringSlot)));
    auto compiledExpr = compileExpression(*coerceToStringExpr);

    // Int32_t test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberInt32,
                                 value::bitcastFrom<int32_t>(42));
    runAndAssertExpression(compiledExpr.get(), "42");

    // Int64_t test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberInt32,
                                 value::bitcastFrom<int64_t>(42));
    runAndAssertExpression(compiledExpr.get(), "42");

    // Double test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::NumberDouble,
                                 value::bitcastFrom<double>(42.213));
    runAndAssertExpression(compiledExpr.get(), "42.213");

    // Decimal test.
    auto [decTag, decVal] = value::makeCopyDecimal(Decimal128(42.213));
    coerceToStringAccessor.reset(decTag, decVal);
    runAndAssertExpression(compiledExpr.get(), "42.2130000000000");

    // BSONString test.
    auto bsonString = BSON("string"
                           << "hello");
    auto bsonStringVal = value::bitcastFrom(bsonString["string"].value());
    coerceToStringAccessor.reset(value::TypeTags::bsonString, bsonStringVal);
    runAndAssertExpression(compiledExpr.get(), "hello");

    // Date test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Date, value::bitcastFrom<uint64_t>(4400));
    runAndAssertExpression(compiledExpr.get(), "1970-01-01T00:00:04.400Z");

    // TimeStamp test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Timestamp,
                                 value::bitcastFrom<uint64_t>(17179869186));
    Timestamp ts(17179869186);
    runAndAssertExpression(compiledExpr.get(), ts.toString());

    // Nothing test.
    coerceToStringAccessor.reset(sbe::value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledExpr.get());

    // BSONObj test.
    auto bsonObj = BSON("number" << 42);
    auto bsonNum = value::bitcastFrom(bsonObj["number"].value());
    auto [bsonTag, bsonVal] = value::copyValue(value::TypeTags::bsonObject, bsonNum);
    coerceToStringAccessor.reset(bsonTag, bsonVal);
    runAndAssertNothing(compiledExpr.get());

    // Array test.
    auto [arrTag, arrVal] = value::makeNewArray();
    coerceToStringAccessor.reset(arrTag, arrVal);
    runAndAssertNothing(compiledExpr.get());

    // ArraySet test.
    auto [arrSetTag, arrSetVal] = value::makeNewArraySet();
    coerceToStringAccessor.reset(arrSetTag, arrSetVal);
    runAndAssertNothing(compiledExpr.get());
}

}  // namespace mongo::sbe
