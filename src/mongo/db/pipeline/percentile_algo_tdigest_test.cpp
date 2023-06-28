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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/random/exponential_distribution.hpp>
#include <boost/random/normal_distribution.hpp>
#include <boost/random/uniform_real_distribution.hpp>
#include <boost/random/weibull_distribution.hpp>
#include <cstdlib>
#include <ctime>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/db/pipeline/percentile_algo_tdigest.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

const double epsilon = 0.000'000'001;
using Centroid = TDigest::Centroid;
using std::vector;

void assertIsOrdered(const TDigest& digest, const std::string& tag) {
    const auto& centroids = digest.centroids();
    if (centroids.size() == 0) {
        return;
    }
    for (size_t i = 1; i < centroids.size(); ++i) {
        if (centroids[i - 1].mean > centroids[i].mean) {
            std::cout.precision(32);
            std::cout << "centroids out of order: " << centroids[i - 1].mean << ","
                      << centroids[i].mean << std::endl;
        }
        ASSERT_LTE(centroids[i - 1].mean, centroids[i].mean)
            << tag << " Centroids are not ordered at: " << i;
    }
}

void assertConformsToScaling(const TDigest& digest,
                             TDigest::ScalingFunction k,
                             int delta,
                             const std::string& tag) {
    const auto& centroids = digest.centroids();
    const int64_t n = digest.n();

    double w = 0;
    double q = 0;
    double kPrev = k(q, delta);
    for (const Centroid& c : centroids) {
        double qNext = (w + c.weight) / n;
        double kNext = k(qNext, delta);
        if (c.weight > 1) {
            ASSERT_LTE(kNext - kPrev, 1.0 + epsilon)
                << tag << ": Scaling violation at centroid with weight " << c.weight
                << ", prev weights add up to: " << w;
        }
        q = qNext;
        kPrev = kNext;
        w += c.weight;
    }
}

void assertIsValid(const TDigest& digest,
                   TDigest::ScalingFunction k,
                   int delta,
                   const std::string& tag) {
    assertIsOrdered(digest, tag);
    assertConformsToScaling(digest, k, delta, tag);
}

// The ranks computed by this function is 0-based.
int computeRank(const vector<double>& sorted, double value) {
    // std::lower_bound returns an iterator pointing to the first element in the range [first, last)
    // that does _not_ satisfy element < value.
    auto lower = std::lower_bound(sorted.begin(), sorted.end(), value);

    ASSERT(lower != sorted.end()) << value << " is larger than the max input " << sorted.back();
    return std::distance(sorted.begin(), lower);
}

// t-digest doesn't guarantee accuracy error bounds (there exists inputs for which the error can
// be arbitrary large), however, for already sorted inputs the error does have the upper bound
// and for most "normal" inputs it's not that far off. Note: the error is defined in terms of
// _rank_.
void assertExpectedAccuracy(const vector<double>& sorted,
                            TDigest& digest,
                            vector<double> percentiles,
                            double accuracyError,
                            const char* msg) {
    for (double p : percentiles) {
        const int trueRank = PercentileAlgorithm::computeTrueRank(sorted.size(), p);
        // If there are duplicates in the data, the true rank is a range of values so we need to
        // find its lower and upper bounds.
        int lowerTrueRank = trueRank;
        const double val = sorted[lowerTrueRank];
        while (lowerTrueRank > 0 && sorted[lowerTrueRank - 1] == val) {
            lowerTrueRank--;
        }
        int upperTrueRank = trueRank;
        while (upperTrueRank + 1 < static_cast<int>(sorted.size()) &&
               sorted[upperTrueRank + 1] == val) {
            upperTrueRank++;
        }

        const double computedPercentile = digest.computePercentile(p).value();

        ASSERT_GTE(computedPercentile, sorted.front())
            << msg << " computed percentile " << computedPercentile << " for " << p
            << " is smaller than the min value " << sorted.front();
        ASSERT_LTE(computedPercentile, sorted.back())
            << msg << " computed percentile " << computedPercentile << " for " << p
            << " is larger than the max value " << sorted.back();

        const int rank = computeRank(sorted, computedPercentile);

        ASSERT_LTE(lowerTrueRank - accuracyError * sorted.size(), rank)
            << msg << " computed percentile " << computedPercentile << " for " << p
            << " is not accurate. Its computed rank " << rank << " is less than lower true rank "
            << lowerTrueRank << " by " << lowerTrueRank - rank;

        ASSERT_LTE(rank, upperTrueRank + accuracyError * sorted.size())
            << msg << " computed percentile " << computedPercentile << " for " << p
            << " is not accurate. Its computed rank " << rank << " is greater than upper true rank "
            << upperTrueRank << " by " << rank - upperTrueRank;
    }
}

/*==================================================================================================
  Tests with fixed datasets.
==================================================================================================*/
/**
 * For the GetPercentile_* tests the scaling function and delta don't matter as the tests create
 * fully formed digests.
 */
TEST(TDigestTest, GetPercentile_Empty) {
    TDigest digest(TDigest::k1_limit, 100);
    ASSERT(!digest.computePercentile(0.2));
    ASSERT(digest.computePercentiles({0.2}).empty());
}

TEST(TDigestTest, GetPercentile_SingleCentroid_SinglePoint) {
    TDigest digest(0,     // negInfCount
                   0,     // posInfCount
                   42.0,  // min
                   42.0,  // max
                   {{1, 42.0}},
                   nullptr /* k1_limit */,
                   1 /* delta */);
    ASSERT_EQ(42.0, digest.computePercentile(0.1));
    ASSERT_EQ(42.0, digest.computePercentile(0.5));
    ASSERT_EQ(42.0, digest.computePercentile(0.9));
}

// Our t-digest computes accurate minimum.
TEST(TDigestTest, GetPercentile_SingleCentroid_Min) {
    TDigest digest(0,    // negInfCount
                   0,    // posInfCount
                   1.0,  // min
                   5.0,  // max
                   {{10, 3.7}},
                   nullptr /* k1_limit */,
                   1 /* delta */);
    ASSERT_EQ(1.0, digest.computePercentile(0));
}

// Our t-digest computes accurate maximum.
TEST(TDigestTest, GetPercentile_SingleCentroid_Max) {
    TDigest digest(0,    // negInfCount
                   0,    // posInfCount
                   1.0,  // min
                   5.0,  // max
                   {{10, 3.7}},
                   nullptr /* k1_limit */,
                   1 /* delta */);
    ASSERT_EQ(5.0, digest.computePercentile(1));
}

// On a single t-digest computes continuous percentiles.
TEST(TDigestTest, GetPercentile_SingleCentroid_EvenlyDistributed) {
    vector<double> inputs(100);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 100}

    TDigest digest(0,                           // negInfCount
                   0,                           // posInfCount
                   1.0,                         // min
                   100.0,                       // max
                   {{100, (1.0 + 100.0) / 2}},  // the single centroid
                   nullptr /* k1_limit */,
                   1 /* delta */);

    for (int i = 1; i < 10; ++i) {
        const double p = i / 10.0;
        const double res = digest.computePercentile(p).value();
        const double expected = inputs[i * 10 - 1] * p + inputs[i * 10] * (1 - p);
        ASSERT_APPROX_EQUAL(expected, res, epsilon) << p << ":" << expected << "," << res;
    }
}

// On tiny inputs like these t-digest will create single-point centroids and compute accurate
// results. The result should match the DiscretePercentile.
TEST(TDigestTest, TinyInput1) {
    vector<double> inputs = {1.0};

    TDigest d{TDigest::k0_limit, 100 /*delta*/};
    d.incorporate(inputs);

    ASSERT_EQ(1.0, *d.computePercentile(0));
    ASSERT_EQ(1.0, *d.computePercentile(0.5));
    ASSERT_EQ(1.0, *d.computePercentile(1));
}

TEST(TDigestTest, TinyInput2) {
    vector<double> inputs = {1.0, 2.0};

    TDigest d{TDigest::k0_limit, 100 /*delta*/};
    d.incorporate(inputs);

    ASSERT_EQ(1.0, *d.computePercentile(0));
    ASSERT_EQ(1.0, *d.computePercentile(0.5));
    ASSERT_EQ(2.0, *d.computePercentile(1));
}

TEST(TDigestTest, TinyInput3) {
    vector<double> inputs = {1.0, 2.0, 3.0};

    TDigest d{TDigest::k0_limit, 100 /*delta*/};
    d.incorporate(inputs);

    ASSERT_EQ(1.0, *d.computePercentile(0));
    ASSERT_EQ(2.0, *d.computePercentile(0.5));
    ASSERT_EQ(3.0, *d.computePercentile(1));
}

// Single-point centroids should yield accurate discrete percentiles, even if the rest of the data
// distribution isn't "even".
TEST(TDigestTest, GetPercentile_SinglePointCentroids) {
    TDigest digest(0,      // negInfCount
                   0,      // posInfCount
                   1,      // min
                   10000,  // max
                   {
                       {1, 1},  // {weight, mean}
                       {1, 2},
                       {1, 3},
                       {37, 10.0},
                       {44, 18.0},
                       {13, 80.0},
                       {1, 800.0},
                       {1, 900.0},
                       {1, 10000.0},
                   },  // 100 datapoints total
                   nullptr /* k1_limit */,
                   1 /* delta */);

    ASSERT_EQ(2.0, digest.computePercentile(0.02));
    ASSERT_EQ(800.0, digest.computePercentile(0.98));
}

/**
 * Tests with the special double inputs. The scaling function doesn't matter for these tests but
 * using a smaller delta to exercise compacting of centroids.
 */
TEST(TDigestTest, Incorporate_OnlyInfinities) {
    const int delta = 50;  // doesn't really matter in this test as no centroids are created
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
    LOGV2(7429515, "{seed}", "Incorporate_OnlyInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    TDigest d{TDigest::k0_limit, delta};
    for (double val : inputs) {
        d.incorporate(val);
    }
    d.flushBuffer();
    assertIsValid(d, TDigest::k0, delta, "Incorporate_OnlyInfinities");

    ASSERT_EQ(inputs.size(), d.n() + d.negInfCount() + d.posInfCount()) << "n of digest: " << d;
    ASSERT_EQ(-inf, d.min()) << "min of digest: " << d;
    ASSERT_EQ(inf, d.max()) << "max of digest: " << d;

    // 70 out of 100 values are negative infinities
    ASSERT_EQ(-inf, *d.computePercentile(0)) << " digest: " << d;
    ASSERT_EQ(-inf, *d.computePercentile(0.001)) << " digest: " << d;
    ASSERT_EQ(-inf, *d.computePercentile(0.1)) << " digest: " << d;
    ASSERT_EQ(-inf, *d.computePercentile(0.7)) << " digest: " << d;

    // 30 out of 100 values are positive infinities
    ASSERT_EQ(inf, *d.computePercentile(0.71)) << " digest: " << d;
    ASSERT_EQ(inf, *d.computePercentile(0.9)) << " digest: " << d;
    ASSERT_EQ(inf, *d.computePercentile(0.999)) << " digest: " << d;
    ASSERT_EQ(inf, *d.computePercentile(1)) << " digest: " << d;
}

TEST(TDigestTest, Incorporate_WithInfinities) {
    const int delta = 50;
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
    LOGV2(7429511, "{seed}", "Incorporate_WithInfinities", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    TDigest d{TDigest::k0_limit, delta};
    for (double val : inputs) {
        d.incorporate(val);
    }
    d.flushBuffer();
    assertIsValid(d, TDigest::k0, delta, "Incorporate_WithInfinities");

    ASSERT_EQ(inputs.size(), d.n() + d.negInfCount() + d.posInfCount()) << "n of digest: " << d;
    ASSERT_EQ(-inf, d.min()) << "min of digest: " << d;
    ASSERT_EQ(inf, d.max()) << "max of digest: " << d;

    // 300 out of 1500 values are negative infinities
    ASSERT_EQ(-inf, *d.computePercentile(0.001)) << " digest: " << d;
    ASSERT_EQ(-inf, *d.computePercentile(0.1)) << " digest: " << d;
    const double pInfEnd = 300.0 / 1500;
    ASSERT_EQ(-inf, *d.computePercentile(pInfEnd)) << "p:" << pInfEnd << " digest: " << d;

    const double pFirstNonInf = 301.0 / 1500;
    ASSERT_NE(-inf, *d.computePercentile(pFirstNonInf)) << "p:" << pFirstNonInf << " digest " << d;

    assertExpectedAccuracy(
        sorted, d, {0.5} /* percentiles */, 0.020 /* accuracyError */, "Incorporate_Infinities");

    // 200 out of 1500 values are positive infinities
    const double pInfStart = 1 - 199.0 / 1500;
    ASSERT_EQ(inf, *d.computePercentile(pInfStart)) << "p:" << pInfStart << " digest: " << d;
    ASSERT_EQ(inf, *d.computePercentile(0.9)) << " digest: " << d;
    ASSERT_EQ(inf, *d.computePercentile(0.999)) << " digest: " << d;
}

TEST(TDigestTest, Incorporate_Nan_ShouldSkip) {
    const int delta = 50;

    vector<double> inputs(1000);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 1000}

    TDigest oracle{TDigest::k0_limit, delta};
    oracle.incorporate(inputs);

    // Add NaN value into the dataset.
    inputs.insert(inputs.begin() + 500, std::numeric_limits<double>::quiet_NaN());

    TDigest d{TDigest::k0_limit, delta};
    d.incorporate(std::numeric_limits<double>::quiet_NaN());
    d.incorporate(inputs);
    d.flushBuffer();
    assertIsValid(d, TDigest::k0, delta, "Incorporate_Nan_ShouldSkip");

    ASSERT_EQ(oracle.n(), d.n()) << "n of digest: " << d;
    ASSERT_EQ(oracle.min(), d.min()) << "min of digest: " << d;
    ASSERT_EQ(oracle.max(), d.max()) << "max of digest: " << d;

    ASSERT_EQ(*oracle.computePercentile(0.1), *d.computePercentile(0.1)) << " digest: " << d;
    ASSERT_EQ(*oracle.computePercentile(0.5), *d.computePercentile(0.5)) << " digest: " << d;
    ASSERT_EQ(*oracle.computePercentile(0.9), *d.computePercentile(0.9)) << " digest: " << d;
}

TEST(TDigestTest, Incorporate_Great_And_Small) {
    const int delta = 50;

    // Create a dataset consisting of four groups of values (large/small refer to the abs value):
    vector<double> inputs(1000);
    // 1. large negative numbers
    inputs[0] = std::numeric_limits<double>::lowest();
    for (size_t i = 1; i < 250; ++i) {
        inputs[i] = std::nextafter(inputs[i - 1] /* from */, -1.0 /* to */);
    }
    // 2. small negative numbers
    inputs[250] = -0.0;
    for (size_t i = 251; i < 500; ++i) {
        inputs[i] = std::nextafter(inputs[i - 1] /* from */, -1.0 /* to */);
    }
    // 3. small positive numbers
    inputs[500] = 0.0;
    for (size_t i = 501; i < 750; ++i) {
        inputs[i] = std::nextafter(inputs[i - 1] /* from */, 1.0 /* to */);
    }
    // 4. large positive numbers
    inputs[750] = std::numeric_limits<double>::max();
    for (size_t i = 751; i < 1000; ++i) {
        inputs[i] = std::nextafter(inputs[i - 1] /* from */, 1.0 /* to */);
    }
    vector<double> sorted = inputs;
    std::sort(sorted.begin(), sorted.end());

    auto seed = time(nullptr);
    LOGV2(7429512, "{seed}", "Incorporate_Great_And_Small", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    TDigest d{TDigest::k0_limit, delta};
    d.incorporate(std::numeric_limits<double>::quiet_NaN());
    for (double val : inputs) {
        d.incorporate(val);
    }
    d.flushBuffer();
    assertIsValid(d, TDigest::k0, delta, "Incorporate_Great_And_Small");

    ASSERT_EQ(inputs.size(), d.n()) << "n of digest: " << d;
    ASSERT_EQ(sorted.front(), d.min()) << "min of digest: " << d;
    ASSERT_EQ(sorted.back(), d.max()) << "max of digest: " << d;

    assertExpectedAccuracy(sorted,
                           d,
                           {0.1, 0.4, 0.5, 0.6, 0.9} /* percentiles */,
                           0.020 /* accuracyError */,
                           "Incorporate_Great_And_Small");
}

/**
 * The following set of tests checks the workings of merging data into a digest. They use a simpler
 * k0 = delta*q/2 scaling function as it's easier to reason about the resulting centroids.
 *
 * For k0 = delta*q/2 size of a centroid cannot excede 2*n/delta and for a fully compacted digest
 * the number of centroids cannot exceed 2*delta (and has to be at least delta/2).
 */
void assertSameCentroids(const std::vector<Centroid>& expected, const TDigest& d) {
    const auto& cs = d.centroids();
    ASSERT_EQ(expected.size(), cs.size()) << "number of centroids in digest: " << d;
    for (size_t i = 0; i < cs.size(); ++i) {
        ASSERT_EQ(expected[i].weight, cs[i].weight)
            << "weight check: centroid " << cs[i] << " in digest: " << d;
        ASSERT_APPROX_EQUAL(expected[i].mean, cs[i].mean, epsilon)
            << "mean check: centroid " << cs[i] << " in digest: " << d;
    }
}

TEST(TDigestTest, IncorporateBatch_k0) {
    const int delta = 10;

    vector<double> batch1(21);  // one extra item to dodge the floating point precision issues
    std::iota(batch1.begin(), batch1.end(), 1.0);  // {1, 2, ..., 21}

    TDigest d{TDigest::k0_limit, delta};
    d.incorporate(batch1);
    d.flushBuffer();

    ASSERT_EQ(batch1.front(), d.min()) << "min of digest: " << d;
    ASSERT_EQ(batch1.back(), d.max()) << "max of digest: " << d;
    ASSERT_EQ(batch1.size(), d.n()) << "n of digest: " << d;

    assertSameCentroids({{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}}, d);
}

TEST(TDigestTest, Merge_TailData_k0) {
    const int delta = 10;

    TDigest d{0,                                                               // negInfCount
              0,                                                               // posInfCount
              1,                                                               // min
              21,                                                              // max
              {{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}},  // centroids
              TDigest::k0_limit,
              delta};
    const int64_t nOld = d.n();
    const double minOld = d.min();

    // Add to the digest data that is larger than any of the already accumulated points.
    vector<double> data(10);
    std::iota(data.begin(), data.end(), d.max() + 1);  // {22, 23, ..., 31}

    d.incorporate(data);
    d.flushBuffer();

    ASSERT_EQ(minOld, d.min()) << "min of digest: " << d;
    ASSERT_EQ(data.back(), d.max()) << "max of digest: " << d;
    ASSERT_EQ(nOld + data.size(), d.n()) << "n of digest: " << d;

    // Because the data in the second batch is sorted higher than the existing data, none of the
    // previously created centroids except the very last one can be merged (because the max weight
    // is 6 but the fully compacted centroids after the first batch all have size of 4).
    assertSameCentroids({{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {5, 19}, {6, 24.5}, {4, 29.5}},
                        d);
}

TEST(TDigestTest, Merge_HeadData_k0) {
    const int delta = 10;

    TDigest d{0,                                                               // negInfCount
              0,                                                               // posInfCount
              1,                                                               // min
              21,                                                              // max
              {{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}},  // centroids
              TDigest::k0_limit,
              delta};
    const int64_t nOld = d.n();
    const double maxOld = d.max();

    // Add to the digest data that is smaller than any of the already accumulated points.
    vector<double> data(10);
    std::iota(data.begin(), data.end(), d.min() - 10.0);  // {-9, -8, ..., 0}

    d.incorporate(data);
    d.flushBuffer();

    ASSERT_EQ(data.front(), d.min()) << "min after the second batch";
    ASSERT_EQ(maxOld, d.max()) << "max after the second batch";
    ASSERT_EQ(nOld + data.size(), d.n()) << "n after the second batch";

    // Because the data in the second batch is sorted lower than the existing data, none of the
    // previously created centroids can be merged (because the max weight is 6 but the fully
    // compacted centroids after the first batch all have size of 4).
    assertSameCentroids({{6, -6.5}, {4, -1.5}, {4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {5, 19}},
                        d);
}

TEST(TDigestTest, Merge_MixedData_k0) {
    const int delta = 10;

    TDigest d{0,                                                               // negInfCount
              0,                                                               // posInfCount
              1,                                                               // min
              21,                                                              // max
              {{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}},  // centroids
              TDigest::k0_limit,
              delta};
    const int64_t nOld = d.n();
    const double minOld = d.min();
    const double maxOld = d.max();

    // Use inputs that land between the current centroids of the digest. Note: more than 3
    // additional inputs would allow the centroids of bigger sizes.
    d.incorporate({4.5, 8.5, 12.5});
    d.flushBuffer();

    ASSERT_EQ(minOld, d.min()) << "min after the first batch";
    ASSERT_EQ(maxOld, d.max()) << "max after the first batch";
    ASSERT_EQ(nOld + 3, d.n()) << "n after the first batch";

    assertSameCentroids({{4, 2.5},
                         {1, 4.5},
                         {4, 6.5},
                         {1, 8.5},
                         {4, 10.5},
                         {1, 12.5},
                         {4, 14.5},
                         {4, 18.5},
                         {1, 21}},
                        d);

    // Incorporating more data would increase the bound on centroid size to 6 and cause compaction.
    d.incorporate({16.5, 22, 23, 24, 25, 26, 27});
    d.flushBuffer();
    ASSERT_EQ(31, d.n()) << "n after the second batch";
    assertSameCentroids({{5, 2.9}, {5, 6.9}, {5, 10.9}, {5, 14.9}, {5, 19}, {6, 24.5}}, d);
}

/**
 * The following tests checks merging of two digests. They use a simpler k0 = delta*q/2 scaling
 * function as it's easier to reason about the resulting centroids. Notice, that the results of
 * merging two digests aren't necessarily the same as when merging data into a digest.
 *
 * For k0 = delta*q/2 size of a centroid cannot exceed 2*n/delta and for a fully compacted digest
 * the number of centroids cannot exceed 2*delta (and has to be at least delta/2).
 */
TEST(TDigestTest, Merge_DigestsOrdered_k0) {
    const int delta = 10;

    TDigest d{0,                                                               // negInfCount
              0,                                                               // posInfCount
              1,                                                               // min
              21,                                                              // max
              {{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}},  // centroids
              TDigest::k0_limit,
              delta};
    const int64_t nOld = d.n();
    const double minOld = d.min();

    const TDigest other{
        0,                                                               // negInfCount
        0,                                                               // posInfCount
        22,                                                              // min
        31,                                                              // max
        {{2, 22.5}, {2, 24.5}, {1, 26}, {2, 27.5}, {2, 29.5}, {1, 31}},  // centroids
        TDigest::k0_limit,
        delta};

    d.merge(other);

    ASSERT_EQ(minOld, d.min()) << "min of digest: " << d;
    ASSERT_EQ(other.max(), d.max()) << "max of digest: " << d;
    ASSERT_EQ(nOld + other.n(), d.n()) << "n of digest: " << d;

    assertSameCentroids({{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {5, 19}, {5, 24}, {5, 29}}, d);
}

TEST(TDigestTest, Merge_DigestsMixed_k0) {
    const int delta = 10;

    TDigest d{0,                                                               // negInfCount
              0,                                                               // posInfCount
              1,                                                               // min
              21,                                                              // max
              {{4, 2.5}, {4, 6.5}, {4, 10.5}, {4, 14.5}, {4, 18.5}, {1, 21}},  // centroids
              TDigest::k0_limit,
              delta};
    const int64_t nOld = d.n();
    const double minOld = d.min();

    const TDigest other{0,    // negInfCount
                        0,    // posInfCount
                        4.5,  // min
                        27,   // max
                        {{2, 6.5}, {2, 14.5}, {1, 22}, {2, 23.5}, {2, 25.5}, {1, 27}},  // centroids
                        TDigest::k0_limit,
                        delta};

    d.merge(other);

    ASSERT_EQ(minOld, d.min()) << "min of digest: " << d;
    ASSERT_EQ(other.max(), d.max()) << "max of digest: " << d;
    ASSERT_EQ(nOld + other.n(), d.n()) << "n of digest: " << d;

    assertSameCentroids({{4, 2.5}, {6, 6.5}, {4, 10.5}, {6, 14.5}, {5, 19}, {6, 24.5}}, d);
}

/**
 * The following test doesn't add coverage but is meant to illustrate the difference between scaling
 * functions.
 */
TEST(TDigestTest, ScalingFunctionEffect) {
    const int delta = 20;

    vector<double> inputs(101);
    std::iota(inputs.begin(), inputs.end(), 1.0);  // {1, 2, ..., 101}

    TDigest d_k0{TDigest::k0_limit, delta};
    d_k0.incorporate(inputs);
    d_k0.flushBuffer();

    TDigest d_k1{TDigest::k1_limit, delta};
    d_k1.incorporate(inputs);
    d_k1.flushBuffer();

    TDigest d_k2{TDigest::k2_limit, delta};
    d_k2.incorporate(inputs);
    d_k2.flushBuffer();

    // k0 attempts to split data into centroids of equal weights.
    assertSameCentroids({{10, 5.5},
                         {10, 15.5},
                         {10, 25.5},
                         {10, 35.5},
                         {10, 45.5},
                         {10, 55.5},
                         {10, 65.5},
                         {10, 75.5},
                         {10, 85.5},
                         {10, 95.5},
                         {1, 101}},
                        d_k0);

    // k1 is biased to create smaller centroids at the extremes of the data.
    assertSameCentroids({{2, 1.5},
                         {6, 5.5},
                         {10, 13.5},
                         {13, 25},
                         {15, 39},
                         {15, 54},
                         {14, 68.5},
                         {12, 81.5},
                         {8, 91.5},
                         {5, 98},
                         {1, 101}},
                        d_k1);

    // k2 is also biased with the same asymptotic characteristics at 0 and 1 as k1 and is cheaper
    // to compute (but it has higher upper bound on the number of number of centroids).
    assertSameCentroids({{1, 1},    {1, 2},    {2, 3.5},  {3, 6},  {4, 9.5},  {5, 14},   {6, 19.5},
                         {7, 26},   {8, 33.5}, {9, 42},   {9, 51}, {9, 60},   {8, 68.5}, {7, 76},
                         {6, 82.5}, {5, 88},   {4, 92.5}, {3, 96}, {2, 98.5}, {1, 100},  {1, 101}},
                        d_k2);
}

/**
 * Until there are enough inputs, t-digest has to keep a centroid per input point, which means they
 * are strictly sorted and the computed percentiles are precise. However, the threshold on the
 * number of the inputs depends on the scaling function and delta.
 */
constexpr size_t dataSize = 100;
void runTestOnSmallDataset(TDigest& d) {
    vector<double> data(dataSize);
    std::iota(data.begin(), data.end(), 1.0);  // {1.0, ..., 100.0}

    d.incorporate(data);
    d.flushBuffer();

    ASSERT_EQ(data.size(), d.centroids().size()) << "number of centroids in " << d;
    // Spot-check a few percentiles.
    ASSERT_EQ(1, d.computePercentile(0.001).value()) << " digest: " << d;
    ASSERT_EQ(8, d.computePercentile(0.08).value()) << " digest: " << d;
    ASSERT_EQ(42, d.computePercentile(0.42).value()) << " digest: " << d;
    ASSERT_EQ(71, d.computePercentile(0.705).value()) << " digest: " << d;
    ASSERT_EQ(99, d.computePercentile(0.99).value()) << " digest: " << d;
    ASSERT_EQ(100, d.computePercentile(0.9999).value()) << " digest: " << d;

    // Check that asking for the same percentiles at once gives the same answers.
    vector<double> pctls = d.computePercentiles({0.001, 0.08, 0.42, 0.705, 0.99, 0.9999});
    ASSERT_EQ(1, pctls[0]) << "p:0.001 digest: " << d;
    ASSERT_EQ(8, pctls[1]) << "p:0.08 digest: " << d;
    ASSERT_EQ(42, pctls[2]) << "p:0.42 digest: " << d;
    ASSERT_EQ(71, pctls[3]) << "p:0.705 digest: " << d;
    ASSERT_EQ(99, pctls[4]) << "p:0.99 digest: " << d;
    ASSERT_EQ(100, pctls[5]) << "p:0.9999 digest: " << d;
}
TEST(TDigestTest, PreciseOnSmallDataset_k0) {
    TDigest d{TDigest::k0_limit, dataSize + 1 /* delta */};
    runTestOnSmallDataset(d);
}
TEST(TDigestTest, PreciseOnSmallDataset_k1) {
    TDigest d{TDigest::k1_limit, dataSize * 2 /* delta */};
    runTestOnSmallDataset(d);
}
TEST(TDigestTest, PreciseOnSmallDataset_k2) {
    TDigest d{TDigest::k2_limit, dataSize * 2 /* delta */};
    runTestOnSmallDataset(d);
}

/*==================================================================================================
  Tests with various data distributions.
==================================================================================================*/

// Generates n * dupes values using provided distribution. The dupes can be either kept together or
// spread across the whole generated dataset. NB: when data is fed to t-digest the algorithm fills
// and then sorts a buffer so spreading the duplicates within distances comparable to the
// buffer-size doesn't serve any purpose.
template <typename TDist>
vector<double> generateData(
    TDist& dist, size_t n, size_t dupes, bool keepDupesTogether, long seed) {
    if (seed == 0) {
        seed = time(nullptr);
    }
    LOGV2(7429513, "{seed}", "generateData", "seed"_attr = seed);
    std::mt19937 generator(seed);

    vector<double> inputs;
    inputs.reserve(n * dupes);

    for (size_t i = 0; i < n; ++i) {
        auto val = dist(generator);
        inputs.push_back(val);
        if (keepDupesTogether) {
            for (size_t j = 1; j < dupes; ++j) {
                inputs.push_back(val);
            }
        }
    }
    if (!keepDupesTogether && dupes > 1) {
        // duplicate 'inputs' the requested number of times.
        vector<double> temp = inputs;
        for (size_t j = 1; j < dupes; ++j) {
            std::shuffle(inputs.begin(), inputs.end(), generator);
            temp.insert(temp.end(), inputs.begin(), inputs.end());
        }
        inputs.swap(temp);
    }
    return inputs;
}

typedef vector<double> (*DataGenerator)(size_t, size_t, bool, long);

// Generates 'n' values in [0, 100] range with uniform distribution.
vector<double> generateUniform(size_t n, size_t dupes, bool keepDupesTogether, long seed) {
    boost::random::uniform_real_distribution<double> dist(0 /* min */, 100 /* max */);
    return generateData(dist, n, dupes, keepDupesTogether, seed);
}

// Generates 'n' values from normal distribution with mean = 0.0 and sigma = 0.5.
vector<double> generateNormal(size_t n, size_t dupes, bool keepDupesTogether, long seed) {
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 0.5 /* sigma */);
    return generateData(dist, n, dupes, keepDupesTogether, seed);
}

// Generates 'n' values from exponential distribution with lambda = 1.0: p(x)=lambda*e^(-lambda*x).
vector<double> generateExponential(size_t n, size_t dupes, bool keepDupesTogether, long seed) {
    boost::random::exponential_distribution<double> dist(1.0 /* lambda */);
    return generateData(dist, n, dupes, keepDupesTogether, seed);
}

// Generates 'n' values from weibull distribution with a : 1.0, b: 0.5 to produce a heavy tail.
vector<double> generateWeibull(size_t n, size_t dupes, bool keepDupesTogether, long seed) {
    boost::random::weibull_distribution<double> dist(1.0 /* a */, 0.5 /* b */);
    return generateData(dist, n, dupes, keepDupesTogether, seed);
}

/*
 * The following tests generate datasets with 10,000 values. T-digest does not guarantee error
 * bounds, but on well-behaved datasets it should be within 0.5% for the middle percentiles and even
 * better for the extreme ones.
 *
 * These tests also indirectly validate the merging and compacting with a more complex scaling
 * function.
 */

void runTestWithDataGenerator(TDigest::ScalingFunction k_limit,
                              TDigest::ScalingFunction k,
                              DataGenerator dg,
                              const vector<double>& percentiles,
                              double accuracy,
                              const char* msg) {
    vector<double> inputs =
        dg(100'000 /* nUnique */, 1 /* dupes */, true /* keepDupesTogether*/, 0 /*seed*/);

    const int delta = 500;
    TDigest digest(k_limit, delta);
    for (auto val : inputs) {
        digest.incorporate(val);
    }
    digest.flushBuffer();
    assertIsValid(digest, k, delta, msg);

    ASSERT_EQ(inputs.size(), digest.n());
    ASSERT_LTE(digest.centroids().size(), 2 * (k(1, delta) - k(0, delta)))
        << "Upper bound on the number of centroids";

    std::sort(inputs.begin(), inputs.end());
    assertExpectedAccuracy(inputs, digest, percentiles, accuracy, msg);
}

TEST(TDigestTest, UniformDistribution_Mid) {
    const vector<double> percentiles = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateUniform,
                             percentiles,
                             0.005 /* accuracy */,
                             "Uniform distribution mid");
}
TEST(TDigestTest, UniformDistribution_Extr) {
    const vector<double> percentiles = {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateUniform,
                             percentiles,
                             0.0005 /* accuracy */,
                             "Uniform distribution extr");
}

TEST(TDigestTest, NormalDistribution_Mid) {
    const vector<double> percentiles = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateNormal,
                             percentiles,
                             0.005 /* accuracy */,
                             "Normal distribution mid");
}
TEST(TDigestTest, NormalDistribution_Extr) {
    const vector<double> percentiles = {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateNormal,
                             percentiles,
                             0.0005 /* accuracy */,
                             "Normal distribution extr");
}

TEST(TDigestTest, ExponentialDistribution_Mid) {
    const vector<double> percentiles = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateExponential,
                             percentiles,
                             0.005 /* accuracy */,
                             "Exponential distribution mid");
}
TEST(TDigestTest, ExponentialDistribution_Extr) {
    const vector<double> percentiles = {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateExponential,
                             percentiles,
                             0.0005 /* accuracy */,
                             "Exponential distribution extr");
}

TEST(TDigestTest, WeibullDistribution_Mid) {
    const vector<double> percentiles = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateWeibull,
                             percentiles,
                             0.005 /* accuracy */,
                             "Exponential distribution mid");
}
TEST(TDigestTest, WeibullDistribution_Extr) {
    const vector<double> percentiles = {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999};
    runTestWithDataGenerator(TDigest::k2_limit,
                             TDigest::k2,
                             generateWeibull,
                             percentiles,
                             0.0005 /* accuracy */,
                             "Exponential distribution extr");
}

/**
 * Tests distributions with duplicated data. Notice that we tend to get lower accuracy in presence
 * of many duplicates.
 */
void runTestWithDuplicatesInData(DataGenerator dg,
                                 size_t n,
                                 size_t dupes,
                                 bool keepDupesTogether,
                                 const vector<double>& percentiles,
                                 double accuracy,
                                 const char* msg) {
    const int delta = 500;
    vector<double> inputs = dg(n, dupes, keepDupesTogether, 0 /*seed*/);

    TDigest digest(TDigest::k2_limit, delta);
    for (auto val : inputs) {
        digest.incorporate(val);
    }
    digest.flushBuffer();
    assertIsValid(digest, TDigest::k2, delta, msg);

    ASSERT_EQ(inputs.size(), digest.n());
    ASSERT_LTE(digest.centroids().size(), 2 * (TDigest::k2(1, delta) - TDigest::k2(0, delta)))
        << "Upper bound on the number of centroids";

    std::sort(inputs.begin(), inputs.end());
    assertExpectedAccuracy(inputs, digest, percentiles, accuracy, msg);
}

TEST(TDigestTest, Duplicates_uniform_mid) {
    const vector<double> percentiles = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};

    runTestWithDuplicatesInData(generateUniform,
                                10'000 /* nUnique*/,
                                10 /* dupes */,
                                false /* keepDupesTogether*/,
                                percentiles,
                                0.005 /* accuracy */,
                                "Uniform distribution with shuffled dupes mid (10'000x10)");
    runTestWithDuplicatesInData(generateUniform,
                                10'000 /* nUnique*/,
                                10 /* dupes */,
                                true /* keepDupesTogether*/,
                                percentiles,
                                0.005 /* accuracy */,
                                "Uniform distribution with clustered dupes mid (10'000x10)");

    runTestWithDuplicatesInData(generateUniform,
                                1000 /* nUnique*/,
                                100 /* dupes */,
                                false /* keepDupesTogether*/,
                                percentiles,
                                0.01 /* accuracy */,
                                "Uniform distribution with shuffled dupes mid (1000x100)");
    runTestWithDuplicatesInData(generateUniform,
                                1000 /* nUnique*/,
                                100 /* dupes */,
                                true /* keepDupesTogether*/,
                                percentiles,
                                0.01 /* accuracy */,
                                "Uniform distribution with clustered dupes mid (1000x100)");
}

TEST(TDigestTest, Duplicates_uniform_extr) {
    const vector<double> percentiles = {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999};

    runTestWithDuplicatesInData(generateUniform,
                                10'000 /* nUnique*/,
                                10 /* dupes */,
                                false /* keepDupesTogether*/,
                                percentiles,
                                0.0005 /* accuracy */,
                                "Uniform distribution with shuffled dupes extr (10'000x10)");
    runTestWithDuplicatesInData(generateUniform,
                                10'000 /* nUnique*/,
                                10 /* dupes */,
                                true /* keepDupesTogether*/,
                                percentiles,
                                0.0005 /* accuracy */,
                                "Uniform distribution with clustered dupes extr (10'000x10)");

    runTestWithDuplicatesInData(generateUniform,
                                1000 /* nUnique*/,
                                100 /* dupes */,
                                false /* keepDupesTogether*/,
                                percentiles,
                                0.005 /* accuracy */,
                                "Uniform distribution with shuffled dupes extr (1000x100)");
    runTestWithDuplicatesInData(generateUniform,
                                1000 /* nUnique*/,
                                100 /* dupes */,
                                true /* keepDupesTogether*/,
                                percentiles,
                                0.005 /* accuracy */,
                                "Uniform distribution with clustered dupes extr (1000x100)");
}

TEST(TDigestTest, Duplicates_all) {
    const int delta = 100;
    const vector<double> inputs(10'000, 42);

    TDigest digest(TDigest::k2_limit, delta);
    for (auto val : inputs) {
        digest.incorporate(val);
    }
    digest.flushBuffer();
    assertIsValid(digest, TDigest::k2, delta, "All duplicates");

    // The t-digest of all duplicates will still contain multiple centroids but because all of them
    // have the same mean that is equal to min and max, the interpolation should always return that
    // mean as a result and, thus, would produce accurate percentile.
    assertExpectedAccuracy(
        inputs,
        digest,
        {0.0001, 0.001, 0.01, 0.1, 0.2, 0.5, 0.8, 0.9, 0.99, 0.999, 0.9999} /* percentiles */,
        0.0 /* accuracy */,
        "All duplicates mid");
}

TEST(TDigestTest, Duplicates_two_clusters) {
    const int delta = 100;
    vector<double> sorted(10'000, 0);
    for (size_t i = 0; i < 0.8 * sorted.size(); i++) {
        sorted[i] = 17;
    }
    for (size_t i = 0.8 * sorted.size(); i < sorted.size(); i++) {
        sorted[i] = 42;
    }
    vector<double> inputs = sorted;
    auto seed = time(nullptr);
    LOGV2(7429514, "{seed}", "Duplicates_two_clusters", "seed"_attr = seed);
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(seed));

    TDigest digest(TDigest::k2_limit, delta);
    for (auto val : inputs) {
        digest.incorporate(val);
    }
    digest.flushBuffer();
    assertIsValid(digest, TDigest::k2, delta, "Duplicates_two_clusters");
    assertExpectedAccuracy(sorted,
                           digest,
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9} /* percentiles */,
                           0.01 /* accuracy */,
                           "Duplicates two clusters mid");
    assertExpectedAccuracy(sorted,
                           digest,
                           {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999} /* percentiles */,
                           0.005 /* accuracy */,
                           "Duplicates two clusters extr");
}

TEST(TDigestTest, Frankenstein_distribution) {
    vector<vector<double>> chunks = {
        generateNormal(10000, 1, true, 0),       // 10000
        generateNormal(2000, 5, true, 0),        // 10000
        generateNormal(1000, 10, true, 0),       // 10000
        generateNormal(500, 20, true, 0),        // 10000
        generateNormal(250, 40, true, 0),        // 10000
        generateUniform(20000, 1, true, 0),      // 30000
        generateUniform(100, 100, true, 0),      // 10000
        generateUniform(50, 200, true, 0),       // 10000
        generateExponential(10000, 1, true, 0),  // 10000
    };
    vector<double> inputs;
    inputs.reserve(100'000);
    for (const auto& chunk : chunks) {
        inputs.insert(inputs.end(), chunk.begin(), chunk.end());
    }
    std::shuffle(inputs.begin(), inputs.end(), std::mt19937(2023 /*seed*/));

    const int delta = 500;
    TDigest digest(TDigest::k2_limit, delta);
    for (auto val : inputs) {
        digest.incorporate(val);
    }
    digest.flushBuffer();
    assertIsValid(digest, TDigest::k2, delta, "Frankenstein distribution");

    ASSERT_EQ(inputs.size(), digest.n());

    // For k2 the upper bound on the number of centroids is 2*delta
    ASSERT_LTE(digest.centroids().size(), 2 * delta) << "Upper bound on the number of centroids";

    std::sort(inputs.begin(), inputs.end());
    assertExpectedAccuracy(inputs,
                           digest,
                           {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9},
                           0.01,
                           "Frankenstein distribution mid");
    assertExpectedAccuracy(inputs,
                           digest,
                           {0.0001, 0.001, 0.01, 0.99, 0.999, 0.9999},
                           0.005,
                           "Frankenstein distribution extr");
}

/**
 * The tests below were used to assess accuracy of t-digest on datasets of large size (>=1e7) over
 * multiple iterations. They never fail but we'd like to keep the code alive to be able to repeat
 * the experiments in the future if we decide to tune t-digest further. However, for running as part
 * of unit tests the number of iterations and the dataset size have been set to lower values.
 */
vector<int> computeError(vector<double> sorted,
                         TDigest& digest,
                         const vector<double>& percentiles) {
    vector<int> errors;
    errors.reserve(percentiles.size());
    for (double p : percentiles) {
        const double pctl = digest.computePercentile(p).value();
        errors.push_back(std::abs(computeRank(sorted, pctl) -
                                  PercentileAlgorithm::computeTrueRank(sorted.size(), p)));
    }
    return errors;
}

constexpr int nIterations = 1;
constexpr int nUnique = 100'000;
std::pair<vector<vector<int>> /*errors*/, vector<int> /*# centroids*/> generateAccuracyStats(
    DataGenerator dg,
    TDigest::ScalingFunction k_limit,
    const vector<double>& deltas,
    const vector<double>& percentiles) {
    vector<vector<int>> errors(deltas.size(), vector<int>(percentiles.size(), 0));
    vector<int> n_centroids(deltas.size(), 0);

    long seed = time(nullptr);
    for (int i = 0; i < nIterations; ++i) {
        std::cout << "*** iteration " << i << std::endl;
        vector<double> data = dg(nUnique, 1 /* dupes */, false /* keepDupesTogether */, ++seed);
        vector<double> sorted = data;
        std::sort(sorted.begin(), sorted.end());

        for (size_t di = 0; di < deltas.size(); di++) {
            TDigest digest(k_limit, deltas[di]);
            for (auto val : data) {
                digest.incorporate(val);
            }
            digest.flushBuffer();
            assertIsValid(digest, TDigest::k2, deltas[di], std::to_string(deltas[di]));

            auto errors_single_iter = computeError(sorted, digest, percentiles);
            for (size_t ei = 0; ei < errors_single_iter.size(); ei++) {
                errors[di][ei] += errors_single_iter[ei];
            }

            // For one of the iterations let's assess the amount of overlap between the centroids.
            // Two centroids overlap if the max of the left one is greater than the min of the right
            // one. From the empirical review it seems unlikely for a centroid to overlap with more
            // than 20 others, so we'll limit the loop to that.
            const auto& cs = digest.centroids();
            n_centroids[di] += cs.size();
        }
    }
    return std::make_pair(errors, n_centroids);
}

void runAccuracyTest(DataGenerator dg) {
    const vector<double> deltas = {
        100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000, 6000, 7000};
    const vector<double> percentiles = {
        0.00001, 0.0001, 0.001, 0.01, 0.1, 0.5, 0.9, 0.99, 0.999, 0.9999, 0.99999};

    const auto& [abs_errors, n_centroids] =
        generateAccuracyStats(dg, TDigest::k2_limit, deltas, percentiles);

    for (double p : percentiles)
        std::cout << p << ",";
    std::cout << "# centroids" << std::endl;
    for (size_t di = 0; di < deltas.size(); di++) {
        std::cout << deltas[di] << ",";
        for (size_t ei = 0; ei < percentiles.size(); ei++) {
            std::cout << abs_errors[di][ei] / static_cast<double>(nIterations) << ",";
        }
        std::cout << n_centroids[di] / static_cast<double>(nIterations) << std::endl;
    }
}

// TEST(TDigestTest, AccuracyStats_uniform) {
//     runAccuracyTest(generateUniform);
// }
// TEST(TDigestTest, AccuracyStats_normal) {
//     runAccuracyTest(generateNormal);
// }
// TEST(TDigestTest, AccuracyStats_exp) {
//     runAccuracyTest(generateExponential);
// }
// TEST(TDigestTest, AccuracyStats_weibull) {
//     runAccuracyTest(generateWeibull);
// }
}  // namespace
}  // namespace mongo
