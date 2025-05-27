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

#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_block_test_helpers.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {
namespace {

using SBEDateDiffTest = EExpressionTestFixture;
using SBEIsTimeUnitTest = EExpressionTestFixture;
using SBEIsDayOfWeekTest = EExpressionTestFixture;
const TimeZoneDatabase kDefaultTimeZoneDatabase{};
const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();

/**
 * Resets value accessor 'accessor' with string 'value'.
 */
void setValue(std::string value, value::OwnedValueAccessor& accessor) {
    auto [tag, val] = value::makeNewString(value);
    accessor.reset(tag, val);
}

/**
 * Converts OID type parameter 'oid' to SBE type tag and value pair.
 */
std::pair<value::TypeTags, value::Value> convertOIDToSbeValue(const OID& oid) {
    auto [tag, value] = value::makeNewObjectId();
    oid.view().readInto(value::getObjectIdView(value));
    return {tag, value};
}

/**
 * Converts Timestamp type parameter 'timestamp' to SBE type tag and value pair.
 */
std::pair<value::TypeTags, value::Value> convertTimestampToSbeValue(const Timestamp& timestamp) {
    return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(timestamp.asULL())};
}

/**
 * Makes 64-bit integer SBE value and tag pair from 'value'.
 */
std::pair<value::TypeTags, value::Value> makeLongValue(long long value) {
    return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(value)};
}

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
 * A test for SBE built-in function "dateDiff".
 */
TEST_F(SBEDateDiffTest, BasicDateDiff) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor startDateAccessor;
    auto startDateSlot = bindAccessor(&startDateAccessor);
    value::OwnedValueAccessor endDateAccessor;
    auto endDateSlot = bindAccessor(&endDateAccessor);
    value::OwnedValueAccessor unitAccessor;
    auto unitSlot = bindAccessor(&unitAccessor);
    value::OwnedValueAccessor timezoneAccessor;
    auto timezoneSlot = bindAccessor(&timezoneAccessor);
    value::OwnedValueAccessor startOfWeekAccessor;
    auto startOfWeekSlot = bindAccessor(&startOfWeekAccessor);

    value::ViewOfValueAccessor blockAccessor;
    auto blockSlot = bindAccessor(&blockAccessor);
    value::ViewOfValueAccessor bitsetAccessor;
    auto bitsetSlot = bindAccessor(&bitsetAccessor);

    // Construct an invocation of "dateDiff" function without 'startOfWeek' parameter.
    auto dateDiffExpression =
        sbe::makeE<sbe::EFunction>("dateDiff",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(startDateSlot),
                                               makeE<EVariable>(endDateSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledDateDiff = compileExpression(*dateDiffExpression);

    // Construct an invocation of "dateDiff" function with 'startOfWeek' parameter.
    dateDiffExpression = sbe::makeE<sbe::EFunction>("dateDiff",
                                                    sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                                                makeE<EVariable>(startDateSlot),
                                                                makeE<EVariable>(endDateSlot),
                                                                makeE<EVariable>(unitSlot),
                                                                makeE<EVariable>(timezoneSlot),
                                                                makeE<EVariable>(startOfWeekSlot)));
    auto compiledDateDiffWithStartOfWeek = compileExpression(*dateDiffExpression);

    // Construct an invocation of "valueBlockDateDiff" function.
    auto valueBlockDateDiffExpression =
        sbe::makeE<sbe::EFunction>("valueBlockDateDiff",
                                   sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                               makeE<EVariable>(blockSlot),
                                               makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(endDateSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledValueBlockDateDiff = compileExpression(*valueBlockDateDiffExpression);

    // Construct an invocation of "valueBlockDateDiff" function with 'startOfWeek' parameter.
    valueBlockDateDiffExpression =
        sbe::makeE<sbe::EFunction>("valueBlockDateDiff",
                                   sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                               makeE<EVariable>(blockSlot),
                                               makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(endDateSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(timezoneSlot),
                                               makeE<EVariable>(startOfWeekSlot)));
    auto compiledValueBlockDateDiffWithStartOfWeek =
        compileExpression(*valueBlockDateDiffExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> startDate;
        std::pair<value::TypeTags, value::Value> endDate;
        std::pair<value::TypeTags, value::Value> unit;
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
        boost::optional<std::pair<value::TypeTags, value::Value>> startOfWeek;
    };

    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    const std::pair<value::TypeTags, value::Value> kAnyDate{makeDateValue(2020, 11, 1, 18, 23, 36)};
    const OID kOid = OID::gen();
    std::vector<TestCase> testCases{
        {// Sunny day case.
         makeDateValue(2020, 11, 01, 18, 23, 36),
         makeDateValue(2020, 11, 01, 20, 0, 0),
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         makeLongValue(2)},
        {// Accepts OID values.
         convertOIDToSbeValue(kOid),
         convertOIDToSbeValue(kOid),
         value::makeNewString("millisecond"),
         value::makeNewString("GMT"),
         makeLongValue(0)},
        {// Accepts Timestamp values.
         convertTimestampToSbeValue(Timestamp{Seconds{2}, 0}),
         convertTimestampToSbeValue(Timestamp{Seconds{4}, 0}),
         value::makeNewString("second"),
         value::makeNewString("America/New_York"),
         makeLongValue(2)},
        {// 'startDate' is Nothing.
         kNothing,
         makeDateValue(2020, 11, 01, 20, 0, 0),
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'endDate' is Nothing.
         makeDateValue(2020, 11, 01, 20, 0, 0),
         kNothing,
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'startDate' is not a valid type.
         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
         makeDateValue(2020, 11, 01, 20, 0, 0),
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'endDate' is not a valid type.
         makeDateValue(2020, 11, 01, 20, 0, 0),
         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'unit' is not a valid type.
         kAnyDate,
         kAnyDate,
         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
         value::makeNewString("GMT"),
         kNothing},
        {// 'unit' is Nothing.
         kAnyDate,
         kAnyDate,
         kNothing,
         value::makeNewString("GMT"),
         kNothing},
        {// 'unit' is not a recognized value.
         kAnyDate,
         kAnyDate,
         value::makeNewString("century"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'timezone' is not a valid type.
         kAnyDate,
         kAnyDate,
         value::makeNewString("hour"),
         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
         kNothing},
        {// 'timezone' is not a recognized value.
         kAnyDate,
         kAnyDate,
         value::makeNewString("hour"),
         value::makeNewString("Arctic/North_Pole"),
         kNothing},
        {// 'timezone' is Nothing.
         kAnyDate,
         kAnyDate,
         value::makeNewString("hour"),
         kNothing,
         kNothing},
        {
            // 'startOfWeek' is present and invalid type.
            kAnyDate,
            kAnyDate,
            value::makeNewString("hour"),
            value::makeNewString("GMT"),
            kNothing,         // result
            makeLongValue(1)  // startOfWeek
        },
        {
            // 'startOfWeek' is present, valid type but invalid value, unit is not week.
            kAnyDate,
            kAnyDate,
            value::makeNewString("hour"),
            value::makeNewString("GMT"),
            makeLongValue(0),                // result
            value::makeNewString("INVALID")  // startOfWeek
        },
        {
            // 'startOfWeek' is Nothing, unit is week.
            kAnyDate,
            kAnyDate,
            value::makeNewString("week"),
            value::makeNewString("GMT"),
            kNothing,  // result
            kNothing   // startOfWeek
        },
        {
            // 'startOfWeek' is invalid type, unit is week.
            kAnyDate,
            kAnyDate,
            value::makeNewString("week"),
            value::makeNewString("GMT"),
            kNothing,         // result
            makeLongValue(0)  // startOfWeek
        },
        {
            // 'startOfWeek' is invalid value, unit is week.
            kAnyDate,
            kAnyDate,
            value::makeNewString("week"),
            value::makeNewString("GMT"),
            kNothing,                        // result
            value::makeNewString("holiday")  // startOfWeek
        },
        {
            // 'startOfWeek' is valid value, unit is week.
            makeDateValue(2021, 01, 25, 8, 0, 0),  // Monday
            makeDateValue(2021, 01, 26, 8, 0, 0),  // Tuesday
            value::makeNewString("week"),
            value::makeNewString("GMT"),
            makeLongValue(1),                // result
            value::makeNewString("Tuesday")  // startOfWeek
        },
        {
            // 'startOfWeek' is not specified (should default to Sunday), unit is week.
            makeDateValue(2021, 01, 23, 8, 0, 0),  // Saturday
            makeDateValue(2021, 01, 24, 8, 0, 0),  // Sunday
            value::makeNewString("week"),
            value::makeNewString("GMT"),
            makeLongValue(1)  // result
        }};

    {
        int testNumber{0};
        for (auto&& testCase : testCases) {
            // Values will be freed after running block tests.
            startDateAccessor.reset(false, testCase.startDate.first, testCase.startDate.second);
            endDateAccessor.reset(false, testCase.endDate.first, testCase.endDate.second);
            unitAccessor.reset(false, testCase.unit.first, testCase.unit.second);
            timezoneAccessor.reset(false, testCase.timezone.first, testCase.timezone.second);
            if (testCase.startOfWeek) {
                startOfWeekAccessor.reset(
                    false, testCase.startOfWeek->first, testCase.startOfWeek->second);
            }

            // Execute the "dateDiff" function.
            auto result = runCompiledExpression(
                (testCase.startOfWeek ? compiledDateDiffWithStartOfWeek : compiledDateDiff).get());
            auto [resultTag, resultValue] = result;
            value::ValueGuard resultGuard(resultTag, resultValue);

            auto [compResultTag, compResultValue] = compareValue(resultTag,
                                                                 resultValue,
                                                                 testCase.expectedValue.first,
                                                                 testCase.expectedValue.second);
            value::ValueGuard compResultGuard(compResultTag, compResultValue);

            ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32);
            ASSERT_EQUALS(compResultValue, 0)
                << "Failed test #" << testNumber << ", result: " << result
                << ", expected: " << testCase.expectedValue;
            ++testNumber;
        }
    }

    {
        for (auto&& testCase : testCases) {
            endDateAccessor.reset(testCase.endDate.first, testCase.endDate.second);
            unitAccessor.reset(testCase.unit.first, testCase.unit.second);
            timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
            if (testCase.startOfWeek) {
                startOfWeekAccessor.reset(testCase.startOfWeek->first,
                                          testCase.startOfWeek->second);
            }
            value::HeterogeneousBlock block;
            block.push_back(testCase.startDate.first, testCase.startDate.second);
            blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(&block));

            value::HeterogeneousBlock bitset;
            bitset.push_back(makeBool(true));
            bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                                 value::bitcastFrom<value::ValueBlock*>(&bitset));

            // Execute the "valueBlockDateDiff" function.
            auto result = runCompiledExpression((testCase.startOfWeek
                                                     ? compiledValueBlockDateDiffWithStartOfWeek
                                                     : compiledValueBlockDateDiff)
                                                    .get());
            auto [resultTag, resultValue] = result;
            value::ValueGuard resultGuard(resultTag, resultValue);

            assertBlockEq(resultTag,
                          resultValue,
                          std::vector{std::pair(testCase.expectedValue.first,
                                                testCase.expectedValue.second)});
        }
    }
}

/**
 * A test for SBE built-in function "isTimeUnit".
 */
TEST_F(SBEIsTimeUnitTest, Basic) {
    value::OwnedValueAccessor unitAccessor;
    auto unitSlot = bindAccessor(&unitAccessor);

    auto isTimeUnitExpression =
        sbe::makeE<sbe::EFunction>("isTimeUnit", sbe::makeEs(makeE<EVariable>(unitSlot)));
    auto compiledIsTimeUnitExpression = compileExpression(*isTimeUnitExpression);

    // Verify that when passed Nothing returns Nothing.
    unitAccessor.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledIsTimeUnitExpression.get());

    // Verify that when passed not a string returns Nothing.
    unitAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5));
    runAndAssertNothing(compiledIsTimeUnitExpression.get());

    // Verify that when passed an invalid unit returns false.
    setValue("century", unitAccessor);
    ASSERT_FALSE(runCompiledExpressionPredicate(compiledIsTimeUnitExpression.get()));

    // Verify that when passed a valid unit returns true.
    setValue("second", unitAccessor);
    ASSERT_TRUE(runCompiledExpressionPredicate(compiledIsTimeUnitExpression.get()));
}

/**
 * A test for SBE built-in function "isDayOfWeek".
 */
TEST_F(SBEIsDayOfWeekTest, Basic) {
    value::OwnedValueAccessor dayOfWeekAccessor;
    auto dayOfWeekSlot = bindAccessor(&dayOfWeekAccessor);

    auto isDayOfWeekExpression =
        sbe::makeE<sbe::EFunction>("isDayOfWeek", sbe::makeEs(makeE<EVariable>(dayOfWeekSlot)));
    auto compiledIsDayOfWeekExpression = compileExpression(*isDayOfWeekExpression);

    // Verify that when passed Nothing returns Nothing.
    dayOfWeekAccessor.reset(value::TypeTags::Nothing, 0);
    runAndAssertNothing(compiledIsDayOfWeekExpression.get());

    // Verify that when passed not a string returns Nothing.
    dayOfWeekAccessor.reset(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5));
    runAndAssertNothing(compiledIsDayOfWeekExpression.get());

    // Verify that when passed an invalid unit returns false.
    setValue("m1", dayOfWeekAccessor);
    ASSERT_FALSE(runCompiledExpressionPredicate(compiledIsDayOfWeekExpression.get()));

    // Verify that when passed a valid unit returns true.
    setValue("Wednesday", dayOfWeekAccessor);
    ASSERT_TRUE(runCompiledExpressionPredicate(compiledIsDayOfWeekExpression.get()));
}
}  // namespace mongo::sbe
