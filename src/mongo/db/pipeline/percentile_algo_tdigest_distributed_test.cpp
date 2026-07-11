// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/percentile_algo.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <ostream>
#include <random>
#include <vector>

#include <boost/random/normal_distribution.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using std::vector;

vector<double> generateData(size_t n) {
    auto seed = 1680031021;  // arbitrary
    LOGV2(7492710, "generateData", "seed"_attr = seed);
    std::mt19937 generator(seed);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 0.5 /* sigma */);

    vector<double> inputs;
    inputs.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        inputs.push_back(dist(generator));
    }
    return inputs;
}

// Yes, a binary search could be faster, but it's not worth doing it here.
int computeRank(const vector<double>& sorted, double value) {
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (value <= sorted[i]) {
            return i;
        }
    }
    ASSERT(false) << value << " is larger than the max input " << sorted.back();
    return -1;
}

TEST(TDigestTestParallel, CombineEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    dynamic_cast<PartialPercentile<Value>*>(empty.get())
        ->combine(dynamic_cast<PartialPercentile<Value>*>(empty.get())->serialize());

    ASSERT(!empty->computePercentile(0.5).has_value())
        << "Combining two empty digests should produce an empty digest";
}

TEST(TDigestTestParallel, CombineWithEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> digest = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (double val : inputs) {
        digest->incorporate(val);
    }
    const double median = *(digest->computePercentile(0.5));

    auto emptyAsPartial = dynamic_cast<PartialPercentile<Value>*>(empty.get());
    auto digestAsPartial = dynamic_cast<PartialPercentile<Value>*>(digest.get());
    digestAsPartial->combine(emptyAsPartial->serialize());

    ASSERT_EQ(median, *(digest->computePercentile(0.5)))
        << "Combining with empty digest should not change the median";
}

TEST(TDigestTestParallel, CombineIntoEmpty) {
    std::unique_ptr<PercentileAlgorithm> empty = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> digest = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (double val : inputs) {
        digest->incorporate(val);
    }
    const double median = *(digest->computePercentile(0.5));

    auto emptyAsPartial = dynamic_cast<PartialPercentile<Value>*>(empty.get());
    auto digestAsPartial = dynamic_cast<PartialPercentile<Value>*>(digest.get());
    emptyAsPartial->combine(digestAsPartial->serialize());

    ASSERT_EQ(median, *(digest->computePercentile(0.5)))
        << "Combining into empty digest should not change the median";
}

TEST(TDigestTestParallel, CombineTwoNonEmpty) {
    std::unique_ptr<PercentileAlgorithm> d1 = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> d2 = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (size_t i = 0; i < inputs.size() / 2; i++) {
        d1->incorporate(inputs[i]);
    }
    for (size_t i = inputs.size() / 2; i < inputs.size(); ++i) {
        d2->incorporate(inputs[i]);
    }

    auto partial1 = dynamic_cast<PartialPercentile<Value>*>(d1.get());
    auto partial2 = dynamic_cast<PartialPercentile<Value>*>(d2.get());
    partial1->combine(partial2->serialize());

    std::sort(inputs.begin(), inputs.end());

    // Min and max are computed precisely
    ASSERT_EQ(inputs.front(), *(d1->computePercentile(0.0))) << "Min after combining";
    ASSERT_EQ(inputs.back(), *(d1->computePercentile(1.0))) << "Max after combining";

    const double median = *(d1->computePercentile(0.5));
    const int rank = computeRank(inputs, median);
    ASSERT_APPROX_EQUAL(0.5 * inputs.size(), rank, 100) << "Median after combining: " << median;
}

TEST(TDigestTestParallel, CombineInfinities) {
    const double inf = std::numeric_limits<double>::infinity();

    std::unique_ptr<PercentileAlgorithm> d1 = createTDigestDistributedClassic();
    std::unique_ptr<PercentileAlgorithm> d2 = createTDigestDistributedClassic();
    vector<double> inputs = generateData(10'000);
    for (size_t i = 0; i < inputs.size() / 2; i++) {
        d1->incorporate(inputs[i]);
    }
    d1->incorporate(inf);
    for (size_t i = inputs.size() / 2; i < inputs.size(); ++i) {
        d2->incorporate(inputs[i]);
    }
    d2->incorporate(-inf);

    auto partial1 = dynamic_cast<PartialPercentile<Value>*>(d1.get());
    auto partial2 = dynamic_cast<PartialPercentile<Value>*>(d2.get());
    partial1->combine(partial2->serialize());

    std::sort(inputs.begin(), inputs.end());

    // Min and max are computed precisely
    ASSERT_EQ(-inf, *(d1->computePercentile(0.0))) << "Min after combining";
    ASSERT_EQ(inf, *(d1->computePercentile(1.0))) << "Max after combining";

    const double median = *(d1->computePercentile(0.5));
    const int rank = computeRank(inputs, median);
    ASSERT_APPROX_EQUAL(0.5 * inputs.size(), rank, 100) << "Median after combining: " << median;
}

}  // namespace
}  // namespace mongo
