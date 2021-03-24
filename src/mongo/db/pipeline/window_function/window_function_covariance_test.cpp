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
#include "mongo/db/pipeline/window_function/window_function.h"
#include "mongo/db/pipeline/window_function/window_function_covariance.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WindowFunctionCovarianceSampTest : public unittest::Test {
public:
    WindowFunctionCovarianceSampTest() : covariance(nullptr) {}

    WindowFunctionCovarianceSamp covariance;
};

class WindowFunctionCovariancePopTest : public unittest::Test {
public:
    WindowFunctionCovariancePopTest() : covariance(nullptr) {}

    WindowFunctionCovariancePop covariance;
};

void addToWindowCovariance(WindowFunctionCovariance* covariance,
                           const std::vector<Value>& valToAdd) {
    for (auto val : valToAdd) {
        covariance->add(val);
    }
}

// -------------- Test CovarianceSamp window function ----------
TEST_F(WindowFunctionCovarianceSampTest, EmptyWindowShouldReturnNull) {
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionCovarianceSampTest, SingletonWindowShouldReturnNull) {
    covariance.add(Value(std::vector<Value>({Value(1.0), Value(2.0)})));
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionCovarianceSampTest, WindowAddition) {
    const std::vector<Value> valToAdd = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
    };
    addToWindowCovariance(&covariance, valToAdd);

    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.700000), 1e-5);

    // Test addition to the window correctly accumulate the result.
    covariance.add(Value(std::vector<Value>({Value(4.7), Value(3.6)})));
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 2.483334), 1e-5);
}

TEST_F(WindowFunctionCovarianceSampTest, WindowRemoval) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(Decimal128(0)), Value(Decimal128(1.5))})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
        Value(std::vector<Value>({Value(4.7), Value(3.6)})),
    };
    addToWindowCovariance(&covariance, values);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 2.483334), 1e-5);

    covariance.remove(values[0]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.815000), 1e-5);

    // Adding back the value just removed should result in the same value as before.
    covariance.add(values[0]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 2.483334), 1e-5);
    covariance.remove(values[0]);

    covariance.remove(values[1]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
    covariance.remove(values[2]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionCovarianceSampTest, CanHandleNaN) {
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()),
                                  Value(std::numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains NaN value, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    covariance.remove(values[0]);  // Remove the NaN value in the window.
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.0), 1e-5);

    covariance.reset();

    values = std::vector<Value>({
        Value(
            std::vector<Value>({Value(Decimal128::kPositiveNaN), Value(Decimal128::kPositiveNaN)})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    });
    addToWindowCovariance(&covariance, values);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveNaN));

    covariance.remove(values[0]);  // Remove the NaN value in the window.
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.0), 1e-5);
}

TEST_F(WindowFunctionCovarianceSampTest, CanHandleInfinity) {
    // Test double infinity.
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(-std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains infinity values of different sign, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    // Remove the NaN/infinite value in the window. The remaining inf value should be resolved to
    // the corresponding inf value.
    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::infinity()));

    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.0), 1e-5);

    covariance.reset();

    // Test Decimal128 infinity.
    values = {
        Value(std::vector<Value>(
            {Value(Decimal128::kNegativeInfinity), Value(Decimal128::kPositiveNaN)})),
        Value(
            std::vector<Value>({Value(Decimal128::kPositiveNaN), Value(Decimal128::kPositiveNaN)})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains infinity values of different sign, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveNaN));

    // Remove the NaN/infinite value in the window. The remaining inf value should be resolved to
    // the corresponding inf value.
    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveNaN));

    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.0), 1e-5);
}

TEST_F(WindowFunctionCovarianceSampTest, ReturnNaNOverInfIfExistsBoth) {
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()),
                                  Value(std::numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // When the window contains both infinity and NaN, the result should be NaN rather than Inf.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::infinity()));
    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.0), 1e-5);
}

// -------------- Test CovariancePop window function ----------
TEST_F(WindowFunctionCovariancePopTest, EmptyWindowShouldReturnNull) {
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionCovariancePopTest, SingletonWindowShouldReturnZero) {
    covariance.add(Value(std::vector<Value>({Value(1.0), Value(2.0)})));
    ASSERT_VALUE_EQ(covariance.getValue(), Value(0.0));
}

TEST_F(WindowFunctionCovariancePopTest, WindowAddition) {
    const std::vector<Value> valToAdd = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
    };
    addToWindowCovariance(&covariance, valToAdd);

    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.350000), 1e-5);

    // Test addition to the window correctly accumulate the result.
    covariance.add(Value(std::vector<Value>({Value(4.7), Value(3.6)})));
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.655556), 1e-5);
}

TEST_F(WindowFunctionCovariancePopTest, WindowRemoval) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(Decimal128(1.4)), Value(Decimal128(2.5))})),
        Value(std::vector<Value>({Value(4.7), Value(3.6)})),
    };
    addToWindowCovariance(&covariance, values);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.655556), 1e-5);

    covariance.remove(values[0]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.907500), 1e-5);

    // Adding back the value just removed should result in the same value as before.
    covariance.add(values[0]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.655556), 1e-5);
    covariance.remove(values[0]);

    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.0), 1e-5);
    covariance.remove(values[2]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
}

TEST_F(WindowFunctionCovariancePopTest, CanHandleNaN) {
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()),
                                  Value(std::numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains NaN value, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    covariance.remove(values[0]);  // Remove the NaN value in the window.
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);

    covariance.reset();

    values = std::vector<Value>({
        Value(
            std::vector<Value>({Value(Decimal128::kPositiveNaN), Value(Decimal128::kPositiveNaN)})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    });
    addToWindowCovariance(&covariance, values);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveNaN));

    covariance.remove(values[0]);  // Remove the NaN value in the window.
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);
}

TEST_F(WindowFunctionCovariancePopTest, CanHandleInfinity) {
    // Test double infinity.
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(-std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains infinity values of different sign, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    // Remove the NaN/infinite value in the window. The remaining inf value should be resolved to
    // the corresponding inf value.
    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::infinity()));

    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);

    covariance.reset();

    // Test Decimal128 infinity.
    values = {
        Value(std::vector<Value>(
            {Value(Decimal128::kNegativeInfinity), Value(Decimal128::kPositiveInfinity)})),
        Value(std::vector<Value>(
            {Value(Decimal128::kPositiveInfinity), Value(Decimal128::kPositiveInfinity)})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // The window contains infinity values of different sign, so the result should be NaN.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveNaN));

    // Remove the NaN/infinite value in the window. The remaining inf value should be resolved to
    // the corresponding inf value.
    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(Decimal128::kPositiveInfinity));

    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);
}

TEST_F(WindowFunctionCovariancePopTest, ReturnNaNOverInfIfExistsBoth) {
    std::vector<Value> values = {
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()),
                                  Value(std::numeric_limits<double>::quiet_NaN())})),
        Value(std::vector<Value>({Value(std::numeric_limits<double>::infinity()),
                                  Value(std::numeric_limits<double>::infinity())})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
    };
    addToWindowCovariance(&covariance, values);
    // When the window contains both infinity and NaN, the result should be NaN rather than Inf.
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::quiet_NaN()));

    covariance.remove(values[0]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(std::numeric_limits<double>::infinity()));
    covariance.remove(values[1]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);
}

TEST_F(WindowFunctionCovariancePopTest, NonNumericTypesHaveNoImpactOnCovariance) {
    const std::string str = "non-numeric-type";
    const std::vector<Value> values = {
        // Numeric type check is before NaN check, so this value should not cause NaN result.
        Value(std::vector<Value>({Value(std::numeric_limits<double>::quiet_NaN()), Value(str)})),
        Value(std::vector<Value>({Value(1.0), Value(2.0)})),
        Value(std::vector<Value>({Value(str), Value(2.0)})),
        Value(std::vector<Value>({Value(1.0), Value(BSONNULL)})),
        Value(std::vector<Value>({Value(2.0), Value(4.0)})),
        Value(std::vector<Value>({Value(str), Value(BSONNULL)})),
    };
    addToWindowCovariance(&covariance, values);

    // Note that non-numeric input simply has no impact on coavariance and won't throw or fail the
    // computation.

    // Only numeric values 'values[1]' and 'values[4]' should be considered "valid".
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);

    // Removing a non-numeric value, covariance should remain the same.
    covariance.remove(values[0]);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.5), 1e-5);

    // Remove a numeric value.
    covariance.remove(values[1]);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(0.0));
}

TEST_F(WindowFunctionCovariancePopTest, NonDecimalNumericResultShouldBeCoercedToDouble) {
    ASSERT_VALUE_EQ(covariance.getValue(), Value(BSONNULL));
    covariance.add(Value(std::vector<Value>({Value(0), Value(1)})));

    ASSERT_EQUALS(covariance.getValue().getType(), NumberDouble);
    ASSERT_VALUE_EQ(covariance.getValue(), Value(0.0));

    covariance.add(Value(std::vector<Value>({Value(1), Value(2)})));
    ASSERT_EQUALS(covariance.getValue().getType(), NumberDouble);
}

TEST_F(WindowFunctionCovariancePopTest, WidenTypeToDecimalOnlyIfNeeded) {
    const std::vector<Value> values = {
        Value(std::vector<Value>({Value(0), Value(1.5)})),
        Value(std::vector<Value>({Value(1.4), Value(2.5)})),
    };
    addToWindowCovariance(&covariance, values);

    ASSERT_EQUALS(covariance.getValue().getType(), NumberDouble);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 0.350000), 1e-5);

    covariance.add(Value(std::vector<Value>({Value(Decimal128(4.7)), Value(Decimal128(3.6))})));
    ASSERT_EQUALS(covariance.getValue().getType(), NumberDecimal);
    ASSERT_LTE(fabs(covariance.getValue().coerceToDouble() - 1.655556), 1e-5);
}

}  // namespace
}  // namespace mongo
