/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

class WindowFunctionConcatArraysTest : public AggregationContextFixture {
public:
    WindowFunctionConcatArraysTest() : expCtx(getExpCtx()), concatArrays(expCtx.get()) {}

    void addValuesToWindow(const std::vector<Value>& values) {
        for (const auto& val : values) {
            concatArrays.add(val);
        }
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionConcatArrays concatArrays;
};

TEST_F(WindowFunctionConcatArraysTest, EmptyWindowReturnsDefault) {
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{std::vector<Value>()});

    concatArrays.add(Value{std::vector<Value>({Value(1)})});
    concatArrays.remove(Value{std::vector<Value>({Value(1)})});
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{std::vector<Value>()});
}

TEST_F(WindowFunctionConcatArraysTest, SingleInsertionShouldReturnAVector) {
    concatArrays.add(Value{std::vector<Value>({Value(1)})});
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{std::vector<Value>({Value(1)})});

    concatArrays.reset();
    concatArrays.add(Value{std::vector<Value>({Value("str"_sd)})});
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{std::vector<Value>({Value("str"_sd)})});
}

TEST_F(WindowFunctionConcatArraysTest, ComplexWindowPreservesInsertionOrder) {
    std::vector<Value> values = {Value(std::vector<Value>({Value(1), Value(2)})),
                                 Value(std::vector<Value>({Value("three"_sd), Value("four"_sd)})),
                                 Value(std::vector<Value>({Value(BSONObj())}))};

    std::vector<Value> expected = {
        Value(1), Value(2), Value("three"_sd), Value("four"_sd), Value(BSONObj())};

    addValuesToWindow(values);
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, Removal) {
    addValuesToWindow(std::vector<Value>({
        Value(std::vector<Value>({Value(1), Value(2)})),
        Value(std::vector<Value>({Value(3), Value(4)})),
    }));

    concatArrays.remove(Value(std::vector<Value>({Value(1), Value(2)})));
    std::vector<Value> expected = {Value(3), Value(4)};

    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, AllowsDuplicates) {
    addValuesToWindow(std::vector<Value>({
        Value(std::vector<Value>({Value(1), Value(1)})),
        Value(std::vector<Value>({Value(2), Value(3)})),
        Value(std::vector<Value>({Value(1), Value(1)})),
    }));

    // $concatArrays allows duplicates in the array returned by the window function
    std::vector<Value> expected = {Value(1), Value(1), Value(2), Value(3), Value(1), Value(1)};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});

    // remove() '{1, 1}' only removes one instance of value
    std::vector<Value> toRemove = {Value(1), Value(1)};
    concatArrays.remove(Value(toRemove));

    expected = {Value(2), Value(3), Value(1), Value(1)};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, RemovalDoesNotAffectOrder) {
    addValuesToWindow(std::vector<Value>({Value(std::vector<Value>({Value(1), Value(2)})),
                                          Value(std::vector<Value>({Value(3), Value(4)})),
                                          Value(std::vector<Value>({Value(5), Value(6)}))}));

    std::vector<Value> expected = {Value(1), Value(2), Value(3), Value(4), Value(5), Value(6)};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(expected));

    concatArrays.remove(Value(std::vector<Value>({Value(1), Value(2)})));
    expected = {Value(3), Value(4), Value(5), Value(6)};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, DoubleNestedArraysShouldReturnSingleNestedArrays) {
    std::vector<Value> values = {
        Value(std::vector<Value>({
            Value(std::vector<Value>({Value("In a double nested array"_sd)})),
            Value(std::vector<Value>({Value("Also in a double nested array"_sd)})),
        })),
        Value(std::vector<Value>({Value("Only singly nested"_sd)})),
        Value(std::vector<Value>({Value(std::vector<Value>({Value(1), Value(2)}))}))};

    std::vector<Value> expected = {
        Value(std::vector<Value>({Value("In a double nested array"_sd)})),
        Value(std::vector<Value>({Value("Also in a double nested array"_sd)})),
        Value("Only singly nested"_sd),
        Value(std::vector<Value>({Value(1), Value(2)}))};

    addValuesToWindow(values);
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});

    concatArrays.remove(values[0]);
    expected = {Value("Only singly nested"_sd), Value(std::vector<Value>({Value(1), Value(2)}))};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, RemoveExactlyAllValuesInWindow) {
    std::vector<Value> value = {Value(1), Value(2)};

    concatArrays.add(Value(value));
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(value));

    concatArrays.remove(Value(value));
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(std::vector<Value>({})));
}

DEATH_TEST_F(WindowFunctionConcatArraysTest, CannotRemoveFromEmptyWindowFunction, "tassert") {
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(std::vector<Value>({})));

    std::vector<Value> attemptToRemove = {Value(1), Value(2), Value(2)};
    ASSERT_THROWS_CODE(concatArrays.remove(Value(attemptToRemove)), AssertionException, 1628401);
}

DEATH_TEST_F(WindowFunctionConcatArraysTest, CannotRemoveNonArrayFromWindow, "tassert") {
    std::vector<Value> value = {Value(1), Value(2)};

    concatArrays.add(Value(value));
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(value));

    ASSERT_THROWS_CODE(concatArrays.remove(Value(1)), AssertionException, 1628400);
}

TEST_F(WindowFunctionConcatArraysTest, ConcatArraysOnlyAcceptsArrays) {
    // $concatArrays should refuse to insert a non-array value
    Value badValue = Value(1);
    ASSERT_THROWS_CODE(concatArrays.add(badValue), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(WindowFunctionConcatArraysTest, TracksMemoryUsageOnAddAndRemove) {
    size_t trackingSize = sizeof(WindowFunctionConcatArrays);
    ASSERT_EQ(concatArrays.getApproximateSize(), trackingSize);

    auto largeStrInArr =
        Value(std::vector<Value>({Value("$concatArrays is a great window function"_sd)}));
    concatArrays.add(largeStrInArr);
    trackingSize += largeStrInArr.getApproximateSize();
    ASSERT_EQ(concatArrays.getApproximateSize(), trackingSize);

    concatArrays.add(largeStrInArr);
    trackingSize += largeStrInArr.getApproximateSize();
    ASSERT_EQ(concatArrays.getApproximateSize(), trackingSize);

    concatArrays.remove(largeStrInArr);
    trackingSize -= largeStrInArr.getApproximateSize();
    ASSERT_EQ(concatArrays.getApproximateSize(), trackingSize);
}

}  // namespace
}  // namespace mongo
