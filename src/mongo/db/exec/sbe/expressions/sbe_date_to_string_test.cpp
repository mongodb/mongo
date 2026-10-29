// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {

using SBEDateToStringTest = EExpressionTestFixture;
const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();

/**
 * Makes Date type SBE value and tag pair from date parts 'year', 'month' and so on.
 */
std::pair<value::TypeTags, value::Value> makeDateValue(
    long long year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) {
    return {value::TypeTags::Date,
            value::bitcastFrom<int64_t>(
                kDefaultTimeZone.createFromDateParts(year, month, day, hour, minute, second, 0)
                    .toMillisSinceEpoch())};
}

}  // namespace

/**
 * A test for SBE built-in function "DateToString".
 */
TEST_F(SBEDateToStringTest, BasicDateToString) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor formatAccessor;
    auto formatSlot = bindAccessor(&formatAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    // Construct an invocation of "DateToString" function.
    auto DateToStringExpression =
        sbe::makeE<sbe::EFunction>(EFn::kDateToString,
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateSlot),
                                               makeE<EVariable>(formatSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledDateToString = compileExpression(*DateToStringExpression);


    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(
        value::TagValueView{value::TypeTags::timeZoneDB,
                            value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get())});

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> date;
        std::pair<value::TypeTags, value::Value> format;
        std::string_view expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    const std::pair<value::TypeTags, value::Value> kNull{value::TypeTags::Null, 0};
    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2023, 8, 14, 12, 24, 36)};
    std::vector<TestCase> validTestCases{
        {
            // Ideal case.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view("08/14/2023, 12:24:36"),
        },
        {
            // America/New York is 4 hours behind UTC.
            value::makeNewString("America/New_York"),
            kDate,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view("08/14/2023, 08:24:36"),
        },
        {
            // Try a weirder format string.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString(
                "The %dth day of the %mth month of the year %Y, in the %Sth "
                "second of the %Mth minute of the %Hth hour with timezone offset %z is the %wnd "
                "day of the %Urd week of the year, and the %jth day of the year."),
            std::string_view(
                "The 14th day of the 08th month of the year 2023, in the 36th "
                "second of the 24th minute of the 12th hour with timezone offset +0000 is the 2nd "
                "day of the 33rd week of the year, and the 226th day of the year."),
        },
    };
    std::vector<TestCase> invalidTestCases{
        {
            //'timezone' is Nothing.
            kNothing,
            kDate,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'timezone' is not a valid type.
            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0)},
            kDate,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'timezone' is not a recognized value.
            value::makeNewString("Arctic/North_Pole"),
            kDate,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'date' is Nothing.
            value::makeNewString("UTC"),
            kNothing,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'date' is Null.
            value::makeNewString("UTC"),
            kNull,
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'date' is not a valid type.
            value::makeNewString("UTC"),
            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0)},
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            std::string_view(),
        },
        {
            //'format' is Nothing.
            value::makeNewString("UTC"),
            kDate,
            kNothing,
            std::string_view(),
        },
        {
            //'format' is a valid string, but not a valid format.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("Random String%"),
            std::string_view(),
        },
        {
            //'format' is not a valid type.
            value::makeNewString("UTC"),
            kDate,
            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0)},
            std::string_view(),
        },
    };

    int testNumber{0};
    for (auto&& testCase : validTestCases) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateAccessor.reset(testCase.date.first, testCase.date.second);
        formatAccessor.reset(testCase.format.first, testCase.format.second);

        // Execute the "DateToString" function.
        value::TagValueOwned resultString =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledDateToString.get()));

        ASSERT(value::isString(resultString.tag()))
            << "Failed test #" << testNumber << ", result tag: " << resultString.tag()
            << ", expected a string";
        auto resultStringView = value::getStringView(resultString.tag(), resultString.value());
        ASSERT_EQUALS(resultStringView, testCase.expectedValue)
            << "Failed test #" << testNumber << ", result: " << resultStringView
            << ", expected: " << testCase.expectedValue;
        ++testNumber;
    }
    for (auto&& testCase : invalidTestCases) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateAccessor.reset(testCase.date.first, testCase.date.second);
        formatAccessor.reset(testCase.format.first, testCase.format.second);

        // Execute the "DateToString" function.
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledDateToString.get()));

        ASSERT_EQUALS(result.tag(), value::TypeTags::Nothing)
            << "Failed test #" << testNumber << ", result: " << result.tag()
            << ", expected Nothing";
        ++testNumber;
    }
}

}  // namespace mongo::sbe
