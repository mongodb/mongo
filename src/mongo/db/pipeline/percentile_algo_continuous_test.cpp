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
#include "mongo/db/pipeline/percentile_algo_continuous.h"
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
    LOGV2(7514415, "generateNormal", "seed"_attr = seed);
    std::mt19937 generator(seed);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 1.0 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);
    for (size_t i = 0; i < n; i++) {
        inputs.push_back(dist(generator));
    }

    return inputs;
}

double computeTestPercentile(double p, vector<double> inputs, int n) {
    double rank = p * (n - 1);
    int rank_ceil = ceil(rank);
    int rank_floor = floor(rank);

    if (rank_ceil == rank && rank == rank_floor) {
        return inputs[rank];
    } else {
        return (rank_ceil - rank) * inputs[rank_floor] + (rank - rank_floor) * inputs[rank_ceil];
    }
}

/**
 * Basics.
 */
TEST(ContinuousPercentileTest, NoInputs) {
    ContinuousPercentile cp;
    ASSERT(!cp.computePercentile(0.1));
    ASSERT(cp.computePercentiles({0.1, 0.5}).empty());
}

TEST(ContinuousPercentileTest, TinyInput1) {
    vector<double> inputs = {1.0};

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_EQ(1.0, *cp.computePercentile(0));
    ASSERT_EQ(1.0, *cp.computePercentile(0.5));
    ASSERT_EQ(1.0, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, TinyInput2) {
    vector<double> inputs = {1.0, 2.0};

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_EQ(1.0, *cp.computePercentile(0));
    ASSERT_EQ(1.5, *cp.computePercentile(0.5));
    ASSERT_EQ(2.0, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, TinyInput3) {
    vector<double> inputs = {1.0, 2.0, 3.0};

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_EQ(1.0, *cp.computePercentile(0));
    ASSERT_EQ(2.0, *cp.computePercentile(0.5));
    ASSERT_EQ(3.0, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, Basic) {
    vector<double> inputs(100);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 100}

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_EQ(1.0, *cp.computePercentile(0));
    ASSERT_EQ(1.99, *cp.computePercentile(0.01));
    ASSERT_EQ(10.9, *cp.computePercentile(0.1));
    ASSERT_EQ(50.5, *cp.computePercentile(0.5));
    ASSERT_EQ(95.05, *cp.computePercentile(0.95));
    ASSERT_EQ(99.01, *cp.computePercentile(0.99));
    ASSERT_EQ(100.0, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, ComputeMultiplePercentilesAtOnce) {
    const vector<double> inputs = generateNormal(100);

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    const vector<double> ps = cp.computePercentiles({0.5, 0.9, 0.1});
    ASSERT_EQ(*cp.computePercentile(0.5), ps[0]);
    ASSERT_EQ(*cp.computePercentile(0.9), ps[1]);
    ASSERT_EQ(*cp.computePercentile(0.1), ps[2]);
}

/**
 * Tests with some special "doubles".
 */
TEST(ContinuousPercentileTest, Incorporate_OnlyInfinities) {
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
    LOGV2(7514413, "Incorporate_OnlyInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    ContinuousPercentile cp;

    for (double val : inputs) {
        cp.incorporate(val);
    }

    // 70 out of 100 values are negative infinities
    ASSERT_EQ(-inf, *cp.computePercentile(0));
    ASSERT_EQ(-inf, *cp.computePercentile(0.001));
    ASSERT_EQ(-inf, *cp.computePercentile(0.1));
    ASSERT_EQ(-inf, *cp.computePercentile(0.7));

    // 30 out of 100 values are positive infinities
    ASSERT_EQ(inf, *cp.computePercentile(0.71));
    ASSERT_EQ(inf, *cp.computePercentile(0.9));
    ASSERT_EQ(inf, *cp.computePercentile(0.999));
    ASSERT_EQ(inf, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, Incorporate_WithInfinities) {
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

    vector<double> sorted = inputs;  // sorted by construction
    auto seed = time(nullptr);
    LOGV2(7514414, "Incorporate_WithInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    ContinuousPercentile cp;

    for (double val : inputs) {
        cp.incorporate(val);
    }

    // 300 out of 1500 values are negative infinities
    ASSERT_EQ(-inf, *cp.computePercentile(0));
    ASSERT_EQ(-inf, *cp.computePercentile(0.001));
    ASSERT_EQ(-inf, *cp.computePercentile(0.1));
    const double pInfEnd = 300.0 / 1500;
    ASSERT_EQ(-inf, *cp.computePercentile(pInfEnd)) << "p:" << pInfEnd;

    const double pFirstNonInf = 301.0 / 1500;
    ASSERT_NE(-inf, *cp.computePercentile(pFirstNonInf)) << "p:" << pFirstNonInf;

    // non-inf values
    ASSERT_EQ(450.5, *cp.computePercentile(0.5));
    ASSERT_EQ(900.2, *cp.computePercentile(0.8));

    // interpolation between inf/-inf and real number should return inf/-inf
    ASSERT_EQ(inf, *cp.computePercentile(0.867));
    ASSERT_EQ(-inf, *cp.computePercentile(0.2));

    // 200 out of 1500 values are positive infinities
    const double pInfStart = 1 - 199.0 / 1500;
    ASSERT_EQ(inf, *cp.computePercentile(pInfStart)) << "p:" << pInfStart;
    ASSERT_EQ(inf, *cp.computePercentile(0.9));
    ASSERT_EQ(inf, *cp.computePercentile(0.999));
    ASSERT_EQ(inf, *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, Incorporate_Nan_ShouldSkip) {
    vector<double> inputs(1000);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 1000}
    const vector<double> sorted = inputs;          // sorted by construction

    // Add NaN value into the dataset.
    inputs.insert(inputs.begin() + 500, std::numeric_limits<double>::quiet_NaN());

    ContinuousPercentile cp;
    cp.incorporate(std::numeric_limits<double>::quiet_NaN());
    cp.incorporate(inputs);

    ASSERT_EQ(100.9, *cp.computePercentile(0.1));
    ASSERT_EQ(500.5, *cp.computePercentile(0.5));
    ASSERT_EQ(900.1, *cp.computePercentile(0.9));
}

/**
 * Tests with data from normal distribution and some corner cases.
 */
TEST(ContinuousPercentileTest, SmallDataset) {
    const int n = 10;
    vector<double> inputs = generateNormal(n);

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_LT(*cp.computePercentile(0.49), *cp.computePercentile(0.5));
    ASSERT_LT(*cp.computePercentile(0.4), *cp.computePercentile(0.5));

    std::sort(inputs.begin(), inputs.end());
    ASSERT_EQ(inputs.front(), *cp.computePercentile(0));
    ASSERT_EQ(computeTestPercentile(0.1, inputs, n), *cp.computePercentile(0.1));
    ASSERT_EQ(computeTestPercentile(0.5, inputs, n), *cp.computePercentile(0.5));
    ASSERT_EQ(computeTestPercentile(0.9, inputs, n), *cp.computePercentile(0.9));
    ASSERT_EQ(inputs.back(), *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, LargerDataset) {
    const int n = 1000;
    vector<double> inputs = generateNormal(n);

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_LT(*cp.computePercentile(0.499), *cp.computePercentile(0.4999));
    ASSERT_LT(*cp.computePercentile(0.4999), *cp.computePercentile(0.5));
    ASSERT_LT(*cp.computePercentile(0.5), *cp.computePercentile(0.5001));

    std::sort(inputs.begin(), inputs.end());
    ASSERT_EQ(inputs.front(), *cp.computePercentile(0));
    ASSERT_EQ(computeTestPercentile(0.005, inputs, n), *cp.computePercentile(0.005));
    ASSERT_EQ(computeTestPercentile(0.5, inputs, n), *cp.computePercentile(0.5));
    ASSERT_EQ(computeTestPercentile(0.995, inputs, n), *cp.computePercentile(0.995));
    ASSERT_EQ(inputs.back(), *cp.computePercentile(1));
}

TEST(ContinuousPercentileTest, Presorted) {
    const int n = 100;
    vector<double> inputs = generateNormal(n);
    std::sort(inputs.begin(), inputs.end());

    ContinuousPercentile cp;
    cp.incorporate(inputs);

    ASSERT_EQ(computeTestPercentile(0.1, inputs, n), *cp.computePercentile(0.1));
    ASSERT_EQ(computeTestPercentile(0.5, inputs, n), *cp.computePercentile(0.5));
    ASSERT_EQ(computeTestPercentile(0.99, inputs, n), *cp.computePercentile(0.99));
}

}  // namespace
}  // namespace mongo
