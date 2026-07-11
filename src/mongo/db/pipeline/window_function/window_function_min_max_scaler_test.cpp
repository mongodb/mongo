// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
class WindowFunctionMinMaxScalerTest : public AggregationContextFixture {
public:
    WindowFunctionMinMaxScalerTest()
        : expCtx(getExpCtx()),
          minMaxScaler(std::make_unique<WindowFunctionMinMaxScaler>(expCtx.get())) {
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        expCtx->setCollator(std::move(collator));
    }

    void createWindowFunctionMinMaxScalerWithSMinAndSMax(std::pair<Value, Value> sMinAndsMax) {
        minMaxScaler = std::make_unique<WindowFunctionMinMaxScaler>(expCtx.get(), sMinAndsMax);
    }

    // Test case runner that adds values into the window then asserts the value is as expected.
    // The first Value in the provided vector will always be assumed to be the Value of the
    // "current" document being processed.
    void runTestCase(std::vector<Value> values, Value expected) {
        if (values.empty()) {
            return;
        }

        for (const auto& v : values) {
            minMaxScaler->add(v);
        }

        ASSERT_VALUE_EQ(minMaxScaler->getValue(*values.begin()), expected);
    }

    int getRandomBetweenBounds(int lower, int upper) {
        auto& prng = expCtx->getOperationContext()->getClient()->getPrng();
        return lower + int(double(upper - lower) * (prng.nextCanonicalDouble()));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
    std::unique_ptr<WindowFunctionMinMaxScaler> minMaxScaler;
};

TEST_F(WindowFunctionMinMaxScalerTest, SingleValueAddedIntoWindow) {
    // If a single value is added into the window, $minMaxScaler should always return 0.
    // This is because the window has no range between the min and the max.

    Value randomValue = Value(getRandomBetweenBounds(1, 1000));
    minMaxScaler->add(randomValue);
    ASSERT_VALUE_EQ(minMaxScaler->getValue(randomValue), Value(0));

    // Adding in more of the same value should also cause the computed value to remain at 0,
    // because the min and the max will remain 0.
    int nTimes = getRandomBetweenBounds(2, 25);
    for (int i = 0; i < nTimes; i++) {
        minMaxScaler->add(randomValue);
    }
    ASSERT_VALUE_EQ(minMaxScaler->getValue(randomValue), Value(0));

    // Change the sMin and sMax bounds also should not affect the return value of the min of the
    // domain.
    int sMin = getRandomBetweenBounds(1, 100);
    int sMax = sMin + getRandomBetweenBounds(1, 100);
    createWindowFunctionMinMaxScalerWithSMinAndSMax({Value(sMin), Value(sMax)});
    for (int i = 0; i < nTimes; i++) {
        minMaxScaler->add(randomValue);
    }
    ASSERT_VALUE_EQ(minMaxScaler->getValue(randomValue), Value(sMin));
}

TEST_F(WindowFunctionMinMaxScalerTest, GetValueOnWindowMin) {
    // Check that the if the current value is the min of the window, the returned value is zero.
    int windowMin = getRandomBetweenBounds(1, 100);
    std::vector<Value> values = {
        Value(windowMin), Value(windowMin + 20), Value(windowMin + 30), Value(windowMin + 40)};
    runTestCase(values, Value(0));

    // Scaling the domain by sMin and sMax should always produce the sMin.
    int sMin = getRandomBetweenBounds(1, 100);
    int sMax = sMin + getRandomBetweenBounds(1, 100);
    createWindowFunctionMinMaxScalerWithSMinAndSMax({Value(sMin), Value(sMax)});
    runTestCase(values, Value(sMin));
}

TEST_F(WindowFunctionMinMaxScalerTest, GetValueOnWindowMax) {
    // Check that the if the current value is the max of the window, the returned value is one.
    int windowMin = getRandomBetweenBounds(1, 100);
    std::vector<Value> values = {
        Value(windowMin + 40), Value(windowMin + 30), Value(windowMin + 20), Value(windowMin + 10)};
    runTestCase(values, Value(1));

    // Scaling the domain by sMin and sMax should always produce the sMin.
    int sMin = getRandomBetweenBounds(1, 100);
    int sMax = sMin + getRandomBetweenBounds(1, 100);
    createWindowFunctionMinMaxScalerWithSMinAndSMax({Value(sMin), Value(sMax)});
    runTestCase(values, Value(sMax));
}

TEST_F(WindowFunctionMinMaxScalerTest, GetValueOnWindowIntermediate) {
    // Get the value when current value is in-between the min and max.
    std::vector<Value> values = {Value(35), Value(10) /*min*/, Value(110) /*max*/, Value(23)};
    // $minMaxScaler = (xi - min(x)) / (max(x) - min(x)) = (35 - 10) / (110 - 10) = 25 / 100 = 0.25
    runTestCase(values, Value(0.25));

    // Check that the value works scaled between sMin and sMax
    int sMin = 50;
    int sMax = 150;
    // Scaled value = ((unscaled value) * (sMax - sMin)) + sMin = (0.25 * (150 - 50)) + 50
    // = (0.25 * 100) + 50 = 75
    createWindowFunctionMinMaxScalerWithSMinAndSMax({Value(sMin), Value(sMax)});
    runTestCase(values, Value(75));
}

TEST_F(WindowFunctionMinMaxScalerTest, GetValueOnWindowIntermediateWithDoubles) {
    // Get the value when current value is in-between the min and max.
    std::vector<Value> values = {Value(3.5), Value(1.0) /*min*/, Value(11.0) /*max*/, Value(2.3)};
    // $minMaxScaler = (xi - min(x)) / (max(x) - min(x)) = (3.5 - 1.0) / (11.0 - 1.0) = 2.5 / 10.0 =
    // 0.25
    runTestCase(values, Value(0.25));

    // Check that the value works scaled between sMin and sMax
    int sMin = 5.0;
    int sMax = 15.0;
    // Scaled value = ((unscaled value) * (sMax - sMin)) + sMin = (0.25 * (15.0 - 5.0)) + 5.0
    // = (0.25 * 10.0) + 5.0 = 7.5
    createWindowFunctionMinMaxScalerWithSMinAndSMax({Value(sMin), Value(sMax)});
    runTestCase(values, Value(7.5));
}

};  // namespace
};  // namespace mongo
