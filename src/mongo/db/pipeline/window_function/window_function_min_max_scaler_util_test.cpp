/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/window_function_min_max_scaler_util.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class WindowFunctionMinMaxScalerUtilTest : public AggregationContextFixture {
public:
    WindowFunctionMinMaxScalerUtilTest() : expCtx(getExpCtx()) {
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        expCtx->setCollator(std::move(collator));
    }
    // Asserts that, with all the needed inputs to min_max_scaler::computeResult(),
    // the function returns the expected value.
    void assertComputeResultExpected(Value expectedResult,
                                     Value currentValue,
                                     Value windowMin,
                                     Value windowMax,
                                     Value sMin = Value(0),
                                     Value sMax = Value(1)) {
        ASSERT_VALUE_EQ(
            min_max_scaler::computeResult(currentValue,
                                          min_max_scaler::MinAndMax(windowMin, windowMax),
                                          min_max_scaler::MinAndMax(sMin, sMax)),
            expectedResult);
    }

    int getRandomBetweenBounds(int lower, int upper) {
        auto& prng = expCtx->getOperationContext()->getClient()->getPrng();
        return lower + int(double(upper - lower) * (prng.nextCanonicalDouble()));
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;
};

// --------------------------- min_max_scaler::computeResult() tests -------------------------------

TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultAllZerosTest) {
    assertComputeResultExpected(Value(0),  // expectedResult
                                Value(0),  // currentValue
                                Value(0),  // windowMin
                                Value(0),  // windowMax
                                Value(0),  // sMin
                                Value(0)   // sMax
    );
}

// Tests that if windowMin and windowMax are equal, sMin is always returned.
TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultWindowMinAndMaxEqualTest) {
    // Testing with sMin and sMax defaults (0 and 1, respectively).
    int windowMin = getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(0),          // expectedResult
                                Value(windowMin),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMin)   // windowMax
    );

    // Test same case, with sMin and sMax scaled.
    int sMin = getRandomBetweenBounds(1, 1000);
    int sMax = sMin + getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(sMin),       // expectedResult
                                Value(windowMin),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMin),  // windowMax
                                Value(sMin),       // sMin
                                Value(sMax)        // sMax
    );
}

// Tests that if current value is windowMin, sMin is always returned
TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultResultIsWindowMinTest) {
    // Testing with sMin and sMax defaults (0 and 1, respectively).
    int windowMin = getRandomBetweenBounds(1, 1000);
    int windowMax = windowMin + getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(0),          // expectedResult
                                Value(windowMin),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMax)   // windowMax
    );

    // Test same case, with sMin and sMax scaled.
    int sMin = getRandomBetweenBounds(1, 1000);
    int sMax = sMin + getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(sMin),       // expectedResult
                                Value(windowMin),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMax),  // windowMax
                                Value(sMin),       // sMin
                                Value(sMax)        // sMax
    );
}

// Tests that if the current value is the windowMax, sMax is always returned
TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultCurrentIsWindowMaxTest) {
    // Testing with sMin and sMax defaults (0 and 1, respectively).
    int windowMin = getRandomBetweenBounds(1, 1000);
    int windowMax = windowMin + getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(1),          // expectedResult
                                Value(windowMax),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMax)   // windowMax
    );

    // Test same case, with sMin and sMax scaled.
    int sMin = getRandomBetweenBounds(1, 1000);
    int sMax = sMin + getRandomBetweenBounds(1, 1000);
    assertComputeResultExpected(Value(sMax),       // expectedResult
                                Value(windowMax),  // currentValue
                                Value(windowMin),  // windowMin
                                Value(windowMax),  // windowMax
                                Value(sMin),       // sMin
                                Value(sMax)        // sMax
    );
}

// Tests that if the current value is between the windowMin and windowMax,
// the proper value value between sMin and sMax is returned.
TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultWindowIntermediateTest) {
    // Testing with sMin and sMax defaults (0 and 1, respectively).
    int currentValue = 35;
    int windowMin = 10;
    int windowMax = 110;
    {
        // $minMaxScaler = (xi - min(x)) / (max(x) - min(x)) = (35 - 10) / (110 - 10) = 25 / 100
        // = 0.25
        double expectedValue = 0.25;
        assertComputeResultExpected(Value(expectedValue),  // expectedResult
                                    Value(currentValue),   // currentValue
                                    Value(windowMin),      // windowMin
                                    Value(windowMax)       // windowMax
        );
    }

    // Test same case, with sMin and sMax scaled.
    int sMin = 50;
    int sMax = 150;
    {
        // Scaled expected value = ((unscaled value) * (sMax - sMin)) + sMin
        // = (0.25 * (150 - 50)) + 50 = (0.25 * 100) + 50 = 75
        int expectedValue = 75;
        assertComputeResultExpected(Value(expectedValue),  // expectedResult
                                    Value(currentValue),   // currentValue
                                    Value(windowMin),      // windowMin
                                    Value(windowMax),      // windowMax
                                    Value(sMin),           // sMin
                                    Value(sMax)            // sMax
        );
    }
}

// Tests that if the current value is between the windowMin and windowMax,
// the proper value value between sMin and sMax is returned.
// Instead of ints, doubles are used, to confirm we can handle both numeric types.
TEST_F(WindowFunctionMinMaxScalerUtilTest, ComputeResultWindowIntermediateWithDoublesTest) {
    // Testing with sMin and sMax defaults (0 and 1, respectively).
    double currentValue = 3.5;
    double windowMin = 1.0;
    double windowMax = 11.0;
    {
        // $minMaxScaler = (xi - min(x)) / (max(x) - min(x)) = (35 - 10) / (110 - 10) = 25 / 100
        // = 0.25
        double expectedValue = 0.25;
        assertComputeResultExpected(Value(expectedValue),  // expectedResult
                                    Value(currentValue),   // currentValue
                                    Value(windowMin),      // windowMin
                                    Value(windowMax)       // windowMax
        );
    }

    // Test same case, with sMin and sMax scaled.
    double sMin = 5.0;
    double sMax = 15.0;
    {
        // Scaled expected value
        // = ((unscaled value) * (sMax - sMin)) + sMin
        // = (0.25 * (15.0 - 5.0)) + 5.0
        // = (0.25 * 10.0) + 5.0 = 7.5
        double expectedValue = 7.5;
        assertComputeResultExpected(Value(expectedValue),  // expectedResult
                                    Value(currentValue),   // currentValue
                                    Value(windowMin),      // windowMin
                                    Value(windowMax),      // windowMax
                                    Value(sMin),           // sMin
                                    Value(sMax)            // sMax
        );
    }
}

// Tests that if the current value is less than the windowMin,
// min_max_scaler::computeResult() tasserts.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest,
             ComputeResultCurrentValueLTWindowMinThrowsTest,
             "") {
    // currentValue < windowMin < windowMax
    int currentValue = getRandomBetweenBounds(1, 1000);
    int windowMin = currentValue + getRandomBetweenBounds(1, 1000);
    int windowMax = windowMin + getRandomBetweenBounds(1, 1000);

    // Hits tassert 9459903.
    min_max_scaler::computeResult(Value(currentValue),
                                  min_max_scaler::MinAndMax(Value(windowMin), Value(windowMax)),
                                  min_max_scaler::MinAndMax(Value(0), Value(1)));
}

// Tests that if the current value is greater than the windowMax,
// min_max_scaler::computeResult() tasserts.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest,
             ComputeResultCurrentValueGTWindowMaxThrowsTest,
             "") {
    // windowMin < windowMax < currentValue
    int windowMin = getRandomBetweenBounds(1, 1000);
    int windowMax = windowMin + getRandomBetweenBounds(1, 1000);
    int currentValue = windowMax + getRandomBetweenBounds(1, 1000);

    // Hits tassert 9459903.
    min_max_scaler::computeResult(Value(currentValue),
                                  min_max_scaler::MinAndMax(Value(windowMin), Value(windowMax)),
                                  min_max_scaler::MinAndMax(Value(0), Value(1)));
}

// ------------------------------ min_max_scaler::MinAndMax tests ----------------------------------

// Tests that after min and max are set without update, they can be read back with same values.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxFetchTest) {
    int min = getRandomBetweenBounds(1, 1000);
    int max = min + getRandomBetweenBounds(0, 1000);

    const auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(min));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(max));
}

// Tests that min and max are updated to first provided value, after empty construction.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxUpdateAfterEmptyConstructionTest) {
    Value update = Value(getRandomBetweenBounds(1, 1000));

    auto minAndMax = min_max_scaler::MinAndMax();
    minAndMax.update(update);

    ASSERT_VALUE_EQ(minAndMax.min(), Value(update));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(update));
}

// Tests that min can be updated and read back.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxUpdateMinTest) {
    // newMin < initialMin < max
    int newMin = getRandomBetweenBounds(1, 1000);
    int initialMin = newMin + getRandomBetweenBounds(1, 1000);
    int max = initialMin + getRandomBetweenBounds(1, 1000);

    auto minAndMax = min_max_scaler::MinAndMax(Value(initialMin), Value(max));
    minAndMax.update(Value(newMin));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(newMin));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(max));
}

// Tests that max can be updated and read back.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxUpdateMaxTest) {
    // min < initialMax < newMax
    int min = getRandomBetweenBounds(1, 1000);
    int initialMax = min + getRandomBetweenBounds(1, 1000);
    int newMax = initialMax + getRandomBetweenBounds(1, 1000);

    auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(initialMax));
    minAndMax.update(Value(newMax));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(min));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(newMax));
}

// Tests that empty MinAndMax can be created, an initial value can be provided,
// then a new min and max can be updated.
// Mixing behavior of previous tests together.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxUpdateInitalAndMinAndMaxTest) {
    // min < initial < max
    int min = getRandomBetweenBounds(1, 1000);
    int initial = min + getRandomBetweenBounds(1, 1000);
    int max = initial + getRandomBetweenBounds(1, 1000);

    auto minAndMax = min_max_scaler::MinAndMax();

    // Provide initial value.
    minAndMax.update(Value(initial));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(initial));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(initial));

    // Update min and max.
    minAndMax.update(Value(min));
    minAndMax.update(Value(max));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(min));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(max));
}

// Tests that MinAndMax can be initialized with a min and max, then reset,
// then updated and read back.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxResetAndUpdateTest) {
    // min < initial < max
    int min = getRandomBetweenBounds(1, 1000);
    int initial = min + getRandomBetweenBounds(1, 1000);
    int max = initial + getRandomBetweenBounds(1, 1000);

    auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(min));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(max));

    // Reset, update, and read back.
    minAndMax.reset();
    minAndMax.update(Value(initial));

    ASSERT_VALUE_EQ(minAndMax.min(), Value(initial));
    ASSERT_VALUE_EQ(minAndMax.max(), Value(initial));
}

// Tests that MinAndMax can be initialized with min and max as the same value.
TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxEqualTest) {
    // min == max
    int min = getRandomBetweenBounds(0, 1000);
    int max = min;

    const auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));

    ASSERT_VALUE_EQ(minAndMax.min(), minAndMax.max());
}

// Test that trying to construct a MinAndMax with a min greater than max throws.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxConstructMinGTMaxThrowsTest, "") {
    // max < min
    int max = getRandomBetweenBounds(1, 1000);
    int min = max + getRandomBetweenBounds(1, 1000);

    // Hits tassert 9522900.
    const auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));
}

// Test that trying to fetch the min value from an empty MinAndMax throws.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxEmptyConstructionFetchMinThrowsTest, "") {
    const auto minAndMax = min_max_scaler::MinAndMax();

    // Hits tassert 9522901.
    minAndMax.min();
}

// Test that trying to fetch the max value from an empty MinAndMax throws.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxEmptyConstructionFetchMaxThrowsTest, "") {
    const auto minAndMax = min_max_scaler::MinAndMax();

    // Hits tassert 9522902.
    minAndMax.max();
}

// Test that trying to fetch the min value from MinAndMax constructed with inital values.
// throws after reset.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxFetchMinAfterResetThrowsTest, "") {
    int min = getRandomBetweenBounds(1, 1000);
    int max = min + getRandomBetweenBounds(0, 1000);

    auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));
    minAndMax.reset();

    // Hits tassert 9522901.
    minAndMax.min();
}

// Test that trying to fetch the max value from MinAndMax constructed with inital values.
// throws after reset.
DEATH_TEST_F(WindowFunctionMinMaxScalerUtilTest, MinAndMaxFetchMaxAfterResetThrowsTest, "") {
    int min = getRandomBetweenBounds(1, 1000);
    int max = min + getRandomBetweenBounds(0, 1000);

    auto minAndMax = min_max_scaler::MinAndMax(Value(min), Value(max));
    minAndMax.reset();

    // Hits tassert 9522902.
    minAndMax.max();
}

};  // namespace
};  // namespace mongo
