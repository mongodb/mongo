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

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/window_function/window_function_min_max.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
// TODO SERVER-57884: Add test cases for $firstN/$lastN window functions.
// TODO SERVER-57886: Add test cases for $top/$bottom/$topN/$bottomN window functions.
class WindowFunctionMinMaxNTest : public AggregationContextFixture {
public:
    static constexpr auto kNarg = 3LL;
    WindowFunctionMinMaxNTest()
        : expCtx(getExpCtx()), minThree(expCtx.get(), kNarg), maxThree(expCtx.get(), kNarg) {
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        expCtx->setCollator(std::move(collator));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionMinN minThree;
    WindowFunctionMaxN maxThree;
};

TEST_F(WindowFunctionMinMaxNTest, EmptyWindow) {
    auto test = [](auto windowFunction) {
        ASSERT_VALUE_EQ(windowFunction.getValue(), Value{BSONArray()});

        // No matter how many nullish values we insert, we should still get back the empty array.
        windowFunction.add(Value());
        windowFunction.add(Value(BSONNULL));
        windowFunction.add(Value());
        windowFunction.add(Value(BSONNULL));
        ASSERT_VALUE_EQ(windowFunction.getValue(), Value{BSONArray()});

        // Add a value and show that removing nullish has no effect.
        windowFunction.add(Value{3});
        ASSERT_VALUE_EQ(windowFunction.getValue(), Value{std::vector{Value(3)}});

        windowFunction.remove(Value());
        windowFunction.remove(Value(BSONNULL));
        windowFunction.remove(Value());
        windowFunction.remove(Value(BSONNULL));
        ASSERT_VALUE_EQ(windowFunction.getValue(), Value{std::vector{Value(3)}});
    };
    test(minThree);
    test(maxThree);
}

TEST_F(WindowFunctionMinMaxNTest, WindowSmallerThanN) {
    minThree.add(Value{5});
    minThree.add(Value{7});

    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(5), Value(7)}));

    maxThree.add(Value{5});
    maxThree.add(Value{7});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(7), Value(5)}));
}

TEST_F(WindowFunctionMinMaxNTest, WindowContainsDuplicates) {
    minThree.add(Value{5});
    minThree.add(Value{7});
    minThree.add(Value{7});
    minThree.add(Value{7});
    minThree.add(Value{7});

    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(5), Value(7), Value(7)}));

    maxThree.add(Value{5});
    maxThree.add(Value{5});
    maxThree.add(Value{5});
    maxThree.add(Value{5});
    maxThree.add(Value{5});
    maxThree.add(Value{7});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(7), Value(5), Value(5)}));
}

TEST_F(WindowFunctionMinMaxNTest, BasicCorrectnessTest) {
    minThree.add(Value{5});
    minThree.add(Value{10});
    minThree.add(Value{6});
    minThree.add(Value{12});
    minThree.add(Value{7});
    minThree.add(Value{3});

    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(3), Value(5), Value(6)}));

    minThree.remove(Value{5});
    minThree.remove(Value{10});

    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(3), Value(6), Value(7)}));
    minThree.remove(Value{6});
    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(3), Value(7), Value(12)}));

    minThree.remove(Value{12});
    minThree.remove(Value{7});
    minThree.remove(Value{3});
    ASSERT_VALUE_EQ(minThree.getValue(), Value{BSONArray()});

    maxThree.add(Value{5});
    maxThree.add(Value{9});
    maxThree.add(Value{12});
    maxThree.add(Value{11});
    maxThree.add(Value{3});
    maxThree.add(Value{7});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(12), Value(11), Value(9)}));

    maxThree.remove(Value{5});
    maxThree.remove(Value{9});
    maxThree.remove(Value{12});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(11), Value(7), Value(3)}));

    maxThree.remove(Value{11});
    maxThree.remove(Value{3});
    maxThree.remove(Value{7});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value{BSONArray()});
}

TEST_F(WindowFunctionMinMaxNTest, MixNullsAndNonNulls) {
    // Add four values, half of which are null/missing. We should only return the two non-nulls.
    minThree.add(Value{4});
    minThree.add(Value());
    minThree.add(Value(BSONNULL));
    minThree.add(Value{1});
    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(1), Value(4)}));

    // Add a couple more values. We should still get no nulls/missing.
    minThree.add(Value{3});
    minThree.add(Value());
    minThree.add(Value(BSONNULL));
    minThree.add(Value{2});
    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{Value(1), Value(2), Value(3)}));

    // Add four values, half of which are null/missing. We should only return the two non-nulls.
    maxThree.add(Value{4});
    maxThree.add(Value());
    maxThree.add(Value(BSONNULL));
    maxThree.add(Value{1});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(4), Value(1)}));

    // Add a couple more values. We should still get no nulls/missing.
    maxThree.add(Value{3});
    maxThree.add(Value());
    maxThree.add(Value(BSONNULL));
    maxThree.add(Value{2});
    ASSERT_VALUE_EQ(maxThree.getValue(), Value(std::vector{Value(4), Value(3), Value(2)}));
}

TEST_F(WindowFunctionMinMaxNTest, Ties) {
    // When two elements tie (compare equal), remove() can't pick an arbitrary one,
    // because that would break the invariant that 'add(x); add(y); remove(x)' is equivalent to
    // 'add(y)'.

    auto x = Value{"foo"_sd};
    auto y = Value{"FOO"_sd};
    // x and y are distinguishable,
    ASSERT_VALUE_NE(x, y);
    // but they compare equal according to the ordering.
    ASSERT(expCtx->getValueComparator().evaluate(x == y));

    minThree.add(x);
    minThree.add(y);
    minThree.remove(x);
    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{y}));

    minThree.add(x);
    minThree.add(y);
    minThree.remove(x);

    // Here, we expect ["foo","FOO"] because we remove the first added entry that compares equal
    // to 'x', which is the first instance of 'y'.
    ASSERT_VALUE_EQ(minThree.getValue(), Value(std::vector{x, y}));
}

TEST_F(WindowFunctionMinMaxNTest, TracksMemoryUsageOnAddAndRemove) {
    size_t trackingSize = sizeof(WindowFunctionMinN);
    ASSERT_EQ(minThree.getApproximateSize(), trackingSize);

    auto largeStr = Value{"$minN/maxN are great window functions"_sd};
    minThree.add(largeStr);
    trackingSize += largeStr.getApproximateSize();
    ASSERT_EQ(minThree.getApproximateSize(), trackingSize);

    minThree.add(largeStr);
    trackingSize += largeStr.getApproximateSize();
    ASSERT_EQ(minThree.getApproximateSize(), trackingSize);

    minThree.remove(largeStr);
    trackingSize -= largeStr.getApproximateSize();
    ASSERT_EQ(minThree.getApproximateSize(), trackingSize);
}
}  // namespace
}  // namespace mongo
