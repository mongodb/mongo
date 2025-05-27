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

#include "mongo/db/pipeline/window_function/window_function_set_union.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

namespace mongo {
namespace {

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
    setUnion.add(Value{std::vector<Value>({Value("str"_sd)})});
    ASSERT_EXPECTED_SET(Value{std::vector<Value>({Value("str"_sd)})});
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

    auto largeStr1 = Value("$setUnion is a great window function"_sd);
    auto largeStr2 = Value("$setUnion is still a great window function"_sd);

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
    std::vector<Value> values = {Value("AAA"_sd), Value("aaa"_sd)};
    ASSERT(expCtx->getValueComparator().getEqualTo()(values[0], values[1]));
    setUnion.add(Value(values));

    ASSERT_EXPECTED_SET(Value(std::vector<Value>({Value("AAA"_sd)})));
}

}  // namespace
}  // namespace mongo
