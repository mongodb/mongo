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

#include "mongo/bson/oid.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/query/datetime/date_time_support.h"

namespace mongo::sbe {
namespace {

using SBEDateDiffTest = EExpressionTestFixture;

/**
 * A fixture for SBE built-in function "isTimeUnit".
 */
class SBEIsTimeUnitTest : public EExpressionTestFixture {
public:
    void runAndAssertNothing(const vm::CodeFragment* compiledExpression) {
        auto [resultTag, resultValue] = runCompiledExpression(compiledExpression);
        value::ValueGuard guard(resultTag, resultValue);
        ASSERT_EQUALS(resultTag, sbe::value::TypeTags::Nothing);
        ASSERT_EQUALS(resultValue, 0);
    }
};

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

    auto dateDiffExpression =
        sbe::makeE<sbe::EFunction>("dateDiff",
                                   sbe::makeEs(makeE<EVariable>(timezoneDBSlot),
                                               makeE<EVariable>(startDateSlot),
                                               makeE<EVariable>(endDateSlot),
                                               makeE<EVariable>(unitSlot),
                                               makeE<EVariable>(timezoneSlot)));
    auto compiledDateDiff = compileExpression(*dateDiffExpression);

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
        std::pair<value::TypeTags, value::Value> expectedValue;
    };

    const std::pair<value::TypeTags, value::Value> kNothing{value::TypeTags::Nothing, 0};
    const std::pair<value::TypeTags, value::Value> kAnyDate{
        value::TypeTags::Date, value::bitcastFrom<int64_t>(1604255016000)};  // 2020-11-01 18:23:36
    const OID kOid = OID::gen();
    std::vector<TestCase> testCases{
        {// Sunny day case.
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604255016000)},  // 2020-11-01 18:23:36
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604260800000)},  // 2020-11-01 20:00:00
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2LL)}},
        {// Accepts OID values.
         convertOIDToSbeValue(kOid),
         convertOIDToSbeValue(kOid),
         value::makeNewString("millisecond"),
         value::makeNewString("GMT"),
         {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0LL)}},
        {// Accepts Timestamp values.
         convertTimestampToSbeValue(Timestamp{Seconds{2}, 0}),
         convertTimestampToSbeValue(Timestamp{Seconds{4}, 0}),
         value::makeNewString("second"),
         value::makeNewString("America/New_York"),
         {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2LL)}},
        {// 'startDate' is Nothing.
         kNothing,
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604260800000)},  // 2020-11-01 20:00:00
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'endDate' is Nothing.
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604260800000)},  // 2020-11-01 20:00:00
         kNothing,
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'startDate' is not a valid type.
         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)},
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604260800000)},  // 2020-11-01 20:00:00
         value::makeNewString("hour"),
         value::makeNewString("GMT"),
         kNothing},
        {// 'endDate' is not a valid type.
         {value::TypeTags::Date,
          value::bitcastFrom<int64_t>(1604260800000)},  // 2020-11-01 20:00:00
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
         kNothing}};

    int testNumber{0};
    for (auto&& testCase : testCases) {
        startDateAccessor.reset(testCase.startDate.first, testCase.startDate.second);
        endDateAccessor.reset(testCase.endDate.first, testCase.endDate.second);
        unitAccessor.reset(testCase.unit.first, testCase.unit.second);
        timezoneAccessor.reset(testCase.timezone.first, testCase.timezone.second);

        // Execute the "dateDiff" function.
        auto [resultTag, resultValue] = runCompiledExpression(compiledDateDiff.get());
        value::ValueGuard resultGuard(resultTag, resultValue);

        auto [compResultTag, compResultValue] = compareValue(
            resultTag, resultValue, testCase.expectedValue.first, testCase.expectedValue.second);
        value::ValueGuard compResultGuard(compResultTag, compResultValue);

        ASSERT_EQUALS(compResultTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(compResultValue, 0) << "Failed test #" << testNumber;
        ++testNumber;
    }
}

/**
 * A test for SBE built-in function "isTimeUnit".
 */
TEST_F(SBEIsTimeUnitTest, BasicIsTimeUnit) {
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
}  // namespace mongo::sbe
