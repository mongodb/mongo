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

#include <cstdint>
#include <memory>

namespace mongo::sbe {

class SBEDateToPartsTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        ASSERT_EQUALS(runTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(runVal, 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const int32_t& year,
                                const int32_t& month,
                                const int32_t& day,
                                const int32_t& hour,
                                const int32_t& minute,
                                const int32_t& second,
                                const int32_t& millisecond) {
        auto [runTag, runVal] = runCompiledExpression(compiledExpr);
        value::ValueGuard guard(runTag, runVal);
        auto obj = value::getObjectView(runVal);

        auto [yearTag, yearVal] = obj->getField("year");
        ASSERT_EQUALS(yearTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(year, value::bitcastTo<int32_t>(yearVal));

        auto [monthTag, monthVal] = obj->getField("month");
        ASSERT_EQUALS(monthTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(month, value::bitcastTo<int32_t>(monthVal));

        auto [dayTag, dayVal] = obj->getField("day");
        ASSERT_EQUALS(dayTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(day, value::bitcastTo<int32_t>(dayVal));

        auto [hourTag, hourVal] = obj->getField("hour");
        ASSERT_EQUALS(hourTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(hour, value::bitcastTo<int32_t>(hourVal));

        auto [minuteTag, minuteVal] = obj->getField("minute");
        ASSERT_EQUALS(minuteTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(minute, value::bitcastTo<int32_t>(minuteVal));

        auto [secondTag, secondVal] = obj->getField("second");
        ASSERT_EQUALS(secondTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(second, value::bitcastTo<int32_t>(secondVal));

        auto [millisecondTag, millisecondVal] = obj->getField("millisecond");
        ASSERT_EQUALS(millisecondTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(millisecond, value::bitcastTo<int32_t>(millisecondVal));
    }
};

TEST_F(SBEDateToPartsTest, BasicDateToParts) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto dateToPartsExpr = sbe::makeE<sbe::EFunction>("dateToParts",
                                                      sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                                  makeE<EVariable>(dateSlot),
                                                                  makeE<EVariable>(timezoneSlot)));
    auto compiledDateToParts = compileExpression(*dateToPartsExpr);

    // Test $dateToParts returns the correct date parts.
    TimeZoneDatabase* tzdb = new TimeZoneDatabase();
    timezoneDBAccessor.reset(value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(tzdb));
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledDateToParts.get(), 1970, 1, 1, 6, 5, 29, 999);

    // Test $dateToParts returns Nothing with invalid date.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDateToParts.get());

    // Test $dateToParts returns Nothing with invalid timezone.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDateToParts.get());

    // Test $dateToParts returns Nothing with invalid timezoneDB.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledDateToParts.get());
    delete tzdb;
}

}  // namespace mongo::sbe
