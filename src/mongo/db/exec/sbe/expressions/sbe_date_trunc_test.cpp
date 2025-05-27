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
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {

using SBEDateTruncTest = EExpressionTestFixture;
using SBEIsTimeUnitTest = EExpressionTestFixture;
using SBEIsDayOfWeekTest = EExpressionTestFixture;
const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();

/**
 * Converts Timestamp type parameter 'timestamp' to SBE type tag and value pair.
 */
std::pair<value::TypeTags, value::Value> convertTimestampToSbeValue(const Timestamp& timestamp) {
    return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(timestamp.asULL())};
}

/**
 * Makes 32-bit integer SBE value and tag pair from 'value'.
 */
std::pair<value::TypeTags, value::Value> makeIntValue(int value) {
    return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value)};
}

/**
 * Makes 64-bit integer SBE value and tag pair from 'value'.
 */
std::pair<value::TypeTags, value::Value> makeLongValue(long long value) {
    return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(value)};
}

/**
 * Makes double SBE value and tag pair from 'value'.
 */
std::pair<value::TypeTags, value::Value> makeDoubleValue(double value) {
    return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(value)};
}

/**
 * Makes decimal SBE value and tag pair from 'value'.
 */
std::pair<value::TypeTags, value::Value> makeDecimalValue(const char* value) {
    return value::makeCopyDecimal(Decimal128(value));
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

/**
 * Makes OID type SBE value and tag pair from date parts 'year', 'month' and so on.
 */
std::pair<value::TypeTags, value::Value> makeDateValueOID(
    long long year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) {
    auto oid = OID::gen();
    oid.init(kDefaultTimeZone.createFromDateParts(year, month, day, hour, minute, second, 0));
    auto [tag, value] = value::makeNewObjectId();
    oid.view().readInto(value::getObjectIdView(value));
    return {tag, value};
}

}  // namespace

/**
 * A test for SBE built-in functions "dateTrunc" and "valueBlockDateTrunc".
 */
TEST_F(SBEDateTruncTest, BasicDateTrunc) {
    value::OwnedValueAccessor timezoneDBAccessor;
    auto timezoneDBSlot = bindAccessor(&timezoneDBAccessor);
    value::OwnedValueAccessor dateAccessor;
    auto dateSlot = bindAccessor(&dateAccessor);
    value::OwnedValueAccessor binSizeAccessor;
    auto binSizeSlot = bindAccessor(&binSizeAccessor);
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

    // Construct an invocation of "dateTrunc" function.
    auto dateTruncExpression =
        sbe::makeE<sbe::EFunction>("dateTrunc",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(dateSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(binSizeSlot),
                                               makeE<EVariable>(timezoneSlot),
                                               makeE<EVariable>(startOfWeekSlot)));
    auto compiledDateTrunc = compileExpression(*dateTruncExpression);

    // Construct an invocation of "valueBlockDateTrunc" function.
    auto valueBlockDateTruncExpression =
        sbe::makeE<sbe::EFunction>("valueBlockDateTrunc",
                                   sbe::makeEs(makeE<EVariable>(bitsetSlot),
                                               makeE<EVariable>(blockSlot),
                                               makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(binSizeSlot),
                                               makeE<EVariable>(timezoneSlot),
                                               makeE<EVariable>(startOfWeekSlot)));
    auto compiledValueBlockDateTrunc = compileExpression(*valueBlockDateTruncExpression);

    // Setup timezone database.
    auto timezoneDatabase = std::make_unique<TimeZoneDatabase>();
    timezoneDBAccessor.reset(false,
                             value::TypeTags::timeZoneDB,
                             value::bitcastFrom<TimeZoneDatabase*>(timezoneDatabase.get()));

    struct TestCase {
        std::pair<value::TypeTags, value::Value> timezone;
        std::pair<value::TypeTags, value::Value> date;
        std::pair<value::TypeTags, value::Value> unit;
        std::pair<value::TypeTags, value::Value> binSize;
        std::pair<value::TypeTags, value::Value> startOfWeek;
        std::pair<value::TypeTags, value::Value> expectedValue;  // Output.
    };

    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    const std::pair<value::TypeTags, value::Value> kDate{makeDateValue(2022, 9, 12, 12, 24, 36)};
    const std::pair<value::TypeTags, value::Value> kDateOID{
        makeDateValueOID(2022, 9, 12, 12, 24, 36)};
    const std::pair<value::TypeTags, value::Value> kHourTruncatedDate{
        makeDateValue(2022, 9, 12, 12, 0, 0)};
    std::vector<TestCase> testCases{
        {
            // Sunny day case.
            value::makeNewString("America/New_York"),
            kDate,
            value::makeNewString("hour"),
            makeLongValue(1),
            value::makeNewString("sun"),
            kHourTruncatedDate,
        },
        {
            // Accepts OID values.
            value::makeNewString("GMT"),
            kDateOID,
            value::makeNewString("hour"),
            makeLongValue(1),
            value::makeNewString("sun"),
            kHourTruncatedDate,
        },
        {
            // Accepts Timestamp values.
            value::makeNewString("UTC"),
            convertTimestampToSbeValue(Timestamp{Hours{3}, 0}),
            value::makeNewString("hour"),
            makeLongValue(2),
            value::makeNewString("sun"),
            makeDateValue(1970, 1, 1, 2, 0, 0),
        },
        {// 'timezone' is Nothing.
         kNothing,
         kDate,
         value::makeNewString("hour"),
         makeLongValue(1),
         value::makeNewString("sun"),
         kNothing},
        {// 'timezone' is not a valid type.
         makeLongValue(0),
         kDate,
         value::makeNewString("hour"),
         makeLongValue(1),
         value::makeNewString("sun"),
         kNothing},
        {// 'timezone' is not a recognized value.
         value::makeNewString("Arctic/North_Pole"),
         kDate,
         value::makeNewString("hour"),
         makeLongValue(1),
         value::makeNewString("sun"),
         kNothing},
        {
            // 'date' is Nothing.
            value::makeNewString("UTC"),
            kNothing,
            value::makeNewString("hour"),
            makeLongValue(1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'date' is not a valid type.
            value::makeNewString("UTC"),
            makeLongValue(0),
            value::makeNewString("hour"),
            makeLongValue(1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'unit' is Nothing.
            value::makeNewString("UTC"),
            kDate,
            kNothing,
            makeLongValue(1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'unit' is not a valid type.
            value::makeNewString("UTC"),
            kDate,
            makeLongValue(0),
            makeLongValue(1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'unit' is not a recognized value.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("century"),
            makeLongValue(1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is Nothing.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            kNothing,
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is Nothing.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            kNothing,
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is not a valid type.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            value::makeNewString("one"),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is not an integer.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeDoubleValue(1.5),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is zero.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeLongValue(0),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' is negative.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeLongValue(-1),
            value::makeNewString("sun"),
            kNothing,
        },
        {
            // 'binSize' integer value is accepted.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeIntValue(1),
            value::makeNewString("sun"),
            kHourTruncatedDate,
        },
        {
            // 'binSize' double value is accepted.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeDoubleValue(1.0),
            value::makeNewString("sun"),
            kHourTruncatedDate,
        },
        {
            // 'binSize' decimal value is accepted.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeDecimalValue("1"),
            value::makeNewString("sun"),
            kHourTruncatedDate,
        },
        {// 'startOfWeek' is present and invalid type.
         value::makeNewString("UTC"),
         kDate,
         value::makeNewString("hour"),
         makeLongValue(1),
         makeLongValue(0),
         kHourTruncatedDate},
        {
            // 'startOfWeek' is present, valid type but invalid value, unit is not week.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("hour"),
            makeLongValue(1),
            value::makeNewString("holiday"),
            kHourTruncatedDate,
        },
        {
            // 'startOfWeek' is Nothing, unit is week.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("week"),
            makeLongValue(1),
            kNothing,
            kNothing,
        },
        {
            // 'startOfWeek' is invalid type, unit is week.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("week"),
            makeLongValue(1),
            makeLongValue(0),
            kNothing,
        },
        {
            // 'startOfWeek' is invalid value, unit is week.
            value::makeNewString("UTC"),
            kDate,
            value::makeNewString("week"),
            makeLongValue(1),
            value::makeNewString("holiday"),
            kNothing,
        },
        {
            // 'startOfWeek' is valid value, unit is week.
            value::makeNewString("UTC"),
            makeDateValue(2022, 9, 12, 12, 24, 36),
            value::makeNewString("week"),
            makeLongValue(1),
            value::makeNewString("Saturday"),
            makeDateValue(2022, 9, 10, 0, 0, 0),
        },
    };

    {
        int testNumber{0};
        for (auto&& testCase : testCases) {
            // Values will be freed after running block tests.
            timezoneAccessor.reset(false, testCase.timezone.first, testCase.timezone.second);
            dateAccessor.reset(false, testCase.date.first, testCase.date.second);
            unitAccessor.reset(false, testCase.unit.first, testCase.unit.second);
            binSizeAccessor.reset(false, testCase.binSize.first, testCase.binSize.second);
            startOfWeekAccessor.reset(
                false, testCase.startOfWeek.first, testCase.startOfWeek.second);

            // Execute the "dateTrunc" function.
            auto result = runCompiledExpression(compiledDateTrunc.get());
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
            timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);
            unitAccessor.reset(testCase.unit.first, testCase.unit.second);
            binSizeAccessor.reset(testCase.binSize.first, testCase.binSize.second);
            startOfWeekAccessor.reset(testCase.startOfWeek.first, testCase.startOfWeek.second);

            value::HeterogeneousBlock block;
            block.push_back(testCase.date.first, testCase.date.second);
            blockAccessor.reset(sbe::value::TypeTags::valueBlock,
                                value::bitcastFrom<value::ValueBlock*>(&block));

            value::HeterogeneousBlock bitset;
            bitset.push_back(makeBool(true));
            bitsetAccessor.reset(sbe::value::TypeTags::valueBlock,
                                 value::bitcastFrom<value::ValueBlock*>(&bitset));

            // Execute the "valueBlockDateTrunc" function.
            auto [resultTag, resultValue] =
                runCompiledExpression(compiledValueBlockDateTrunc.get());
            value::ValueGuard resultGuard(resultTag, resultValue);

            assertBlockEq(resultTag,
                          resultValue,
                          std::vector{std::pair(testCase.expectedValue.first,
                                                testCase.expectedValue.second)});
        }
    }
}

}  // namespace mongo::sbe
