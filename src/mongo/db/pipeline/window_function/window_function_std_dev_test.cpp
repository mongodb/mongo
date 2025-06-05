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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/window_function/window_function_stddev.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace mongo {
namespace {

class WindowFunctionStdDevTest : public unittest::Test {
public:
    WindowFunctionStdDevTest()
        : pop(WindowFunctionStdDevPop(nullptr)), samp(WindowFunctionStdDevSamp(nullptr)) {}

    WindowFunctionStdDevPop pop;
    WindowFunctionStdDevSamp samp;

    // Two-pass algorithm
    double stdDevPop(std::vector<double>::const_iterator begin,
                     std::vector<double>::const_iterator end) {
        double mean = std::accumulate(begin, end, 0.0) / (end - begin);
        double squaredDiffs = std::accumulate(begin, end, 0.0, [&](double acc, double val) {
            return acc + (val - mean) * (val - mean);
        });
        return sqrt(squaredDiffs / (end - begin));
    }
};

TEST_F(WindowFunctionStdDevTest, EmptyWindow) {
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});
}

TEST_F(WindowFunctionStdDevTest, SingletonWindow) {
    pop.add(Value{1});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), 0, 1e-15);
    samp.add(Value{1});
    ASSERT_VALUE_EQ(samp.getValue(), Value{BSONNULL});
}

TEST_F(WindowFunctionStdDevTest, ReturnsDouble) {
    pop.add(Value{1});
    pop.add(Value{2});
    pop.add(Value{3});
    ASSERT_EQ(pop.getValue().getType(), BSONType::numberDouble);

    samp.add(Value{1});
    samp.add(Value{2});
    samp.add(Value{3});
    // Returns 1.0
    ASSERT_EQ(samp.getValue().getType(), BSONType::numberDouble);

    pop.add(Value{Decimal128("100000000000000000000000000000")});
    ASSERT_EQ(pop.getValue().getType(), BSONType::numberDouble);
}


TEST_F(WindowFunctionStdDevTest, Add) {
    pop.add(Value{1});
    pop.add(Value{2});
    pop.add(Value{3});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), sqrt(2 / 3.0), 1e-15);

    samp.add(Value{1});
    samp.add(Value{2});
    samp.add(Value{3});
    ASSERT_APPROX_EQUAL(samp.getValue().getDouble(), 1.0, 1e-15);
}

TEST_F(WindowFunctionStdDevTest, Remove1) {
    pop.add(Value{1});
    pop.add(Value{2});
    pop.add(Value{3});
    // Add, then remove
    pop.add(Value{4});
    pop.remove(Value{1});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), sqrt(2 / 3.0), 1e-15);
}

TEST_F(WindowFunctionStdDevTest, Remove2) {
    pop.add(Value{1});
    pop.add(Value{2});
    pop.add(Value{3});
    // Remove, then add
    pop.remove(Value{1});
    pop.add(Value{4});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), sqrt(2 / 3.0), 1e-15);
}

TEST_F(WindowFunctionStdDevTest, SampleRemove) {
    samp.add(Value{1});
    samp.add(Value{2});
    samp.add(Value{3});
    samp.remove(Value{1});
    ASSERT_APPROX_EQUAL(samp.getValue().getDouble(), sqrt(0.5), 1e-15);
}

TEST_F(WindowFunctionStdDevTest, NotDividingByZeroInM2Update) {
    pop.add(Value{1});
    pop.remove(Value{1});
    pop.add(Value{1});
    pop.add(Value{2});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), 0.5, 1e-15);

    double nan = std::numeric_limits<double>::quiet_NaN();
    samp.add(Value{nan});
    samp.remove(Value{nan});
    samp.add(Value{1});
    samp.add(Value{2});
    ASSERT_APPROX_EQUAL(samp.getValue().getDouble(), sqrt(0.5), 1e-15);
}

TEST_F(WindowFunctionStdDevTest, HandlesNonfinite) {
    double inf = std::numeric_limits<double>::infinity();
    double nan = std::numeric_limits<double>::quiet_NaN();

    pop.add(Value{1});
    pop.add(Value{2});
    pop.add(Value{inf});
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});  // 1, 2, inf
    pop.remove(Value{inf});
    ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), 0.5, 1e-15);  // 1, 2
    pop.add(Value{nan});
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});  // 1, 2, nan
    pop.remove(Value{nan});
    pop.add(Value{-inf});
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});  // 1, 2, -inf
    pop.add(Value{inf});
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});  // 1, 2, -inf, inf
    pop.add(Value{nan});
    ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});  // 1, 2, -inf, inf, nan
}

TEST_F(WindowFunctionStdDevTest, Stability) {
    const int collLength = 10000;
    const int windowSize = 100;
    PseudoRandom prng(0);
    std::vector<double> vec(collLength);
    for (int j = 0; j < collLength; j++) {
        vec[j] = prng.nextCanonicalDouble() - 0.5;
    }
    for (int i = 0; i < windowSize; i++) {
        pop.add(Value{vec[i]});
    }
    for (int i = windowSize; i < collLength; i++) {
        pop.add(Value{vec[i]});
        pop.remove(Value{vec[i - windowSize]});
        double trueStdDev = stdDevPop(vec.begin() + i - windowSize + 1, vec.begin() + i + 1);
        double calculatedStdDev = pop.getValue().getDouble();
        ASSERT_LTE(Decimal128(calculatedStdDev).subtract(Decimal128(trueStdDev)).toAbs(),
                   Decimal128("1e-15"));
        double relativeError = (calculatedStdDev - trueStdDev) / trueStdDev;
        ASSERT_LTE(relativeError, 1e-15);
    }
}

TEST_F(WindowFunctionStdDevTest, LargeNumberStability) {
    const int collLength = 10000;
    const int windowSize = 100;
    PseudoRandom prng(0);
    std::vector<double> vec(collLength);
    for (int j = 0; j < collLength; j++) {
        vec[j] = (prng.nextCanonicalDouble() - 0.5) * prng.nextInt64();
    }
    for (int i = 0; i < windowSize; i++) {
        pop.add(Value{vec[i]});
    }
    for (int i = windowSize; i < collLength; i++) {
        pop.add(Value{vec[i]});
        pop.remove(Value{vec[i - windowSize]});
        double trueStdDev = stdDevPop(vec.begin() + i - windowSize + 1, vec.begin() + i + 1);
        double calculatedStdDev = pop.getValue().getDouble();
        double relativeError = (calculatedStdDev - trueStdDev) / trueStdDev;
        ASSERT_LTE(relativeError, 1e-15);
    }
}

TEST_F(WindowFunctionStdDevTest, HandlesUnderflow) {
    double nan = std::numeric_limits<double>::quiet_NaN();
    const int collLength = 10000;
    const int windowSize = 100;
    PseudoRandom prng(0);
    std::vector<double> vec(collLength);
    for (int j = 0; j < collLength; j++) {
        vec[j] = prng.nextCanonicalDouble() - 0.5;
    }
    for (int i = 0; i < collLength / windowSize; i++) {
        // Fill up the window. Remove all but one element. The population std dev should now equal
        // exactly 0 since there is only one element, but due to floating point error, the _m2
        // quantity might be a small negative value. Taking the sqrt of this in the std dev formula
        // would then return NaN.
        for (int j = 0; j < windowSize; j++)
            pop.add(Value{vec[i * windowSize + j]});
        for (int k = 0; k < windowSize - 1; k++)
            pop.remove(Value{vec[i * windowSize + k]});
        // NaN and -NaN are treated as equal when wrapped in a Value.
        ASSERT_VALUE_NE(pop.getValue(), Value{nan});
        ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), 0, 1e-15);
        // Empty the window.
        pop.remove(Value{vec[i * windowSize + (windowSize - 1)]});
        ASSERT_VALUE_EQ(pop.getValue(), Value{BSONNULL});
    }
}

TEST_F(WindowFunctionStdDevTest, ConstantInput) {
    const int collLength = 1000;
    const int windowSize = 100;
    PseudoRandom prng(0);
    const double constant = prng.nextCanonicalDouble() - 0.5;
    for (int i = 0; i < windowSize; i++) {
        pop.add(Value{constant});
    }
    for (int i = windowSize; i < collLength; i++) {
        pop.add(Value{constant});
        pop.remove(Value{constant});
        ASSERT_APPROX_EQUAL(pop.getValue().getDouble(), 0, 1e-15);
    }
}

}  // namespace
}  // namespace mongo
