// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_sum.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class WindowFunctionSumTest : public unittest::Test {
public:
    WindowFunctionSumTest() : sum(WindowFunctionSum(nullptr)) {}

    WindowFunctionSum sum;
};

TEST_F(WindowFunctionSumTest, EmptyWindow) {
    ASSERT_VALUE_EQ(sum.getValue(), Value{0});
}

TEST_F(WindowFunctionSumTest, IgnoresNonnumeric) {
    sum.add(Value{"not a number"sv});
    sum.add(Value{1});
    ASSERT_VALUE_EQ(sum.getValue(), Value{1});
}

TEST_F(WindowFunctionSumTest, NarrowestType1) {
    sum.add(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberInt);

    sum.add(Value(std::numeric_limits<double>::quiet_NaN()));
    sum.add(Value(std::numeric_limits<double>::infinity()));
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
    // Returned type narrows after removing inf/nan.
    sum.remove(Value(std::numeric_limits<double>::quiet_NaN()));
    sum.remove(Value(std::numeric_limits<double>::infinity()));
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberInt);

    sum.add(Value{2147483647});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberLong);
    sum.add(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
    sum.add(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDecimal);

    sum.remove(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
    sum.remove(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberLong);
    // Returned type narrows to int if the value fits.
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberInt);
    // Returned type is double if sum overflows long.
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
}

TEST_F(WindowFunctionSumTest, NarrowestType2) {
    // This test is separate because narrowing a double goes through a different code path if a
    // Decimal128 was never added.
    sum.add(Value{1});
    sum.add(Value{1.5});
    sum.add(Value{2147483647});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
    sum.remove(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberLong);
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberInt);
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
}

TEST_F(WindowFunctionSumTest, NarrowestType3) {
    // Test narrowing a long when neither Decimal128 nor double were added.
    sum.add(Value{2147483648ll});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberLong);
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberInt);
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), BSONType::numberDouble);
}

TEST_F(WindowFunctionSumTest, Overflow) {
    int minInt = std::numeric_limits<int>::min();
    long long minLong = std::numeric_limits<long long>::min();
    Value minIntVal = Value(minInt);
    Value minLongVal = Value(minLong);

    sum.add(minIntVal);
    ASSERT_VALUE_EQ(sum.getValue(), minIntVal);
    sum.add(Value{-1});
    // Potential overflow of the accumulated sum is handled in AccumulatorSum.
    ASSERT_VALUE_EQ(sum.getValue(), Value(static_cast<long long>(minInt) - 1));
    // Overflow when negating minInt is handled in RemovableSum.
    sum.remove(minIntVal);
    ASSERT_VALUE_EQ(sum.getValue(), Value{-1});

    sum.add(minLongVal);
    ASSERT_VALUE_EQ(sum.getValue(), Value(static_cast<double>(minLong) - 1));
    sum.remove(minLongVal);
    ASSERT_VALUE_EQ(sum.getValue(), Value{-1});
}

}  // namespace
}  // namespace mongo
