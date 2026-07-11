// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/window_function/window_function_min_max.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class WindowFunctionMinMaxTest : public AggregationContextFixture {
public:
    WindowFunctionMinMaxTest() : expCtx(getExpCtx()), min(expCtx.get()), max(expCtx.get()) {
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        expCtx->setCollator(std::move(collator));
    }


    boost::intrusive_ptr<ExpressionContext> expCtx;
    WindowFunctionMin min;
    WindowFunctionMax max;
};

TEST_F(WindowFunctionMinMaxTest, EmptyWindow) {
    ASSERT_VALUE_EQ(min.getValue(), Value{BSONNULL});

    ASSERT_VALUE_EQ(max.getValue(), Value{BSONNULL});
}

TEST_F(WindowFunctionMinMaxTest, SingletonWindow) {
    min.add(Value{5});
    ASSERT_VALUE_EQ(min.getValue(), Value{5});

    max.add(Value{7});
    ASSERT_VALUE_EQ(max.getValue(), Value{7});
}

TEST_F(WindowFunctionMinMaxTest, SmallWindow) {
    min.add(Value{5});
    min.add(Value{2});
    min.add(Value{10});
    min.add(Value{3});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    max.add(Value{7});
    max.add(Value{1});
    max.add(Value{8});
    max.add(Value{4});
    ASSERT_VALUE_EQ(max.getValue(), Value{8});
}

TEST_F(WindowFunctionMinMaxTest, NullsAndMissing) {
    min.add(Value{2});

    // Nulls should be ignored.
    min.add(Value{BSONNULL});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    // Missing values should be ignored.
    min.add(Value());
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    // Removal of null/missing values is a no-op.
    min.remove(Value{BSONNULL});
    min.remove(Value());
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    max.add(Value{BSONNULL});
    max.add(Value{8});

    // Nulls should be ignored.
    ASSERT_VALUE_EQ(max.getValue(), Value{8});

    // Missing values should be ignored.
    max.add(Value());
    ASSERT_VALUE_EQ(max.getValue(), Value{8});

    // Removal of null/missing values is a no-op.
    max.remove(Value{BSONNULL});
    max.remove(Value());
    ASSERT_VALUE_EQ(max.getValue(), Value{8});
}

TEST_F(WindowFunctionMinMaxTest, Removal) {
    min.add(Value{5});
    min.add(Value{2});
    min.add(Value{10});
    min.add(Value{3});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    min.remove(Value{5});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    min.remove(Value{2});
    ASSERT_VALUE_EQ(min.getValue(), Value{3});
}

TEST_F(WindowFunctionMinMaxTest, Duplicates) {
    min.add(Value{2});
    min.add(Value{2});
    min.add(Value{99});
    min.add(Value{77});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    // Removing one instance of the min isn't enough.
    min.remove(Value{2});
    ASSERT_VALUE_EQ(min.getValue(), Value{2});

    // The min changes only once all instances are removed.
    min.remove(Value{2});
    ASSERT_VALUE_EQ(min.getValue(), Value{77});
}

TEST_F(WindowFunctionMinMaxTest, Ties) {
    // When two elements tie (compare equal), remove() can't pick an arbitrary one,
    // because that would break the invariant that 'add(x); add(y); remove(x)' is equivalent to
    // 'add(y)'.

    auto x = Value{"foo"sv};
    auto y = Value{"FOO"sv};
    // x and y are distinguishable,
    ASSERT_VALUE_NE(x, y);
    // but they compare equal according to the ordering.
    ASSERT(expCtx->getValueComparator().evaluate(x == y));

    min.add(x);
    min.add(y);
    min.remove(x);
    ASSERT_VALUE_EQ(min.getValue(), y);

    max.add(x);
    max.add(y);
    max.remove(x);
    ASSERT_VALUE_EQ(max.getValue(), y);
}

TEST_F(WindowFunctionMinMaxTest, TracksMemoryUsageOnAddAndRemove) {
    size_t trackingSize = sizeof(WindowFunctionMin);
    ASSERT_EQ(min.getApproximateSize(), trackingSize);

    auto largeStr = Value{"this is quite a long string"sv};
    min.add(largeStr);
    trackingSize += largeStr.getApproximateSize();
    ASSERT_EQ(min.getApproximateSize(), trackingSize);

    min.add(largeStr);
    trackingSize += largeStr.getApproximateSize();
    ASSERT_EQ(min.getApproximateSize(), trackingSize);

    min.remove(largeStr);
    trackingSize -= largeStr.getApproximateSize();
    ASSERT_EQ(min.getApproximateSize(), trackingSize);
}

}  // namespace
}  // namespace mongo
