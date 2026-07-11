// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_set_union.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class WindowFunctionSetUnionTest : public AggregationContextFixture {
public:
    WindowFunctionSetUnionTest() : expCtx(getExpCtx()), setUnion(expCtx.get()) {
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        expCtx->setCollator(std::move(collator));
    }

    void addValuesToWindow(const std::vector<Value>& values) {
        for (const auto& val : values) {
            setUnion.add(val);
        }
    }

    /**
     * Asserts that all elements contained in the array 'result' are contained in the array
     * 'expected' and vice versa.
     */
    void ASSERT_EXPECTED_SET(Value expected) {
        auto result = setUnion.getValue();

        ASSERT_EQUALS(true, result.isArray());
        ASSERT_EQUALS(true, expected.isArray());
        // ASSERT_EQUALS(result.getArrayLength(), expected.getArrayLength());

        std::vector<Value> resultArray = result.getArray();
        std::sort(
            resultArray.begin(), resultArray.end(), expCtx->getValueComparator().getLessThan());

        std::vector<Value> expectedArray = expected.getArray();
        std::sort(
            expectedArray.begin(), expectedArray.end(), expCtx->getValueComparator().getLessThan());

        ASSERT_VALUE_EQ(Value(resultArray), Value(expectedArray));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionSetUnion setUnion;
};

TEST_F(WindowFunctionSetUnionTest, EmptyWindowReturnsDefault) {
    ASSERT_EXPECTED_SET(Value{std::vector<Value>()});

    setUnion.add(Value{std::vector<Value>({Value(1)})});
    setUnion.remove(Value{std::vector<Value>({Value(1)})});
    ASSERT_EXPECTED_SET(Value{std::vector<Value>()});
}

TEST_F(WindowFunctionSetUnionTest, SingleInsertionShouldReturnAVector) {
    setUnion.add(Value{std::vector<Value>({Value(1)})});
    ASSERT_EXPECTED_SET(Value{std::vector<Value>({Value(1)})});

    setUnion.reset();
    setUnion.add(Value{std::vector<Value>({Value("str"sv)})});
    ASSERT_EXPECTED_SET(Value{std::vector<Value>({Value("str"sv)})});
}

TEST_F(WindowFunctionSetUnionTest, ComplexWindow) {
    std::vector<Value> values = {
        Value(std::vector<Value>({Value{2}, Value{5}})),
        Value(std::vector<Value>({Value{std::string("str")}, Value{BSONObj()}}))};

    std::vector<Value> expected = {Value{2}, Value{5}, Value{std::string("str")}, Value{BSONObj()}};

    addValuesToWindow(values);
    ASSERT_EXPECTED_SET(Value{expected});
}

TEST_F(WindowFunctionSetUnionTest, Removal) {
    std::vector<Value> values = {Value(1), Value(2)};

    setUnion.add(Value(values));
    ASSERT_EXPECTED_SET(Value(values));

    setUnion.remove(Value(std::vector<Value>({Value{1}, Value{2}})));
    std::vector<Value> expected = {};
    ASSERT_EXPECTED_SET(Value{expected});

    addValuesToWindow({Value(std::vector<Value>({Value(1), Value(2)})),
                       Value(std::vector<Value>({Value(3), Value(4)}))});
    ASSERT_EXPECTED_SET(Value(std::vector<Value>({Value(1), Value(2), Value(3), Value(4)})));

    setUnion.remove(Value(std::vector<Value>({Value(1), Value(2)})));
    ASSERT_EXPECTED_SET(Value(std::vector<Value>({Value(3), Value(4)})));
}

TEST_F(WindowFunctionSetUnionTest, NotAllowDuplicates) {
    addValuesToWindow({Value(std::vector<Value>({Value{1}, Value{1}, Value{2}, Value{3}}))});

    // $addToSet window function returns a vector of values that are in a set excluding duplicates.
    std::vector<Value> expected = {Value{1}, Value{2}, Value{3}};
    ASSERT_EXPECTED_SET(Value{expected});

    setUnion.remove(Value(std::vector<Value>({Value(1)})));
    // Have to remove element '1' twice to remove it from the set.
    ASSERT_EXPECTED_SET(Value{expected});

    setUnion.remove(Value(std::vector<Value>({Value(2)})));
    expected = std::vector<Value>({Value{1}, Value{3}});
    ASSERT_EXPECTED_SET(Value{expected});
}

TEST_F(WindowFunctionSetUnionTest, SetUnionOnlyAcceptsArrays) {
    // $setUnion should refuse to insert a non-array value.
    Value badValue = Value(1);
    ASSERT_THROWS_CODE(setUnion.add(badValue), AssertionException, ErrorCodes::TypeMismatch);
}

TEST_F(WindowFunctionSetUnionTest, TracksMemoryUsageOnAddAndRemove) {
    size_t trackingSize = sizeof(WindowFunctionSetUnion);
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    auto largeStr1 = Value("$setUnion is a great window function"sv);
    auto largeStr2 = Value("$setUnion is still a great window function"sv);

    setUnion.add(Value(std::vector<Value>({largeStr1})));
    trackingSize += largeStr1.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    setUnion.add(Value(std::vector<Value>({largeStr2})));
    trackingSize += largeStr2.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    setUnion.remove(Value(std::vector<Value>({largeStr2})));
    trackingSize -= largeStr2.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    setUnion.remove(Value(std::vector<Value>({largeStr1})));
    trackingSize -= largeStr1.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    setUnion.add(Value(std::vector<Value>({largeStr1, largeStr2})));
    trackingSize += largeStr1.getApproximateSize() + largeStr2.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);

    setUnion.remove(Value(std::vector<Value>({largeStr1, largeStr2})));
    trackingSize -= largeStr1.getApproximateSize() + largeStr2.getApproximateSize();
    ASSERT_EQ(setUnion.getApproximateSize(), trackingSize);
}

TEST_F(WindowFunctionSetUnionTest, WindowFunctionSetUnionRespectsCollation) {
    std::vector<Value> values = {Value("AAA"sv), Value("aaa"sv)};
    ASSERT(expCtx->getValueComparator().getEqualTo()(values[0], values[1]));
    setUnion.add(Value(values));

    ASSERT_EXPECTED_SET(Value(std::vector<Value>({Value("AAA"sv)})));
}

}  // namespace
}  // namespace mongo
