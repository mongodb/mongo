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

#include "mongo/db/pipeline/window_function/window_function_avg.h"

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo {
namespace {

class WindowFunctionAvgTest : public unittest::Test {
public:
    WindowFunctionAvgTest() : avg(WindowFunctionAvg(nullptr)) {}

    WindowFunctionAvg avg;
};

TEST_F(WindowFunctionAvgTest, EmptyWindow) {
    ASSERT_VALUE_EQ(avg.getValue(), Value{BSONNULL});
}

TEST_F(WindowFunctionAvgTest, SingletonWindow) {
    avg.add(Value{5});
    ASSERT_VALUE_EQ(avg.getValue(), Value{5});
}

TEST_F(WindowFunctionAvgTest, IgnoresNonnumeric) {
    avg.add(Value{"not a number"_sd});
    avg.add(Value{1});
    ASSERT_VALUE_EQ(avg.getValue(), Value{1});
}

TEST_F(WindowFunctionAvgTest, SmallWindow) {
    avg.add(Value{5});
    avg.add(Value{2});
    avg.add(Value{10});
    avg.add(Value{3});
    ASSERT_VALUE_EQ(avg.getValue(), Value{5});
}

TEST_F(WindowFunctionAvgTest, Removal) {
    avg.add(Value{5});
    avg.add(Value{2});
    avg.add(Value{10});
    avg.add(Value{3});
    ASSERT_VALUE_EQ(avg.getValue(), Value{5});

    avg.remove(Value{2});
    ASSERT_VALUE_EQ(avg.getValue(), Value{6});

    avg.remove(Value{10});
    ASSERT_VALUE_EQ(avg.getValue(), Value{4});
}

TEST_F(WindowFunctionAvgTest, FloatingPointAverage) {
    avg.add(Value{5});
    avg.add(Value{2});
    ASSERT_VALUE_EQ(avg.getValue(), Value{7 / 2.0});
}

TEST_F(WindowFunctionAvgTest, NarrowestType) {
    avg.add(Value{1});
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDouble);

    avg.add(Value(Decimal128::kPositiveNaN));
    avg.add(Value(Decimal128::kPositiveInfinity));
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDecimal);
    // Returned type narrows after removing inf/nan.
    avg.remove(Value(Decimal128::kPositiveNaN));
    avg.remove(Value(Decimal128::kPositiveInfinity));
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDouble);

    avg.add(Value{1.5});
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDouble);
    avg.add(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDecimal);
    // Returned type narrows after removing all Decimals in window.
    avg.add(Value{Value(Decimal128("1"))});
    avg.remove(Value{Value(Decimal128("-100000000000000000000000000000"))});
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDecimal);
    avg.remove(Value{Value(Decimal128("1"))});
    ASSERT_EQUALS(avg.getValue().getType(), BSONType::numberDouble);
}

TEST_F(WindowFunctionAvgTest, HandleNaNs) {
    Value nan = Value(std::numeric_limits<double>::quiet_NaN());

    avg.add(Value{1});
    avg.add(nan);
    ASSERT_VALUE_EQ(avg.getValue(), nan);
    avg.add(Value{3});
    ASSERT_VALUE_EQ(avg.getValue(), nan);
    avg.remove(nan);
    ASSERT_VALUE_EQ(avg.getValue(), Value{2});
    // We are not preserving the exact type of NaN.
    avg.add(Value(Decimal128::kNegativeNaN));
    ASSERT_VALUE_EQ(avg.getValue(), Value(Decimal128::kPositiveNaN));
    avg.remove(Value(Decimal128::kNegativeNaN));
    avg.add(Value(std::numeric_limits<double>::signaling_NaN()));
    ASSERT_VALUE_EQ(avg.getValue(), nan);
}

TEST_F(WindowFunctionAvgTest, HandleInfs) {
    Value posInf1 = Value(std::numeric_limits<double>::infinity());
    Value negInf1 = Value(-std::numeric_limits<double>::infinity());
    Value posInf2 = Value(Decimal128::kPositiveInfinity);
    Value negInf2 = Value(Decimal128::kNegativeInfinity);
    Value nan = Value(std::numeric_limits<double>::quiet_NaN());

    avg.add(Value{1});  // 1
    avg.add(posInf1);   // 1, (double) inf
    ASSERT_VALUE_EQ(avg.getValue(), posInf1);

    avg.remove(posInf1);  // 1
    ASSERT_VALUE_EQ(avg.getValue(), Value{1});

    avg.add(posInf2);  // 1, (Decimal128) inf
    ASSERT_VALUE_EQ(avg.getValue(), posInf1);

    avg.remove(posInf2);
    avg.add(negInf1);  // 1, - (double) inf
    ASSERT_VALUE_EQ(avg.getValue(), negInf1);

    avg.remove(negInf1);
    avg.add(negInf2);  // 1, - (Decimal128) inf
    ASSERT_VALUE_EQ(avg.getValue(), negInf1);

    avg.add(posInf1);  // 1, -inf, inf
    ASSERT_VALUE_EQ(avg.getValue(), nan);

    avg.remove(posInf1);
    avg.add(nan);  // 1, -inf, nan
    ASSERT_VALUE_EQ(avg.getValue(), nan);

    avg.remove(nan);
    avg.remove(negInf2);  // 1
    ASSERT_VALUE_EQ(avg.getValue(), Value{1});
}

}  // namespace
}  // namespace mongo
