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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <tuple>

namespace mongo::sbe {

const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();

class SBEDateExpressionAcceptingTimezoneTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr, int32_t expectedResult) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQUALS(value::TypeTags::NumberInt32, runTag);
        ASSERT_EQUALS(expectedResult, value::bitcastTo<int32_t>(runVal));
    }

    void runAndAssertIsoWeekYearExpression(const vm::CodeFragment* compiledExpr,
                                           int64_t expectedResult) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);

        ASSERT_EQUALS(value::TypeTags::NumberInt64, runTag);
        ASSERT_EQUALS(expectedResult, value::bitcastTo<int64_t>(runVal));
    }
};

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicDayOfYear) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfYearExpr = sbe::makeE<sbe::EFunction>("dayOfYear",
                                                    sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                makeE<EVariable>(timezoneDBSlot),
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

    // Test $dayOfYear with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    dayOfYearExpr = sbe::makeE<sbe::EFunction>(
        "dayOfYear", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledDayOfYear = compileExpression(*dayOfYearExpr);

    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledDayOfYear.get(), 1);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicDayOfMonth) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfMonthExpr = sbe::makeE<sbe::EFunction>("dayOfMonth",
                                                     sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                 makeE<EVariable>(timezoneDBSlot),
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

    // Test $dayOfMonth with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    dayOfMonthExpr = sbe::makeE<sbe::EFunction>(
        "dayOfMonth", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledDayOfMonth = compileExpression(*dayOfMonthExpr);

    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledDayOfMonth.get(), 1);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicDayOfWeek) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dayOfWeekExpr = sbe::makeE<sbe::EFunction>("dayOfWeek",
                                                    sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                makeE<EVariable>(timezoneDBSlot),
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

    // Test $dayOfWeek with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    dayOfWeekExpr = sbe::makeE<sbe::EFunction>(
        "dayOfWeek", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledDayOfWeek = compileExpression(*dayOfWeekExpr);

    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledDayOfWeek.get(), 5);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicYear) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    // Test $year with variable timezone
    auto yearExpr = sbe::makeE<sbe::EFunction>("year",
                                               sbe::makeEs(makeE<EVariable>(dateSlot),
                                                           makeE<EVariable>(timezoneDBSlot),
                                                           makeE<EVariable>(timezoneSlot)));
    auto compiledYear = compileExpression(*yearExpr);

    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));

    // Test $year
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 10, 10, 0).toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledYear.get(), 1996);

    // Test $year with epoch start date
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(1970, 1, 1, 0, 0, 0, 0).toMillisSinceEpoch()));
    runAndAssertExpression(compiledYear.get(), 1970);

    // Test $year with earlier than epoch start date
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(1960, 1, 1, 0, 0, 0, 0).toMillisSinceEpoch()));
    runAndAssertExpression(compiledYear.get(), 1960);

    // Test with different year in a different timezone
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(2000, 1, 1, 2, 10, 10, 0).toMillisSinceEpoch()));
    std::tie(timezoneTag, timezoneVal) = value::makeNewString("America/Los_Angeles");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledYear.get(), 1999);

    // Test with different year in a different timezone (as offset)
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(2010, 1, 1, 2, 10, 10, 0).toMillisSinceEpoch()));
    std::tie(timezoneTag, timezoneVal) = value::makeNewString("-08:00");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledYear.get(), 2009);

    // Test $year returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledYear.get());

    // Test $year returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledYear.get());

    // Test $year returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledYear.get());

    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);

    // test $year with constant timezone
    yearExpr = sbe::makeE<sbe::EFunction>(
        "year", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledYear = compileExpression(*yearExpr);

    // Test $year returns the correct value.
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(2023, 7, 18, 10, 10, 10, 0).toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledYear.get(), 2023);

    // Test $year returns Nothing with invalid date
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledYear.get());

    // Test $year returns Nothing with invalid timezone
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneObjAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledYear.get());
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicMonth) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto monthExpr = sbe::makeE<sbe::EFunction>("month",
                                                sbe::makeEs(makeE<EVariable>(dateSlot),
                                                            makeE<EVariable>(timezoneDBSlot),
                                                            makeE<EVariable>(timezoneSlot)));
    auto compiledMonth = compileExpression(*monthExpr);

    // Test $month returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 10, 10, 0).toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledMonth.get(), 7);

    // Test $month returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMonth.get());

    // Test $month returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMonth.get());

    // Test $month returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMonth.get());

    // Test $month with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    monthExpr = sbe::makeE<sbe::EFunction>(
        "month", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledMonth = compileExpression(*monthExpr);

    dateAccessor.reset(
        value::TypeTags::Date,
        value::bitcastFrom<int64_t>(
            kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 10, 10, 0).toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledMonth.get(), 7);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicHour) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto hourExpr = sbe::makeE<sbe::EFunction>("hour",
                                               sbe::makeEs(makeE<EVariable>(dateSlot),
                                                           makeE<EVariable>(timezoneDBSlot),
                                                           makeE<EVariable>(timezoneSlot)));
    auto compiledHour = compileExpression(*hourExpr);

    // Test $hour returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledHour.get(), 10);

    // Test $hour returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledHour.get());

    // Test $hour returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledHour.get());

    // Test $hour returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledHour.get());

    // Test $hour with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    hourExpr = sbe::makeE<sbe::EFunction>(
        "hour", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledHour = compileExpression(*hourExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledHour.get(), 10);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicMinute) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto minuteExpr = sbe::makeE<sbe::EFunction>("minute",
                                                 sbe::makeEs(makeE<EVariable>(dateSlot),
                                                             makeE<EVariable>(timezoneDBSlot),
                                                             makeE<EVariable>(timezoneSlot)));
    auto compiledMinute = compileExpression(*minuteExpr);

    // Test $minute returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledMinute.get(), 11);

    // Test $minute returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMinute.get());

    // Test $minute returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMinute.get());

    // Test $minute returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMinute.get());

    // Test $minute with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    minuteExpr = sbe::makeE<sbe::EFunction>(
        "minute", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledMinute = compileExpression(*minuteExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledMinute.get(), 11);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicSecond) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto secondExpr = sbe::makeE<sbe::EFunction>("second",
                                                 sbe::makeEs(makeE<EVariable>(dateSlot),
                                                             makeE<EVariable>(timezoneDBSlot),
                                                             makeE<EVariable>(timezoneSlot)));
    auto compiledSecond = compileExpression(*secondExpr);

    // Test $second returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledSecond.get(), 12);

    // Test $second returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledSecond.get());

    // Test $second returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledSecond.get());

    // Test $second returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledSecond.get());

    // Test $second with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    secondExpr = sbe::makeE<sbe::EFunction>(
        "second", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledSecond = compileExpression(*secondExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledSecond.get(), 12);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicMillisecond) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto millisecondExpr = sbe::makeE<sbe::EFunction>("millisecond",
                                                      sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                  makeE<EVariable>(timezoneDBSlot),
                                                                  makeE<EVariable>(timezoneSlot)));
    auto compiledMillisecond = compileExpression(*millisecondExpr);

    // Test $millisecond returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledMillisecond.get(), 123);

    // Test $millisecond returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMillisecond.get());

    // Test $millisecond returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMillisecond.get());

    // Test $millisecond returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledMillisecond.get());

    // Test $millisecond with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    millisecondExpr = sbe::makeE<sbe::EFunction>(
        "millisecond", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledMillisecond = compileExpression(*millisecondExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledMillisecond.get(), 123);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicWeek) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto weekExpr = sbe::makeE<sbe::EFunction>("week",
                                               sbe::makeEs(makeE<EVariable>(dateSlot),
                                                           makeE<EVariable>(timezoneDBSlot),
                                                           makeE<EVariable>(timezoneSlot)));
    auto compiledWeek = compileExpression(*weekExpr);

    // Test $week returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 1, 1, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledWeek.get(), 0);

    // Test $week returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledWeek.get());

    // Test $week returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledWeek.get());

    // Test $week returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledWeek.get());

    // Test $week with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    weekExpr = sbe::makeE<sbe::EFunction>(
        "week", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledWeek = compileExpression(*weekExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 1, 1, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledWeek.get(), 0);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicISOWeekYear) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto isoWeekYearExpr = sbe::makeE<sbe::EFunction>("isoWeekYear",
                                                      sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                  makeE<EVariable>(timezoneDBSlot),
                                                                  makeE<EVariable>(timezoneSlot)));
    auto compiledISOWeekYear = compileExpression(*isoWeekYearExpr);

    // Test $isoWeekYear returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertIsoWeekYearExpression(compiledISOWeekYear.get(), static_cast<int64_t>(1996));

    // Test $isoWeekYear returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeekYear.get());

    // Test $isoWeekYear returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeekYear.get());

    // Test $isoWeekYear returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeekYear.get());

    // Test $isoWeekYear with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    isoWeekYearExpr = sbe::makeE<sbe::EFunction>(
        "isoWeekYear", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledISOWeekYear = compileExpression(*isoWeekYearExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertIsoWeekYearExpression(compiledISOWeekYear.get(), static_cast<int64_t>(1996));
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicISODayOfWeek) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto isoDayOfWeekExpr = sbe::makeE<sbe::EFunction>("isoDayOfWeek",
                                                       sbe::makeEs(makeE<EVariable>(dateSlot),
                                                                   makeE<EVariable>(timezoneDBSlot),
                                                                   makeE<EVariable>(timezoneSlot)));
    auto compiledISODayOfWeek = compileExpression(*isoDayOfWeekExpr);

    // Test $isoDayOfWeek returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledISODayOfWeek.get(), 4);

    // Test $isoDayOfWeek returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISODayOfWeek.get());

    // Test $isoDayOfWeek returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISODayOfWeek.get());

    // Test $isoDayOfWeek returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISODayOfWeek.get());

    // Test $isoDayOfWeek with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    isoDayOfWeekExpr = sbe::makeE<sbe::EFunction>(
        "isoDayOfWeek", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledISODayOfWeek = compileExpression(*isoDayOfWeekExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 7, 18, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledISODayOfWeek.get(), 4);
}

TEST_F(SBEDateExpressionAcceptingTimezoneTest, BasicISOWeek) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto isoWeekExpr = sbe::makeE<sbe::EFunction>("isoWeek",
                                                  sbe::makeEs(makeE<EVariable>(dateSlot),
                                                              makeE<EVariable>(timezoneDBSlot),
                                                              makeE<EVariable>(timezoneSlot)));
    auto compiledISOWeek = compileExpression(*isoWeekExpr);

    // Test $isoWeek returns the correct value.
    auto tzdb = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        false, value::TypeTags::timeZoneDB, value::bitcastFrom<TimeZoneDatabase*>(tzdb.get()));
    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 1, 1, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledISOWeek.get(), 1);

    // Test $isoWeek returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeek.get());

    // Test $isoWeek returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeek.get());

    // Test $isoWeek returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledISOWeek.get());

    // Test $isoWeek with constant timezone
    value::OwnedValueAccessor timezoneObjAccessor;
    auto timezoneObjSlot = bindAccessor(&timezoneObjAccessor);
    isoWeekExpr = sbe::makeE<sbe::EFunction>(
        "isoWeek", sbe::makeEs(makeE<EVariable>(dateSlot), makeE<EVariable>(timezoneObjSlot)));
    compiledISOWeek = compileExpression(*isoWeekExpr);

    dateAccessor.reset(value::TypeTags::Date,
                       value::bitcastFrom<int64_t>(
                           kDefaultTimeZone.createFromDateParts(1996, 1, 1, 10, 11, 12, 123)
                               .toMillisSinceEpoch()));
    auto timezone = tzdb->utcZone();
    timezoneObjAccessor.reset(
        false, value::TypeTags::timeZone, value::bitcastFrom<TimeZone*>(&timezone));
    runAndAssertExpression(compiledISOWeek.get(), 1);
}
}  // namespace mongo::sbe
