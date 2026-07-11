// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>

namespace mongo::sbe {

class SBEIsoDateToPartsTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpr) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        ASSERT_EQUALS(result.tag(), sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(result.value(), 0);
    }

    void runAndAssertExpression(const vm::CodeFragment* compiledExpr,
                                const int32_t& isoWeekYear,
                                const int32_t& isoWeek,
                                const int32_t& isoDayOfWeek,
                                const int32_t& hour,
                                const int32_t& minute,
                                const int32_t& second,
                                const int32_t& millisecond) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr));
        auto obj = value::getObjectView(result.value());

        auto [isoWeekYearTag, isoWeekYearVal] = obj->getField("isoWeekYear");
        ASSERT_EQUALS(isoWeekYearTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(isoWeekYear, value::bitcastTo<int32_t>(isoWeekYearVal));

        auto [isoWeekTag, isoWeekVal] = obj->getField("isoWeek");
        ASSERT_EQUALS(isoWeekTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(isoWeek, value::bitcastTo<int32_t>(isoWeekVal));

        auto [isoDayOfWeekTag, isoDayOfWeekVal] = obj->getField("isoDayOfWeek");
        ASSERT_EQUALS(isoDayOfWeekTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(isoDayOfWeek, value::bitcastTo<int32_t>(isoDayOfWeekVal));

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

TEST_F(SBEIsoDateToPartsTest, BasicIsoDateToParts) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    auto isoDateToPartsExpr =
        sbe::makeE<sbe::EFunction>(EFn::kIsoDateToParts,
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledIsoDateToParts = compileExpression(*isoDateToPartsExpr);

    // Test $DateToParts with iso8601 returns the correct date parts.
    TimeZoneDatabase* tzdb = new TimeZoneDatabase();
    timezoneDBAccessor.reset(value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(tzdb));
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    auto [timezoneTag, timezoneVal] = value::makeNewString("UTC");
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    runAndAssertExpression(compiledIsoDateToParts.get(), 1970, 1, 4, 6, 5, 29, 999);

    // Test $DateToParts with iso8601 flag and invalid date returns Nothing.
    dateAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledIsoDateToParts.get());

    // Test $DateToParts with iso8601 flag and invalid timezone returns Nothing.
    dateAccessor.reset(value::TypeTags::Date, value::bitcastFrom<int64_t>(21929999));
    timezoneAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledIsoDateToParts.get());

    // Test $DateToParts with iso8601 flag and invalid timezoneDB returns Nothing.
    timezoneAccessor.reset(timezoneTag, timezoneVal);
    timezoneDBAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(42));
    runAndAssertNothing(compiledIsoDateToParts.get());
    delete tzdb;
}

}  // namespace mongo::sbe
