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
#include "mongo/db/pipeline/window_function/window_function_sum.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class WindowFunctionSumTest : public unittest::Test {
public:
    WindowFunctionSumTest() : sum(WindowFunctionSum(nullptr)) {}

    WindowFunctionSum sum;
};

TEST_F(WindowFunctionSumTest, EmptyWindow) {
    ASSERT_VALUE_EQ(sum.getValue(), Value{0});
}

TEST_F(WindowFunctionSumTest, IgnoresNonnumeric) {
    sum.add(Value{"not a number"_sd});
    sum.add(Value{1});
    ASSERT_VALUE_EQ(sum.getValue(), Value{1});
}

TEST_F(WindowFunctionSumTest, NarrowestType1) {
    sum.add(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), NumberInt);

    sum.add(Value(std::numeric_limits<double>::quiet_NaN()));
    sum.add(Value(std::numeric_limits<double>::infinity()));
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
    // Returned type narrows after removing inf/nan.
    sum.remove(Value(std::numeric_limits<double>::quiet_NaN()));
    sum.remove(Value(std::numeric_limits<double>::infinity()));
    ASSERT_EQUALS(sum.getValue().getType(), NumberInt);

    sum.add(Value{2147483647});
    ASSERT_EQUALS(sum.getValue().getType(), NumberLong);
    sum.add(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
    sum.add(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(sum.getValue().getType(), NumberDecimal);

    sum.remove(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
    sum.remove(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), NumberLong);
    // Returned type narrows to int if the value fits.
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), NumberInt);
    // Returned type is double if sum overflows long.
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
}

TEST_F(WindowFunctionSumTest, NarrowestType2) {
    // This test is separate because narrowing a double goes through a different code path if a
    // Decimal128 was never added.
    sum.add(Value{1});
    sum.add(Value{1.5});
    sum.add(Value{2147483647});
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
    sum.remove(Value{1.5});
    ASSERT_EQUALS(sum.getValue().getType(), NumberLong);
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), NumberInt);
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
}

TEST_F(WindowFunctionSumTest, NarrowestType3) {
    // Test narrowing a long when neither Decimal128 nor double were added.
    sum.add(Value{2147483648ll});
    ASSERT_EQUALS(sum.getValue().getType(), NumberLong);
    sum.remove(Value{1});
    ASSERT_EQUALS(sum.getValue().getType(), NumberInt);
    sum.add(Value(std::numeric_limits<long long>::max()));
    ASSERT_EQUALS(sum.getValue().getType(), NumberDouble);
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
