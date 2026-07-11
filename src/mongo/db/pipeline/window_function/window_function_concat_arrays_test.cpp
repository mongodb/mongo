// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_concat_arrays.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    concatArrays.add(Value{std::vector<Value>({Value("str"sv)})});
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{std::vector<Value>({Value("str"sv)})});
}

TEST_F(WindowFunctionConcatArraysTest, ComplexWindowPreservesInsertionOrder) {
    std::vector<Value> values = {Value(std::vector<Value>({Value(1), Value(2)})),
                                 Value(std::vector<Value>({Value("three"sv), Value("four"sv)})),
                                 Value(std::vector<Value>({Value(BSONObj())}))};

    std::vector<Value> expected = {
        Value(1), Value(2), Value("three"sv), Value("four"sv), Value(BSONObj())};

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
            Value(std::vector<Value>({Value("In a double nested array"sv)})),
            Value(std::vector<Value>({Value("Also in a double nested array"sv)})),
        })),
        Value(std::vector<Value>({Value("Only singly nested"sv)})),
        Value(std::vector<Value>({Value(std::vector<Value>({Value(1), Value(2)}))}))};

    std::vector<Value> expected = {
        Value(std::vector<Value>({Value("In a double nested array"sv)})),
        Value(std::vector<Value>({Value("Also in a double nested array"sv)})),
        Value("Only singly nested"sv),
        Value(std::vector<Value>({Value(1), Value(2)}))};

    addValuesToWindow(values);
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});

    concatArrays.remove(values[0]);
    expected = {Value("Only singly nested"sv), Value(std::vector<Value>({Value(1), Value(2)}))};
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value{expected});
}

TEST_F(WindowFunctionConcatArraysTest, RemoveExactlyAllValuesInWindow) {
    std::vector<Value> value = {Value(1), Value(2)};

    concatArrays.add(Value(value));
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(value));

    concatArrays.remove(Value(value));
    ASSERT_VALUE_EQ(concatArrays.getValue(), Value(std::vector<Value>({})));
}

using WindowFunctionConcatArraysTestDeathTest = WindowFunctionConcatArraysTest;
DEATH_TEST_F(WindowFunctionConcatArraysTestDeathTest,
             CannotRemoveFromEmptyWindowFunction,
             "1628401") {
    concatArrays.reset();

    std::vector<Value> attemptToRemove = {Value(1), Value(2), Value(2)};
    concatArrays.remove(Value(attemptToRemove));
}

DEATH_TEST_F(WindowFunctionConcatArraysTestDeathTest, CannotRemoveNonArrayFromWindow, "1628400") {
    concatArrays.reset();

    std::vector<Value> value = {Value(1), Value(2)};
    concatArrays.add(Value(value));

    concatArrays.remove(Value(1));
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
        Value(std::vector<Value>({Value("$concatArrays is a great window function"sv)}));
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
