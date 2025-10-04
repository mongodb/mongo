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
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace mongo::sbe {
namespace {
using namespace value;

static std::pair<TypeTags, Value> makeDecimal(const std::string& n) {
    return makeCopyDecimal(Decimal128(n));
}

const std::pair<TypeTags, Value> kNull{TypeTags::Null, 0};
const std::pair<TypeTags, Value> kNothing{TypeTags::Nothing, 0};

/**
 * A test for SBE built-in function round with one argument. The "place" argument defaults to 0.
 */
TEST_F(EExpressionTestFixture, RoundOneArg) {
    OwnedValueAccessor numAccessor;
    auto numSlot = bindAccessor(&numAccessor);

    // Construct an invocation of round function.
    auto roundExpression =
        sbe::makeE<sbe::EFunction>("round", sbe::makeEs(makeE<EVariable>(numSlot)));
    auto compiledRound = compileExpression(*roundExpression);

    struct TestCase {
        std::pair<TypeTags, Value> num;
        std::pair<TypeTags, Value> result;
    };

    std::vector<TestCase> testCases = {
        {makeInt32(0), makeInt32(0)},
        {makeInt32(2), makeInt32(2)},
        {makeInt64(4), makeInt64(4)},
        {makeDouble(-2.0), makeDouble(-2.0)},
        {makeDouble(0.9), makeDouble(1.0)},
        {makeDouble(-1.2), makeDouble(-1.0)},
        {makeDecimal("-1.6"), makeDecimal("-2")},
        {makeDouble(1.298), makeDouble(1.0)},
        {makeDecimal("1.298"), makeDecimal("1")},
        // Infinity cases.
        {makeDouble(std::numeric_limits<double>::infinity()),
         makeDouble(std::numeric_limits<double>::infinity())},
        // Decimal128 infinity.
        {makeDecimal("Infinity"), makeDecimal("Infinity")},
        {makeDecimal("-Infinity"), makeDecimal("-Infinity")},
        // NaN cases.
        {makeDouble(std::numeric_limits<double>::quiet_NaN()),
         makeDouble(std::numeric_limits<double>::quiet_NaN())},
        {makeDecimal("NaN"), makeDecimal("NaN")},
        // Null case.
        {kNull, kNothing},
        // Nothing case.
        {kNothing, kNothing},
    };

    int testNum = 0;
    for (auto&& testCase : testCases) {
        numAccessor.reset(testCase.num.first, testCase.num.second);
        ValueGuard expectedResultGuard(testCase.result.first, testCase.result.second);

        // Execute the round function.
        auto [resultTag, resultValue] = runCompiledExpression(compiledRound.get());
        ValueGuard actualResultGuard(resultTag, resultValue);

        auto [compTag, compVal] =
            compareValue(resultTag, resultValue, testCase.result.first, testCase.result.second);
        ASSERT_EQUALS(compTag, TypeTags::NumberInt32) << "unexpected tag for test " << testNum;
        ASSERT_EQUALS(compVal, bitcastFrom<int32_t>(0)) << "unexpected value for test " << testNum;

        testNum++;
    }
}

/**
 * A test for SBE built-in function round with two arguments.
 */
TEST_F(EExpressionTestFixture, RoundTwoArgs) {
    OwnedValueAccessor numAccessor;
    auto numSlot = bindAccessor(&numAccessor);
    OwnedValueAccessor placeAccessor;
    auto placeSlot = bindAccessor(&placeAccessor);

    // Construct an invocation of round function.
    auto roundExpression = sbe::makeE<sbe::EFunction>(
        "round", sbe::makeEs(makeE<EVariable>(numSlot), makeE<EVariable>(placeSlot)));
    auto compiledRound = compileExpression(*roundExpression);

    struct TestCase {
        std::pair<TypeTags, Value> num;
        std::pair<TypeTags, Value> place;
        std::pair<TypeTags, Value> result;
    };

    std::vector<TestCase> testCases = {
        {makeInt32(43), makeInt32(-1), makeInt32(40)},
        // Try rounding with different types for the "place" argument.
        {makeDouble(1.298), makeInt32(0), makeDouble(1.0)},
        {makeDouble(1.298), makeInt64(0ull), makeDouble(1.0)},
        {makeDouble(1.298), makeDouble(0.0), makeDouble(1.0)},
        // Try rounding with a different value for the "place" argument.
        {makeDouble(1.298), makeDouble(1.0), makeDouble(1.3)},
        {makeDouble(23.298), makeDouble(-1.0), makeDouble(20.0)},
        // Decimal tests.
        {makeDecimal("1.298"), makeDouble(0.0), makeDecimal("1")},
        {makeDecimal("1.298"), makeDouble(1.0), makeDecimal("1.3")},
        {makeDecimal("23.298"), makeDouble(-1.0), makeDecimal("20.0")},
        {makeDecimal("1.298912343250054252245154325"),
         makeDouble(20.0),
         makeDecimal("1.29891234325005425225")},
        {makeDecimal("1.298"), makeDouble(100.0), makeDecimal("1.298")},
        // Integer promotion case.
        {makeInt32(2147483647), makeDouble(-1), makeInt64(2147483650)},
        // Infinity cases.
        {makeDouble(std::numeric_limits<double>::infinity()),
         makeDouble(10),
         makeDouble(std::numeric_limits<double>::infinity())},
        {makeDouble(std::numeric_limits<double>::infinity()),
         makeDouble(-10),
         makeDouble(std::numeric_limits<double>::infinity())},
        // Decimal128 infinity.
        {makeDecimal("Infinity"), makeDouble(0), makeDecimal("Infinity")},
        {makeDecimal("-Infinity"), makeDouble(0), makeDecimal("-Infinity")},
        {makeDecimal("Infinity"), makeDouble(10), makeDecimal("Infinity")},
        {makeDecimal("Infinity"), makeDouble(-10), makeDecimal("Infinity")},
        // NaN cases.
        {makeDouble(std::numeric_limits<double>::quiet_NaN()),
         makeDouble(1),
         makeDouble(std::numeric_limits<double>::quiet_NaN())},
        {makeDouble(std::numeric_limits<double>::quiet_NaN()),
         makeDouble(-2),
         makeDouble(std::numeric_limits<double>::quiet_NaN())},
        {makeDecimal("NaN"), makeDouble(0), makeDecimal("NaN")},
        {makeDecimal("NaN"), makeDouble(2), makeDecimal("NaN")},
        // Null cases.
        {kNull, makeDouble(-5), kNothing},
        {kNull, makeDouble(5), kNothing},
        {makeDouble(1.1), kNull, kNothing},
        // Nothing cases.
        {kNothing, makeDouble(-5), kNothing},
        {kNothing, makeDouble(5), kNothing},
        {makeDouble(1.1), kNothing, kNothing},
        // Try the limits of the "place" arg (-20 and 100).
        {makeDouble(1.298), makeDouble(100), makeDouble(1.298)},
        {makeDouble(1.298), makeDouble(-20), makeDouble(0)}};

    int testNum = 0;
    for (auto&& testCase : testCases) {
        numAccessor.reset(testCase.num.first, testCase.num.second);
        placeAccessor.reset(testCase.place.first, testCase.place.second);
        ValueGuard expectedResultGuard(testCase.result.first, testCase.result.second);

        // Execute the round function.
        auto [resultTag, resultValue] = runCompiledExpression(compiledRound.get());
        ValueGuard actualResultGuard(resultTag, resultValue);

        auto [compTag, compVal] =
            compareValue(resultTag, resultValue, testCase.result.first, testCase.result.second);
        ASSERT_EQUALS(compTag, TypeTags::NumberInt32) << "unexpected tag for test " << testNum;
        ASSERT_EQUALS(compVal, bitcastFrom<int32_t>(0)) << "unexpected value for test " << testNum;

        testNum++;
    }
}
}  // namespace
}  // namespace mongo::sbe
