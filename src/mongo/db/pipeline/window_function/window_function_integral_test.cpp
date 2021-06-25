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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/window_function/window_function_integral.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

class WindowFunctionIntegralTest : public AggregationContextFixture {
public:
    WindowFunctionIntegralTest() : expCtx(getExpCtx()), integral(expCtx.get()) {}

    void addValuesToWindow(const std::vector<Value>& values) {
        for (auto val : values)
            integral.add(val);
    }

    void createWindowFunctionIntegralWithUnit(long long kUnitMillis) {
        integral = WindowFunctionIntegral(expCtx.get(), kUnitMillis);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionIntegral integral;
};

TEST_F(WindowFunctionIntegralTest, EmptyWindowShouldReturnNULL) {
    ASSERT_VALUE_EQ(integral.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionIntegralTest, SingleValueShouldReturnZero) {
    const std::vector<Value> singlePoint = {
        Value(std::vector<Value>({Value(0), Value(1)})),
    };
    addValuesToWindow(singlePoint);
    ASSERT_VALUE_EQ(integral.getValue(), Value(0.0));
}

TEST_F(WindowFunctionIntegralTest, SingletonWindow) {
    std::vector<Value> singlePoint = {
        Value(std::vector<Value>({Value(0), Value(1)})),
    };
    addValuesToWindow(singlePoint);
    ASSERT_VALUE_EQ(integral.getValue(), Value(0.0));

    integral.reset();

    singlePoint = std::vector<Value>{
        Value(std::vector<Value>({Value(2), Value(3)})),
    };
    addValuesToWindow(singlePoint);
    // Should still return 0.0 since the window was reset.
    ASSERT_VALUE_EQ(integral.getValue(), Value(0.0));
}

TEST_F(WindowFunctionIntegralTest, SimpleWindowAddition) {
    const std::vector<Value> values = {Value(std::vector<Value>({Value(1), Value(0)})),
                                       Value(std::vector<Value>({Value(3), Value(2)}))};
    addValuesToWindow(values);

    double expectedIntegral = (2 + 0) * (3 - 1) / 2.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    // Adding a new document to the window.
    integral.add(Value(std::vector<Value>({Value(4), Value(1)})));
    expectedIntegral += (2 + 1) * (4 - 3) / 2.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    // Continue to add a new document with a negative Y to the window.
    integral.add(Value(std::vector<Value>({Value(6), Value(-2)})));

    // last-point: (4, 1) -> new-point: (6, -2)
    // delta integral = (4.66667 - 4) * 1 / 2.0 - (6 - 4.66667) * 2 / 2.0 = -1.0
    expectedIntegral -= 1.0;
    ASSERT_LTE(fabs(integral.getValue().coerceToDouble() - expectedIntegral), 1e-5);
}

TEST_F(WindowFunctionIntegralTest, SimpleWindowRemoval) {
    const std::vector<Value> values = {Value(std::vector<Value>({Value(1), Value(0)})),
                                       Value(std::vector<Value>({Value(3), Value(2)})),
                                       Value(std::vector<Value>({Value(5), Value(4)}))};
    addValuesToWindow(values);

    double expectedIntegral = (2 + 0) * (3 - 1) / 2.0 + (4 + 2) * (5 - 3) / 2.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    // Removing the first value in the window.
    integral.remove(values[0]);
    expectedIntegral -= (2 + 0) * (3 - 1) / 2.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    // Continue to remove.
    integral.remove(values[1]);
    expectedIntegral = 0.0;  // Only one document left in the window.
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    // Removing the last value in the window results in a NULL integral.
    integral.remove(values[2]);
    ASSERT_VALUE_EQ(integral.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionIntegralTest, CanHandleNaNValue) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()),
                                  Value(std::numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(3), Value(2)})),
        Value(std::vector<Value>({Value(5), Value(4)}))};
    addValuesToWindow(values);
    // The window contains NaN value, so the returned result should be NaN.
    ASSERT_VALUE_EQ(integral.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    integral.remove(values[0]);  // Remove NaN value resulting a normal value.
    double expectedIntegralWithoutNaN = (2 + 4) * (5 - 3) / 2.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegralWithoutNaN));
}

TEST_F(WindowFunctionIntegralTest, CanHandleInfinity) {
    // Test double infinity.
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(-std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(0), Value(0)})),
        Value(std::vector<Value>({Value(2.0), Value(2.0)})),
    };

    double expectedIntegral = (0 + 2) * (2 - 0) / 2.0;
    addValuesToWindow(values);
    ASSERT_VALUE_EQ(integral.getValue(), Value(std::numeric_limits<double>::infinity()));

    // Remove the infinity value, the window function should still work as usual.
    integral.remove(values[0]);
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    integral.reset();

    // Test Decimal128 infinity.
    values = {
        Value(std::vector<Value>(
            {Value(Decimal128::kNegativeInfinity), Value(Decimal128::kPositiveInfinity)})),
        Value(std::vector<Value>({Value(0), Value(0)})),
        Value(std::vector<Value>({Value(2.0), Value(2.0)})),
    };
    addValuesToWindow(values);
    ASSERT_VALUE_EQ(integral.getValue(), Value(Decimal128::kPositiveInfinity));

    // Remove the infinity value, the window function should still work as usual.
    integral.remove(values[0]);
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));
}

TEST_F(WindowFunctionIntegralTest, ShouldWidenToDecimalOnlyIfNeeded) {
    // 'values' containing Decimal value as input should get a Decimal value in return.
    std::vector<Value> values = {Value(std::vector<Value>({Value(1.0), Value(Decimal128(2))})),
                                 Value(std::vector<Value>({Value(5.0), Value(7.0)}))};
    addValuesToWindow(values);

    auto val = integral.getValue();
    double expectedIntegral = (7 + 2) * (5.0 - 1.0) / 2.0;
    ASSERT_VALUE_EQ(val, Value(expectedIntegral));
    ASSERT_TRUE(val.getType() == NumberDecimal);

    integral.reset();

    // 'values' containing only doubles as input should not get a type-widened value.
    values = std::vector<Value>({Value(std::vector<Value>({Value(1.0), Value(2.0)})),
                                 Value(std::vector<Value>({Value(5.0), Value(7.0)}))});
    addValuesToWindow(values);

    val = integral.getValue();
    ASSERT_VALUE_EQ(val, Value(expectedIntegral));
    ASSERT_TRUE(val.getType() == NumberDouble);
}

TEST_F(WindowFunctionIntegralTest, CanHandleDateTypeWithUnit) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1000)), Value(0)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1002)), Value(2)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1004)), Value(4)})),
    };

    const long long kUnitMillis = 1000;
    createWindowFunctionIntegralWithUnit(kUnitMillis);

    integral.add(values[0]);
    integral.add(values[1]);

    double expectedIntegral = (0 + 2) * (1002 - 1000) / 2.0 / 1000.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    integral.add(values[2]);
    expectedIntegral += (4 + 2) * (1004 - 1002) / 2.0 / 1000.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    integral.remove(values[0]);
    expectedIntegral -= (0 + 2) * (1002 - 1000) / 2.0 / 1000.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));
}

TEST_F(WindowFunctionIntegralTest, DatesWithoutUnitShouldFail) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1000)), Value(2)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1002)), Value(4)})),
        Value(std::vector<Value>({Value(1003), Value(2)})),
        Value(std::vector<Value>({Value(1005), Value(4)})),
    };
    ASSERT_THROWS_CODE(addValuesToWindow(values), AssertionException, 5423902);
}

TEST_F(WindowFunctionIntegralTest, NumbersWithUnitShouldFail) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(3), Value(2)})),
        Value(std::vector<Value>({Value(5), Value(4)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1000)), Value(2)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1002)), Value(4)})),
    };

    const long long kUnitMillis = 1000;
    createWindowFunctionIntegralWithUnit(kUnitMillis);

    ASSERT_THROWS_CODE(addValuesToWindow(values), AssertionException, 5423901);
}

TEST_F(WindowFunctionIntegralTest, ResetShouldNotResetUnit) {
    const std::vector<Value> dateValues = {
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1000)), Value(0)})),
        Value(std::vector<Value>({Value(Date_t::fromMillisSinceEpoch(1002)), Value(2)})),
    };

    const std::vector<Value> numericValues = {
        Value(std::vector<Value>({Value(0), Value(0)})),
        Value(std::vector<Value>({Value(2), Value(2)})),
    };

    const long long kUnitMillis = 1000;
    createWindowFunctionIntegralWithUnit(kUnitMillis);

    addValuesToWindow(dateValues);
    double expectedIntegral = (0 + 2) * (1002 - 1000) / 2.0 / 1000.0;
    ASSERT_VALUE_EQ(integral.getValue(), Value(expectedIntegral));

    integral.reset();

    // Because 'unit' is not reset and is still specified, dates input are expected.
    ASSERT_THROWS_CODE(addValuesToWindow(numericValues), AssertionException, 5423901);
}

TEST_F(WindowFunctionIntegralTest, InputParameterWrongTypeTest) {
    auto dateValue = Value(Date_t::fromMillisSinceEpoch(1000));
    ASSERT_THROWS_CODE(integral.add(dateValue), DBException, 5423900);

    auto emptyArray = Value(std::vector<Value>({}));
    ASSERT_THROWS_CODE(integral.add(emptyArray), DBException, 5423900);

    auto singleton = Value{std::vector<Value>{{Value(5.0)}}};
    ASSERT_THROWS_CODE(integral.add(singleton), DBException, 5423900);

    auto doubleString =
        Value(std::vector<Value>{Value{StringData{"hello"}}, Value{StringData{"world"}}});
    ASSERT_THROWS_CODE(integral.add(doubleString), DBException, 5423900);

    auto str1 = Value(std::vector<Value>{Value{StringData{"hello"}}, Value{1}});
    ASSERT_THROWS_CODE(integral.add(str1), DBException, 5423900);

    auto str2 = Value(std::vector<Value>{Value{1}, Value{StringData{"world"}}});
    ASSERT_THROWS_CODE(integral.add(str2), DBException, 5423900);

    auto str2date = Value(
        std::vector<Value>{Value{Date_t::fromMillisSinceEpoch(1000)}, Value{StringData{"world"}}});
    ASSERT_THROWS_CODE(integral.add(str2), DBException, 5423900);
}


}  // namespace
}  // namespace mongo
