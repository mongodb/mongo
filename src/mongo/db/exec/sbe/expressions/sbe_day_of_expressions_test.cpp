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

#include "mongo/bson/oid.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/query/datetime/date_time_support.h"

namespace mongo::sbe {

class SBEDayOfExpressionTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                int32_t dayOfExpressionExpected) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQUALS(value::TypeTags::NumberInt32, runTag);
        ASSERT_EQUALS(dayOfExpressionExpected, value::bitcastTo<int32_t>(runVal));
    }
};

TEST_F(SBEDayOfExpressionTest, BasicDayOfYear) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfYearExpr = sbe::makeE<sbe::EFunction>("dayOfYear",
                                                    sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                                makeE<EVariable>(dateSlot),
                                                                makeE<EVariable>(timezoneSlot)));
    auto compiledDayOfYear = compileExpression(*dayOfYearExpr);

    // Test $dayOfYear returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledDayOfYear.get(), 1);

    // Test $dayOfYear returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfYear.get());

    // Test $dayOfYear returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfYear.get());

    // Test $dayOfYear returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfYear.get());
}

TEST_F(SBEDayOfExpressionTest, BasicDayOfMonth) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfMonthExpr = sbe::makeE<sbe::EFunction>("dayOfMonth",
                                                     sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                                 makeE<EVariable>(dateSlot),
                                                                 makeE<EVariable>(timezoneSlot)));
    auto compiledDayOfMonth = compileExpression(*dayOfMonthExpr);

    // Test $dayOfMonth returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledDayOfMonth.get(), 1);

    // Test $dayOfMonth returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfMonth.get());

    // Test $dayOfMonth returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfMonth.get());

    // Test $dayOfMonth returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfMonth.get());
}

TEST_F(SBEDayOfExpressionTest, BasicDayOfWeek) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfWeekExpr = sbe::makeE<sbe::EFunction>("dayOfWeek",
                                                    sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                                makeE<EVariable>(dateSlot),
                                                                makeE<EVariable>(timezoneSlot)));
    auto compiledDayOfWeek = compileExpression(*dayOfWeekExpr);

    // Test $dayOfWeek returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledDayOfWeek.get(), 5);

    // Test $dayOfWeek returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfWeek.get());

    // Test $dayOfWeek returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfWeek.get());

    // Test $dayOfWeek returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDayOfWeek.get());
}

}  // namespace mongo::sbe
