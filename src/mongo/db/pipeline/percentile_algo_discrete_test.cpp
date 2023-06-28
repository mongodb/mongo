/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/random/normal_distribution.hpp>
#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>
#include <ostream>
#include <random>

#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/percentile_algo_discrete.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
using std::vector;

vector<double> generateNormal(size_t n) {
    auto seed = time(nullptr);
    LOGV2(7514410, "{seed}", "generateNormal", "seed"_attr = seed);
    std::mt19937 generator(seed);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 1.0 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);
    for (size_t i = 0; i < n; i++) {
        inputs.push_back(dist(generator));
    }

    return inputs;
}

/**
 * Basics.
 */
TEST(DiscretePercentileTest, NoInputs) {
    DiscretePercentile dp;
    ASSERT(!dp.computePercentile(0.1));
    ASSERT(dp.computePercentiles({0.1, 0.5}).empty());
}

TEST(DiscretePercentileTest, TinyInput1) {
    vector<double> inputs = {1.0};

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(1.0, *dp.computePercentile(0));
    ASSERT_EQ(1.0, *dp.computePercentile(0.5));
    ASSERT_EQ(1.0, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, TinyInput2) {
    vector<double> inputs = {1.0, 2.0};

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(1.0, *dp.computePercentile(0));
    ASSERT_EQ(1.0, *dp.computePercentile(0.5));
    ASSERT_EQ(2.0, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, TinyInput3) {
    vector<double> inputs = {1.0, 2.0, 3.0};

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(1.0, *dp.computePercentile(0));
    ASSERT_EQ(2.0, *dp.computePercentile(0.5));
    ASSERT_EQ(3.0, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, Basic) {
    vector<double> inputs(100);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 100}

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(1.0, *dp.computePercentile(0));
    ASSERT_EQ(1.0, *dp.computePercentile(0.01));
    ASSERT_EQ(10.0, *dp.computePercentile(0.1));
    ASSERT_EQ(50.0, *dp.computePercentile(0.5));
    ASSERT_EQ(90.0, *dp.computePercentile(0.9));
    ASSERT_EQ(99.0, *dp.computePercentile(0.99));
    ASSERT_EQ(100.0, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, ComputeMultiplePercentilesAtOnce) {
    const vector<double> inputs = generateNormal(100);

    DiscretePercentile dp;
    dp.incorporate(inputs);

    const vector<double> ps = dp.computePercentiles({0.5, 0.9, 0.1});
    ASSERT_EQ(*dp.computePercentile(0.5), ps[0]);
    ASSERT_EQ(*dp.computePercentile(0.9), ps[1]);
    ASSERT_EQ(*dp.computePercentile(0.1), ps[2]);
}

/**
 * Tests with some special "doubles".
 */
TEST(DiscretePercentileTest, Incorporate_OnlyInfinities) {
    const double inf = std::numeric_limits<double>::infinity();

    // Setup the data as 70 negative infinities, and 30 positive infinities.
    vector<double> inputs(100);
    for (size_t i = 0; i < 70; ++i) {
        inputs[i] = -inf;
    }
    for (size_t i = 70; i < 100; ++i) {
        inputs[i] = inf;
    }
    auto seed = time(nullptr);
    LOGV2(7514412, "{seed}", "Incorporate_OnlyInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    DiscretePercentile dp;

    for (double val : inputs) {
        dp.incorporate(val);
    }

    // 70 out of 100 values are negative infinities
    ASSERT_EQ(-inf, *dp.computePercentile(0));
    ASSERT_EQ(-inf, *dp.computePercentile(0.001));
    ASSERT_EQ(-inf, *dp.computePercentile(0.1));
    ASSERT_EQ(-inf, *dp.computePercentile(0.7));

    // 30 out of 100 values are positive infinities
    ASSERT_EQ(inf, *dp.computePercentile(0.71));
    ASSERT_EQ(inf, *dp.computePercentile(0.9));
    ASSERT_EQ(inf, *dp.computePercentile(0.999));
    ASSERT_EQ(inf, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, Incorporate_WithInfinities) {
    const double inf = std::numeric_limits<double>::infinity();

    // Setup the data as 1000 evenly distributed "normal values", 200 positive infinities, and
    // 300 negative infinities.
    vector<double> inputs(1500);
    for (size_t i = 0; i < 300; ++i) {
        inputs[i] = -inf;
    }
    std::iota(inputs.begin() + 300, inputs.begin() + 1300, 1.0);  // {1, 2, ..., 1000}
    for (size_t i = 1300; i < 1500; ++i) {
        inputs[i] = inf;
    }
    const int n = inputs.size();
    vector<double> sorted = inputs;  // sorted by construction
    auto seed = time(nullptr);
    LOGV2(7514411, "{seed}", "Incorporate_WithInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    DiscretePercentile dp;

    for (double val : inputs) {
        dp.incorporate(val);
    }

    // 300 out of 1500 values are negative infinities
    ASSERT_EQ(-inf, *dp.computePercentile(0));
    ASSERT_EQ(-inf, *dp.computePercentile(0.001));
    ASSERT_EQ(-inf, *dp.computePercentile(0.1));
    const double pInfEnd = 300.0 / 1500;
    ASSERT_EQ(-inf, *dp.computePercentile(pInfEnd)) << "p:" << pInfEnd;

    const double pFirstNonInf = 301.0 / 1500;
    ASSERT_NE(-inf, *dp.computePercentile(pFirstNonInf)) << "p:" << pFirstNonInf;

    ASSERT_EQ(sorted[PercentileAlgorithm::computeTrueRank(n, 0.5)], *dp.computePercentile(0.5));

    // 200 out of 1500 values are positive infinities
    const double pInfStart = 1 - 199.0 / 1500;
    ASSERT_EQ(inf, *dp.computePercentile(pInfStart)) << "p:" << pInfStart;
    ASSERT_EQ(inf, *dp.computePercentile(0.9));
    ASSERT_EQ(inf, *dp.computePercentile(0.999));
    ASSERT_EQ(inf, *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, Incorporate_Nan_ShouldSkip) {
    vector<double> inputs(1000);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 1000}
    const vector<double> sorted = inputs;          // sorted by construction
    const int n = sorted.size();

    // Add NaN value into the dataset.
    inputs.insert(inputs.begin() + 500, std::numeric_limits<double>::quiet_NaN());

    DiscretePercentile dp;
    dp.incorporate(std::numeric_limits<double>::quiet_NaN());
    dp.incorporate(inputs);

    ASSERT_EQ(sorted[PercentileAlgorithm::computeTrueRank(n, 0.1)], *dp.computePercentile(0.1));
    ASSERT_EQ(sorted[PercentileAlgorithm::computeTrueRank(n, 0.5)], *dp.computePercentile(0.5));
    ASSERT_EQ(sorted[PercentileAlgorithm::computeTrueRank(n, 0.9)], *dp.computePercentile(0.9));
}

/**
 * Tests with data from normal distribution and some corner cases.
 */
TEST(DiscretePercentileTest, SmallDataset) {
    const int n = 10;
    vector<double> inputs = generateNormal(n);

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(*dp.computePercentile(0.49), *dp.computePercentile(0.5));
    ASSERT_LT(*dp.computePercentile(0.4), *dp.computePercentile(0.5));

    std::sort(inputs.begin(), inputs.end());
    ASSERT_EQ(inputs.front(), *dp.computePercentile(0));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.1)], *dp.computePercentile(0.1));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.5)], *dp.computePercentile(0.5));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.9)], *dp.computePercentile(0.9));
    ASSERT_EQ(inputs.back(), *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, LargerDataset) {
    const int n = 1000;
    vector<double> inputs = generateNormal(n);

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_LT(*dp.computePercentile(0.499), *dp.computePercentile(0.4999));
    ASSERT_EQ(*dp.computePercentile(0.4999), *dp.computePercentile(0.5));
    ASSERT_LT(*dp.computePercentile(0.5), *dp.computePercentile(0.5001));

    std::sort(inputs.begin(), inputs.end());
    ASSERT_EQ(inputs.front(), *dp.computePercentile(0));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.005)], *dp.computePercentile(0.005));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.5)], *dp.computePercentile(0.5));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.995)], *dp.computePercentile(0.995));
    ASSERT_EQ(inputs.back(), *dp.computePercentile(1));
}

TEST(DiscretePercentileTest, Presorted) {
    const int n = 100;
    vector<double> inputs = generateNormal(n);
    std::sort(inputs.begin(), inputs.end());

    DiscretePercentile dp;
    dp.incorporate(inputs);

    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.1)], *dp.computePercentile(0.1));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.5)], *dp.computePercentile(0.5));
    ASSERT_EQ(inputs[PercentileAlgorithm::computeTrueRank(n, 0.99)], *dp.computePercentile(0.99));
}

}  // namespace
}  // namespace mongo
