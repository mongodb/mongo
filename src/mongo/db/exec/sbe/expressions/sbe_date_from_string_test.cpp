/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {

using SBEDateFromStringTest = EExpressionTestFixture;
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
 * A test for SBE built-in function "DateFromString".
 */
TEST_F(SBEDateFromStringTest, BasicDateFromString) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateStringAccessor;
    auto dateStringSlot = bindAccessor(&dateStringAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);
    value::OwnedValueAccessor formatAccessor;
    auto formatSlot = bindAccessor(&formatAccessor);

    // Construct an invocation of "DateFromString" function.
    auto DateFromStringExpression =
        sbe::makeE<sbe::EFunction>("dateFromString",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateStringSlot),
                                               makeE<EVariable>(timezoneSlot),
                                               makeE<EVariable>(formatSlot)));
    auto compiledDateFromString = compileExpression(*DateFromStringExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> dateString;
        std::pair<value::TypeTags, value::Value> format;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2023, 8, 14, 12, 24, 36)};

    std::vector<TestCase> testCases{
        {
            // Ideal case.
            value::makeNewString("UTC"),
            value::makeNewString("08/14/2023, 12:24:36"),
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            kDate,
        },
        {
            // America/New York is 4 hours behind UTC.
            value::makeNewString("America/New_York"),
            value::makeNewString("08/14/2023, 08:24:36"),
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            kDate,
        },
    };

    int testNumber{0};
    for (auto&& testCase : testCases) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateStringAccessor.reset(testCase.dateString.first, testCase.dateString.second);
        formatAccessor.reset(testCase.format.first, testCase.format.second);

        // Execute the "DateFromString" function.
        auto result = runCompiledExpression(compiledDateFromString.get());
        auto [resultTag, resultValue] = result;
        value::ValueGuard resultGuard(resultTag, resultValue);

        auto [compResultTag, compResultValue] = compareValue(
            resultTag, resultValue, testCase.expectedValue.first, testCase.expectedValue.second);
        value::ValueGuard compResultGuard(compResultTag, compResultValue);

        ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32)
            << "Failed test #" << testNumber << " when running dateFromString, result: " << result
            << ", expected: " << testCase.expectedValue;
        ASSERT_EQUALS(compResultValue, 0)
            << "Failed test #" << testNumber << " when running dateFromString, result: " << result
            << ", expected: " << testCase.expectedValue;
        ++testNumber;
    }
}
/**
 * A test for SBE built-in function "DateFromString" without format specified.
 */
TEST_F(SBEDateFromStringTest, DateFromStringNoFormat) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateStringAccessor;
    auto dateStringSlot = bindAccessor(&dateStringAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    // Construct an invocation of "DateFromString" that doesn't pass in a format.
    auto DateFromStringExpression =
        sbe::makeE<sbe::EFunction>("dateFromString",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateStringSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledDateFromStringWithoutFormat = compileExpression(*DateFromStringExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> dateString;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2023, 8, 14, 12, 24, 36)};

    std::vector<TestCase> testCasesNoFormat{
        {
            // Ideal case.
            value::makeNewString("UTC"),
            value::makeNewString("08/14/2023, 12:24:36"),
            kDate,
        },
        {
            // America/New York is 4 hours behind UTC.
            value::makeNewString("America/New_York"),
            value::makeNewString("08/14/2023, 08:24:36"),
            kDate,
        },
    };

    int testNumber{0};
    for (auto&& testCase : testCasesNoFormat) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateStringAccessor.reset(testCase.dateString.first, testCase.dateString.second);

        // Execute the "DateFromString" function.
        auto result = runCompiledExpression(compiledDateFromStringWithoutFormat.get());
        auto [resultTag, resultValue] = result;
        value::ValueGuard resultGuard(resultTag, resultValue);

        auto [compResultTag, compResultValue] = compareValue(
            resultTag, resultValue, testCase.expectedValue.first, testCase.expectedValue.second);
        value::ValueGuard compResultGuard(compResultTag, compResultValue);

        ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32)
            << "Failed test #" << testNumber
            << " when running dateFromString without format specified, result: " << result
            << ", expected: " << testCase.expectedValue;
        ASSERT_EQUALS(compResultValue, 0)
            << "Failed test #" << testNumber
            << " when running dateFromString without format specified, result: " << result
            << ", expected: " << testCase.expectedValue;
        ++testNumber;
    }
}
/**
 * A test for SBE built-in function "DateFromStringNoThrow".
 */
TEST_F(SBEDateFromStringTest, DateFromStringNoThrow) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateStringAccessor;
    auto dateStringSlot = bindAccessor(&dateStringAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);
    value::OwnedValueAccessor formatAccessor;
    auto formatSlot = bindAccessor(&formatAccessor);

    // Construct an invocation of "DateFromStringNoThrow" function.
    auto DateFromStringNoThrowExpression =
        sbe::makeE<sbe::EFunction>("dateFromStringNoThrow",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateStringSlot),
                                               makeE<EVariable>(timezoneSlot),
                                               makeE<EVariable>(formatSlot)));

    auto compiledDateFromStringNoThrow = compileExpression(*DateFromStringNoThrowExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> dateString;
        std::pair<value::TypeTags, value::Value> format;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2023, 8, 14, 12, 24, 36)};
    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    std::vector<TestCase> testCases{
        {
            // Ideal case.
            value::makeNewString("UTC"),
            value::makeNewString("08/14/2023, 12:24:36"),
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            kDate,
        },
        {
            // America/New York is 4 hours behind UTC.
            value::makeNewString("America/New_York"),
            value::makeNewString("08/14/2023, 08:24:36"),
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            kDate,
        },
        {
            // String that does not parse to a valid date.
            value::makeNewString("UTC"),
            value::makeNewString("13/13/2022 02:00:00"),
            value::makeNewString("%m/%d/%Y, %H:%M:%S"),
            kNothing,
        },
    };

    int testNumber{0};
    for (auto&& testCase : testCases) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateStringAccessor.reset(testCase.dateString.first, testCase.dateString.second);
        formatAccessor.reset(testCase.format.first, testCase.format.second);

        // Execute the "DateFromStringNoThrow" function.
        auto result = runCompiledExpression(compiledDateFromStringNoThrow.get());
        auto [resultTag, resultValue] = result;
        value::ValueGuard resultGuard(resultTag, resultValue);

        auto [compResultTag, compResultValue] = compareValue(
            resultTag, resultValue, testCase.expectedValue.first, testCase.expectedValue.second);
        value::ValueGuard compResultGuard(compResultTag, compResultValue);

        ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32)
            << "Failed test #" << testNumber
            << " when running dateFromStringNoThrow, result: " << result
            << ", expected: " << testCase.expectedValue;
        ASSERT_EQUALS(compResultValue, 0)
            << "Failed test #" << testNumber
            << " when running dateFromStringNoThrow, result: " << result
            << ", expected: " << testCase.expectedValue;
        ++testNumber;
    }
}
/**
 * A test for SBE built-in function "DateFromStringNoThrow" without format specified.
 */
TEST_F(SBEDateFromStringTest, DateFromStringNoThrowNoFormat) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateStringAccessor;
    auto dateStringSlot = bindAccessor(&dateStringAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);

    // Construct an invocation of "DateFromStringNoThrow" that doesn't pass in a format.
    auto DateFromStringNoThrowExpression =
        sbe::makeE<sbe::EFunction>("dateFromStringNoThrow",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateStringSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledDateFromStringNoThrowWithoutFormat =
        compileExpression(*DateFromStringNoThrowExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> dateString;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2023, 8, 14, 12, 24, 36)};
    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    std::vector<TestCase> testCases{
        {
            // Ideal case.
            value::makeNewString("UTC"),
            value::makeNewString("08/14/2023, 12:24:36"),
            kDate,
        },
        {
            // America/New York is 4 hours behind UTC.
            value::makeNewString("America/New_York"),
            value::makeNewString("08/14/2023, 08:24:36"),
            kDate,
        },
        {
            // String that does not parse to a valid date.
            value::makeNewString("UTC"),
            value::makeNewString("13/13/2022 02:00:00"),
            kNothing,
        },
    };
    int testNumber{0};
    for (auto&& testCase : testCases) {
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
        dateStringAccessor.reset(testCase.dateString.first, testCase.dateString.second);

        // Execute the "DateFromStringNoThrow" function.
        auto result = runCompiledExpression(compiledDateFromStringNoThrowWithoutFormat.get());
        auto [resultTag, resultValue] = result;
        value::ValueGuard resultGuard(resultTag, resultValue);

        auto [compResultTag, compResultValue] = compareValue(
            resultTag, resultValue, testCase.expectedValue.first, testCase.expectedValue.second);
        value::ValueGuard compResultGuard(compResultTag, compResultValue);

        ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32)
            << "Failed test #" << testNumber
            << " when running dateFromStringNoThrow without format specified, result: " << result
            << ", expected: " << testCase.expectedValue;
        ASSERT_EQUALS(compResultValue, 0)
            << "Failed test #" << testNumber
            << " when running dateFromStringNoThrow without format specified, result: " << result
            << ", expected: " << testCase.expectedValue;
        ++testNumber;
    }
}

}  // namespace mongo::sbe
